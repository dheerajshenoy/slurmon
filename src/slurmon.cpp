#include "slurmon.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <thread>

template <typename T>
void
load_config_field(const toml::table &parent, std::string_view section,
                  std::string_view key, T &out)
{
    if (auto table = parent[section].as_table())
    {
        if (auto value = table->get_as<T>(key))
            out = value->get();
    }
}

// Case-insensitive substring search, allocation-free.
static bool
icontains(const std::string &haystack, const std::string &needle_lower)
{
    if (needle_lower.empty())
        return true;
    if (haystack.size() < needle_lower.size())
        return false;
    const size_t stop = haystack.size() - needle_lower.size();
    for (size_t i = 0; i <= stop; ++i)
    {
        size_t k = 0;
        for (; k < needle_lower.size(); ++k)
        {
            char c = static_cast<char>(
                std::tolower(static_cast<unsigned char>(haystack[i + k])));
            if (c != needle_lower[k])
                break;
        }
        if (k == needle_lower.size())
            return true;
    }
    return false;
}

static bool
matches_query(const Job &j, const std::string &q_lower)
{
    if (q_lower.empty())
        return true;
    return icontains(j.id(), q_lower) || icontains(j.name(), q_lower)
           || icontains(j.state(), q_lower) || icontains(j.time(), q_lower);
}

// Filter jobs by search query, returning borrowed pointers into `jobs`
// (no Job copies).
static std::vector<const Job *>
filter_jobs(const std::vector<Job> &jobs, const std::string &query)
{
    std::vector<const Job *> out;
    out.reserve(jobs.size());
    if (query.empty())
    {
        for (const auto &j : jobs)
            out.push_back(&j);
        return out;
    }
    std::string q;
    q.reserve(query.size());
    for (char c : query)
        q.push_back(
            static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    for (const auto &j : jobs)
        if (matches_query(j, q))
            out.push_back(&j);
    return out;
}

// Forward-declared here; defined near cancel_job.
static std::vector<std::string>
expand_array_ids(const std::string &id);

// Convert SLURM elapsed strings ("D-HH:MM:SS", "HH:MM:SS", "MM:SS",
// "SS", or "INVALID"/"UNLIMITED"/"NOT_SET") to seconds. Non-parseable
// values yield -1 so they sort together at one end.
static long long
time_to_seconds(const std::string &s)
{
    if (s.empty())
        return -1;
    long long days = 0;
    std::string rest = s;
    auto dash        = rest.find('-');
    if (dash != std::string::npos)
    {
        try
        {
            days = std::stoll(rest.substr(0, dash));
        }
        catch (...)
        {
            return -1;
        }
        rest = rest.substr(dash + 1);
    }
    std::vector<long long> parts;
    std::string cur;
    for (char c : rest)
    {
        if (c == ':')
        {
            if (cur.empty())
                return -1;
            try
            {
                parts.push_back(std::stoll(cur));
            }
            catch (...)
            {
                return -1;
            }
            cur.clear();
        }
        else if (std::isdigit(static_cast<unsigned char>(c)))
        {
            cur.push_back(c);
        }
        else
        {
            return -1;
        }
    }
    if (!cur.empty())
    {
        try
        {
            parts.push_back(std::stoll(cur));
        }
        catch (...)
        {
            return -1;
        }
    }
    long long h = 0, m = 0, sec = 0;
    if (parts.size() == 3)
    {
        h   = parts[0];
        m   = parts[1];
        sec = parts[2];
    }
    else if (parts.size() == 2)
    {
        m   = parts[0];
        sec = parts[1];
    }
    else if (parts.size() == 1)
    {
        sec = parts[0];
    }
    else
    {
        return -1;
    }
    return days * 86400 + h * 3600 + m * 60 + sec;
}

static void
sort_jobs(std::vector<const Job *> &jobs, Slurmon::SortKey key,
          bool descending)
{
    if (key == Slurmon::SortKey::None || jobs.size() < 2)
        return;

    // Precompute sort keys once (Schwartzian) — time parsing and
    // numeric-prefix parsing are the expensive bits.
    struct Keyed
    {
        const Job *job;
        long long num = 0;
        const std::string *str = nullptr;
    };
    std::vector<Keyed> tagged;
    tagged.reserve(jobs.size());
    for (const Job *j : jobs)
    {
        Keyed k{j};
        switch (key)
        {
        case Slurmon::SortKey::Id:
        {
            try
            {
                k.num = std::stoll(j->id());
            }
            catch (...)
            {
                k.num = -1;
            }
            k.str = &j->id();
            break;
        }
        case Slurmon::SortKey::Name:  k.str = &j->name();  break;
        case Slurmon::SortKey::State: k.str = &j->state(); break;
        case Slurmon::SortKey::Time:  k.num = time_to_seconds(j->time()); break;
        default: break;
        }
        tagged.push_back(k);
    }

    auto cmp = [key](const Keyed &a, const Keyed &b)
    {
        if (key == Slurmon::SortKey::Id)
        {
            if (a.num >= 0 && b.num >= 0 && a.num != b.num)
                return a.num < b.num;
            return *a.str < *b.str;
        }
        if (key == Slurmon::SortKey::Time)
            return a.num < b.num;
        return *a.str < *b.str;
    };
    std::stable_sort(tagged.begin(), tagged.end(),
                     [&](const Keyed &a, const Keyed &b)
                     { return descending ? cmp(b, a) : cmp(a, b); });
    for (size_t i = 0; i < jobs.size(); ++i)
        jobs[i] = tagged[i].job;
}

static const char *
sort_key_label(Slurmon::SortKey k)
{
    switch (k)
    {
    case Slurmon::SortKey::Id:    return "id";
    case Slurmon::SortKey::Name:  return "name";
    case Slurmon::SortKey::State: return "state";
    case Slurmon::SortKey::Time:  return "time";
    default:                      return "none";
    }
}

// Return a color decorator based on the job state
static ftxui::Decorator
state_color(const std::string &state)
{
    using namespace ftxui;
    if (state == "RUNNING")
        return color(Color::Green);
    if (state == "PENDING")
        return color(Color::Yellow);
    if (state == "COMPLETED")
        return color(Color::Blue);
    if (state == "COMPLETING")
        return color(Color::Cyan);
    if (state == "SUSPENDED")
        return color(Color::Magenta);
    if (state == "FAILED" || state == "NODE_FAIL" || state == "BOOT_FAIL"
        || state == "OUT_OF_MEMORY" || state == "DEADLINE")
        return color(Color::Red);
    if (state == "CANCELLED" || state == "TIMEOUT" || state == "PREEMPTED")
        return color(Color::RedLight);
    return color(Color::Default);
}

// Constructor for the Slurmon class
Slurmon::Slurmon(int argc, char **argv) : m_selected_row(-1)
{
    init_args();
    try
    {
        m_argparse.parse_args(argc, argv);
    }
    catch (const std::exception &e)
    {
        std::cerr << e.what() << std::endl;
        std::exit(1);
    }
    parse_args();
    init_config();
}

// Destructor for the Slurmon class
Slurmon::~Slurmon()
{
    m_running.store(false, std::memory_order_relaxed);
}

// Run the main loop of the application
void
Slurmon::run()
{
    init_ui();
}

// Initialize command-line arguments
void
Slurmon::init_args() noexcept
{
    m_argparse.add_argument("-c", "--config")
        .nargs(1)
        .help("Path to the configuration file (TOML format)");

    m_argparse.add_argument("-v", "--version")
        .nargs(0)
        .help("Print version information and exit");

    m_argparse.add_argument("--squeue-args")
        .nargs(1)
        .help("Extra arguments appended to the squeue command "
              "(e.g. --squeue-args=\"-u $USER -p gpu\")");

    m_argparse.add_argument("--sacct-args")
        .nargs(1)
        .help("Extra arguments appended to the sacct command "
              "(e.g. --sacct-args=\"-S 2024-01-01 -u $USER\")");

    m_argparse.add_argument("--scancel-args")
        .nargs(1)
        .help("Extra arguments appended to the scancel command "
              "(e.g. --scancel-args=\"--signal=TERM\")");
}

// Parse command-line arguments
void
Slurmon::parse_args() noexcept
{
    if (m_argparse.is_used("--config"))
    {
        m_config_path = m_argparse.get<std::string>("--config");
    }

    if (m_argparse.is_used("--version"))
    {
        std::cout << "slurmon " << SLURMON_VERSION << std::endl;
        std::exit(0);
    }

    if (m_argparse.is_used("--squeue-args"))
        m_squeue_args = m_argparse.get<std::string>("--squeue-args");
    if (m_argparse.is_used("--sacct-args"))
        m_sacct_args = m_argparse.get<std::string>("--sacct-args");
    if (m_argparse.is_used("--scancel-args"))
        m_scancel_args = m_argparse.get<std::string>("--scancel-args");
}

// Initialize the user interface using FTXUI
void
Slurmon::init_ui() noexcept
{
    using namespace ftxui;

    auto screen = ScreenInteractive::Fullscreen();

    if (m_split_size < 0)
    {
        auto dim     = Terminal::Size();
        m_split_size = std::max(20, dim.dimx / 2);
    }

    auto fetch_current = [this]
    {
        return m_view_mode == ViewMode::History ? fetch_history_jobs()
                                                : fetch_jobs();
    };

    auto view_of = [this](const std::vector<Job> &src)
                       -> const std::vector<const Job *> &
    {
        if (m_view_dirty)
        {
            m_view_cache = filter_jobs(src, m_search_query);
            sort_jobs(m_view_cache, m_sort_key, m_sort_descending);
            m_view_dirty = false;
        }
        return m_view_cache;
    };

    {
        auto initial = fetch_current();
        std::lock_guard<std::mutex> lk(m_jobs_mutex);
        m_jobs       = std::move(initial);
        m_view_dirty = true;
    }

    std::thread ticker([&]
    {
        while (m_running.load(std::memory_order_relaxed))
        {
            int slices = std::max(1, m_config.job_view.refresh_interval) * 10;
            for (int i = 0;
                 i < slices && m_running.load(std::memory_order_relaxed); ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));

            if (!m_running.load(std::memory_order_relaxed))
                break;

            auto fresh = fetch_current();
            {
                std::lock_guard<std::mutex> lk(m_jobs_mutex);
                m_jobs       = std::move(fresh);
                m_view_dirty = true;
                auto vsz     = static_cast<int>(view_of(m_jobs).size());
                if (m_selected_row >= vsz)
                    m_selected_row = vsz - 1;
                if (m_selected_row < 0 && vsz > 0)
                    m_selected_row = 0;
            }
            screen.PostEvent(Event::Custom);
        }
    });

    auto left_renderer = Renderer([&]
    {
        std::lock_guard<std::mutex> lk(m_jobs_mutex);
        const auto &view = view_of(m_jobs);
        if (m_selected_row == -1 && !view.empty())
            m_selected_row = 0;
        if (m_selected_row >= static_cast<int>(view.size()))
            m_selected_row
                = view.empty() ? -1 : static_cast<int>(view.size()) - 1;

        std::string title = m_view_mode == ViewMode::History
                                ? " Jobs (history) "
                                : " Jobs ";
        if (m_sort_key != SortKey::None)
        {
            title += "[sort: ";
            title += sort_key_label(m_sort_key);
            title += m_sort_descending ? " ▼] " : " ▲] ";
        }
        return window(text(title) | bold, vbox(build_rows(view)));
    });

    auto right_renderer = Renderer([&]
    {
        std::lock_guard<std::mutex> lk(m_jobs_mutex);
        const auto &view = view_of(m_jobs);

        Element details;
        if (m_selected_row >= 0
            && m_selected_row < static_cast<int>(view.size()))
        {
            const Job &j = *view[m_selected_row];
            auto field   = [](const std::string &label, Element val)
            {
                return hbox({
                    text(label) | bold | size(WIDTH, EQUAL, 10),
                    std::move(val),
                });
            };
            Elements rows;
            if (m_config.detail_view.show_id)
                rows.push_back(field("ID:", text(j.id())));
            if (m_config.detail_view.show_name)
                rows.push_back(field("Name:", text(j.name())));
            if (m_config.detail_view.show_state)
                rows.push_back(field(
                    "State:",
                    text(j.state()) | state_color(j.state()) | bold));
            if (m_config.detail_view.show_user)
                rows.push_back(field("User:", text(j.user())));
            if (m_config.detail_view.show_time)
                rows.push_back(field("Time:", text(j.time())));
            if (m_config.detail_view.show_nodes)
                rows.push_back(field("Nodes:", text(j.nodes())));
            if (m_config.detail_view.show_nodelist)
                rows.push_back(
                    field("Nodelist:", text(j.nodelist_or_reason())));
            details = rows.empty() ? filler() : vbox(std::move(rows));
        }
        else
        {
            details = text("select a job to see details") | dim;
        }

        Element log_content;
        std::string log_title
            = m_show_stderr ? " Logs (stderr) " : " Logs (stdout) ";
        if (m_selected_row >= 0
            && m_selected_row < static_cast<int>(view.size()))
        {
            const Job &j      = *view[m_selected_row];
            const auto &paths = log_paths_for(j.id());
            const std::string &path
                = m_show_stderr ? paths.stderr_path : paths.stdout_path;

            if (path.empty())
            {
                log_content = text("no log path reported for this job") | dim;
            }
            else
            {
                auto tail = read_tail(path, 500);
                if (tail.empty())
                {
                    log_content = vbox({
                        text(path) | dim,
                        text("(empty or unreadable)") | dim,
                    });
                }
                else
                {
                    Elements lines;
                    std::istringstream ss(tail);
                    std::string line;
                    while (std::getline(ss, line))
                    {
                        auto cr = line.rfind('\r');
                        if (cr != std::string::npos)
                            line.erase(0, cr + 1);
                        lines.push_back(text(line));
                    }
                    log_content = vbox(std::move(lines))
                                  | focusPositionRelative(0, 1) | frame;
                }
            }
        }
        else
        {
            log_content = text("select a job to see logs") | dim;
        }

        Elements panes;
        if (m_config.detail_view.show)
            panes.push_back(window(text(" Details ") | bold, details));
        if (m_config.log_view.show)
            panes.push_back(window(text(log_title) | bold, log_content) | flex);
        if (panes.empty())
            panes.push_back(filler());
        return vbox(std::move(panes));
    });

    bool right_visible
        = m_config.detail_view.show || m_config.log_view.show;
    bool left_visible = m_config.job_view.show;

    Component content_component;
    if (left_visible && right_visible)
        content_component
            = ResizableSplitLeft(left_renderer, right_renderer, &m_split_size);
    else if (right_visible)
        content_component = Container::Vertical({right_renderer});
    else if (left_visible)
        content_component = Container::Vertical({left_renderer});
    else
        content_component = Renderer([] {
            return text("All views hidden, nothing to see here") | dim | center
                   | flex;
        });

    auto renderer = Renderer(content_component, [&]
    {
        Elements root = {content_component->Render() | flex};
        if (m_search_mode || !m_search_query.empty())
        {
            std::string prefix = m_search_mode ? "/" : "filter: ";
            std::string q = m_search_mode ? m_search_buffer : m_search_query;
            auto bar      = hbox({text(prefix) | bold, text(q),
                                  text(m_search_mode ? "_" : "") | blink})
                            | (m_search_mode ? color(Color::Yellow)
                                             : color(Color::Default));
            root.push_back(bar);
        }
        if (m_show_footer)
        {
            root.push_back(
                text("/ search, s sort, t history, ? help, F1 footer, q quit") | dim
                | center);
        }
        Element page = vbox(std::move(root));

        if (m_show_help_dialog)
        {
            auto binding = [](const std::string &keys, const std::string &desc)
            {
                return hbox({
                    text(keys) | bold | size(WIDTH, EQUAL, 14),
                    text(desc),
                });
            };
            auto help
                = window(text(" Help ") | bold,
                         vbox({
                             binding("j / k", "move selection down / up"),
                             binding("gg", "jump to first job"),
                             binding("G", "jump to last job"),
                             binding("e", "toggle stdout / stderr log"),
                             binding("c", "cancel selected job / array range"),
                             binding("C", "cancel parent job (array root)"),
                             binding("/", "search (id/name/state/time)"),
                             binding("t", "toggle live / history view"),
                             binding("s", "cycle sort (none→id→name→state→time)"),
                             binding("S", "toggle sort direction"),
                             binding("Esc", "clear active filter"),
                             binding("?", "toggle this help"),
                             binding("F1", "toggle footer"),
                             binding("q", "quit"),
                             text(""),
                             text("mouse: drag the vertical split") | dim,
                             text("press any key to close") | dim | center,
                         }))
                  | size(WIDTH, GREATER_THAN, 44) | clear_under | center;
            page = dbox({page, help});
        }

        if (m_show_cancel_dialog)
        {
            auto expanded = expand_array_ids(m_cancel_target_id);
            std::string will;
            if (expanded.empty())
            {
                will = "(invalid id — nothing will be cancelled)";
            }
            else if (expanded.size() == 1)
            {
                will = expanded.front();
            }
            else
            {
                will = std::to_string(expanded.size()) + " tasks: "
                       + expanded.front() + " … " + expanded.back();
            }

            Elements dialog_children = {
                text("Cancel Job") | bold | center,
                separator(),
                text("ID:      " + m_cancel_target_id),
                text("Name:    " + m_cancel_target_name),
                text("Cancels: " + will) | dim,
                text(""),
                text("Really cancel?") | center,
                text(""),
                text("[y] confirm    [n/Esc] cancel") | dim | center,
            };
            if (!m_cancel_status.empty())
            {
                dialog_children.push_back(separator());
                dialog_children.push_back(text(m_cancel_status) | center);
            }
            auto dialog = window(text(" Confirm ") | bold,
                                 vbox(std::move(dialog_children)))
                          | size(WIDTH, GREATER_THAN, 40) | clear_under
                          | center;
            page        = dbox({page, dialog});
        }
        return page;
    });

    auto container = CatchEvent(renderer, [&](Event event)
    {
        if (event == Event::Custom)
            return true;

        if (m_search_mode)
        {
            if (event == Event::Return)
            {
                m_search_query = m_search_buffer;
                m_search_mode  = false;
                std::lock_guard<std::mutex> lk(m_jobs_mutex);
                m_view_dirty   = true;
                auto vsz       = static_cast<int>(view_of(m_jobs).size());
                m_selected_row = vsz > 0 ? 0 : -1;
                return true;
            }
            if (event == Event::Escape)
            {
                m_search_mode = false;
                m_search_buffer.clear();
                return true;
            }
            if (event == Event::Backspace)
            {
                if (!m_search_buffer.empty())
                    m_search_buffer.pop_back();
                return true;
            }
            if (event.is_character())
            {
                m_search_buffer += event.character();
                return true;
            }
            return true;
        }

        if (event == Event::Character('/'))
        {
            m_search_mode = true;
            m_search_buffer.clear();
            return true;
        }

        if (event == Event::Character('t'))
        {
            m_view_mode = m_view_mode == ViewMode::Live ? ViewMode::History
                                                        : ViewMode::Live;
            auto fresh = fetch_current();
            std::lock_guard<std::mutex> lk(m_jobs_mutex);
            m_jobs         = std::move(fresh);
            m_view_dirty   = true;
            m_selected_row = m_jobs.empty() ? -1 : 0;
            return true;
        }

        if (event == Event::Escape && !m_search_query.empty())
        {
            m_search_query.clear();
            std::lock_guard<std::mutex> lk(m_jobs_mutex);
            m_view_dirty   = true;
            m_selected_row = m_jobs.empty() ? -1 : 0;
            return true;
        }

        if (m_show_help_dialog)
        {
            m_show_help_dialog = false;
            return true;
        }

        if (event == Event::Character('?'))
        {
            m_show_help_dialog = true;
            return true;
        }

        if (m_show_cancel_dialog)
        {
            if (event == Event::Character('y') || event == Event::Return)
            {
                bool ok = cancel_job(m_cancel_target_id);
                m_cancel_status
                    = ok ? "cancelled — refreshing…" : "scancel failed";
                if (ok)
                {
                    auto fresh = fetch_jobs();
                    std::lock_guard<std::mutex> lk(m_jobs_mutex);
                    m_jobs       = std::move(fresh);
                    m_view_dirty = true;
                    auto vsz     = static_cast<int>(view_of(m_jobs).size());
                    if (m_selected_row >= vsz)
                        m_selected_row = vsz - 1;
                }
                m_show_cancel_dialog = false;
                m_cancel_status.clear();
                return true;
            }
            if (event == Event::Character('n') || event == Event::Escape)
            {
                m_show_cancel_dialog = false;
                m_cancel_status.clear();
                return true;
            }
            return true;
        }

        if (event == Event::Character('c') || event == Event::Character('C'))
        {
            std::lock_guard<std::mutex> lk(m_jobs_mutex);
            const auto &view = view_of(m_jobs);
            if (m_selected_row >= 0
                && m_selected_row < static_cast<int>(view.size()))
            {
                const Job &j = *view[m_selected_row];
                if (event == Event::Character('C'))
                {
                    auto us            = j.id().find('_');
                    m_cancel_target_id = (us == std::string::npos)
                                             ? j.id()
                                             : j.id().substr(0, us);
                }
                else
                {
                    m_cancel_target_id = j.id();
                }
                m_cancel_target_name = j.name();
                m_show_cancel_dialog = true;
                return true;
            }
            return false;
        }

        if (event == Event::Character('j'))
        {
            std::lock_guard<std::mutex> lk(m_jobs_mutex);
            auto vsz
                = static_cast<int>(view_of(m_jobs).size());
            if (m_selected_row < vsz - 1)
            {
                m_selected_row++;
                return true;
            }
            if (m_loop_after_end && vsz > 0)
            {
                m_selected_row = 0;
                return true;
            }
            return false;
        }

        if (event == Event::Character('k'))
        {
            std::lock_guard<std::mutex> lk(m_jobs_mutex);
            if (m_selected_row > 0)
            {
                m_selected_row--;
                return true;
            }
            if (m_loop_after_end)
            {
                auto vsz = static_cast<int>(
                    view_of(m_jobs).size());
                if (vsz > 0)
                {
                    m_selected_row = vsz - 1;
                    return true;
                }
            }
            return false;
        }

        if (event == Event::Character('g'))
        {
            using clock             = std::chrono::steady_clock;
            constexpr auto kTimeout = std::chrono::milliseconds(500);
            auto now                = clock::now();
            if (m_pending_g_time.time_since_epoch().count() != 0
                && now - m_pending_g_time < kTimeout)
            {
                m_pending_g_time = {};
                std::lock_guard<std::mutex> lk(m_jobs_mutex);
                auto vsz = static_cast<int>(
                    view_of(m_jobs).size());
                if (vsz > 0)
                {
                    m_selected_row = 0;
                    return true;
                }
                return false;
            }
            m_pending_g_time = now;
            return true;
        }

        if (event == Event::Character('G'))
        {
            m_pending_g_time = {};
            std::lock_guard<std::mutex> lk(m_jobs_mutex);
            auto vsz
                = static_cast<int>(view_of(m_jobs).size());
            if (vsz > 0)
            {
                m_selected_row = vsz - 1;
                return true;
            }
            return false;
        }

        if (event == Event::Character('e'))
        {
            m_show_stderr = !m_show_stderr;
            return true;
        }

        if (event == Event::Character('s'))
        {
            switch (m_sort_key)
            {
            case SortKey::None:  m_sort_key = SortKey::Id;    break;
            case SortKey::Id:    m_sort_key = SortKey::Name;  break;
            case SortKey::Name:  m_sort_key = SortKey::State; break;
            case SortKey::State: m_sort_key = SortKey::Time;  break;
            case SortKey::Time:  m_sort_key = SortKey::None;  break;
            }
            std::lock_guard<std::mutex> lk(m_jobs_mutex);
            m_view_dirty   = true;
            auto vsz       = static_cast<int>(view_of(m_jobs).size());
            m_selected_row = vsz > 0 ? 0 : -1;
            return true;
        }

        if (event == Event::Character('S'))
        {
            m_sort_descending = !m_sort_descending;
            std::lock_guard<std::mutex> lk(m_jobs_mutex);
            m_view_dirty   = true;
            auto vsz       = static_cast<int>(view_of(m_jobs).size());
            m_selected_row = vsz > 0 ? 0 : -1;
            return true;
        }

        if (event == Event::Character('q'))
        {
            m_running.store(false, std::memory_order_relaxed);
            screen.Exit();
            return true;
        }

        if (event == Event::F1)
        {
            m_show_footer = !m_show_footer;
            return true;
        }

        return false;
    });

    screen.Loop(container);

    m_running.store(false, std::memory_order_relaxed);
    if (ticker.joinable())
        ticker.join();
}

void
Slurmon::init_config() noexcept
{
    if (m_config_path.empty())
        return;

    m_config = Config();
    // Parse the TOML configuration file
    toml::table toml;
    try
    {
        toml = toml::parse_file(m_config_path);
    }
    catch (const toml::parse_error &e)
    {
        std::cerr << "Failed to parse config file: " << m_config_path << "\n"
                  << e.description() << std::endl;
        return;
    }

    // [footer]
    load_config_field(toml, "footer", "show", m_config.footer.show);
    m_show_footer = m_config.footer.show;

    // [job_view]
    load_config_field(toml, "job_view", "show", m_config.job_view.show);
    load_config_field(toml, "job_view", "loop_after_end",
                      m_config.job_view.loop_after_end);
    m_loop_after_end = m_config.job_view.loop_after_end;
    {
        int64_t tmp = m_config.job_view.refresh_interval;
        load_config_field(toml, "job_view", "refresh_interval", tmp);
        if (tmp >= 1)
            m_config.job_view.refresh_interval = static_cast<int>(tmp);
    }

    if (auto arr = toml["job_view"]["columns"].as_array())
    {
        std::vector<std::string> cols;
        for (auto &&v : *arr)
            if (auto s = v.value<std::string>())
                cols.push_back(*s);
        if (!cols.empty())
            m_config.job_view.columns = std::move(cols);
    }
    load_config_field(toml, "job_view", "sort_by", m_config.job_view.sort_by);
    load_config_field(toml, "job_view", "sort_descending",
                      m_config.job_view.sort_descending);
    {
        const auto &sb = m_config.job_view.sort_by;
        if (sb == "id")         m_sort_key = SortKey::Id;
        else if (sb == "name")  m_sort_key = SortKey::Name;
        else if (sb == "state") m_sort_key = SortKey::State;
        else if (sb == "time")  m_sort_key = SortKey::Time;
        else                    m_sort_key = SortKey::None;
        m_sort_descending = m_config.job_view.sort_descending;
    }

    // [log_view]
    load_config_field(toml, "log_view", "show", m_config.log_view.show);
    load_config_field(toml, "log_view", "error_first",
                      m_config.log_view.error_first);
    m_show_stderr = m_config.log_view.error_first;

    // [detail_view]
    load_config_field(toml, "detail_view", "show",
                      m_config.detail_view.show);
    load_config_field(toml, "detail_view", "show_id",
                      m_config.detail_view.show_id);
    load_config_field(toml, "detail_view", "show_name",
                      m_config.detail_view.show_name);
    load_config_field(toml, "detail_view", "show_state",
                      m_config.detail_view.show_state);
    load_config_field(toml, "detail_view", "show_user",
                      m_config.detail_view.show_user);
    load_config_field(toml, "detail_view", "show_time",
                      m_config.detail_view.show_time);
    load_config_field(toml, "detail_view", "show_nodes",
                      m_config.detail_view.show_nodes);
    load_config_field(toml, "detail_view", "show_nodelist",
                      m_config.detail_view.show_nodelist);
}

// Column definitions for the job list. `width` == -1 means the column
// auto-sizes to fit its content; `flex` marks the column that gets any
// remaining horizontal space.
// Column definitions for the job list. Each column carries the squeue
// format specifier and sacct field name used to fetch it (nullptr when
// the source doesn't expose an equivalent). `base_width` == -1 means the
// column auto-sizes to its widest value; `flex` marks the column that
// consumes any remaining horizontal space.
struct JobColumn
{
    const char *key;
    const char *label;
    int base_width;
    bool flex;
    Slurmon::SortKey sort;
    bool colored;
    const char *squeue_fmt;
    const char *sacct_fmt;
};

static const std::vector<JobColumn> &
all_job_columns()
{
    // clang-format off
    static const std::vector<JobColumn> cols = {
        // key             label              W   flex   sort                    color  squeue    sacct
        {"id",             "ID",              -1, false, Slurmon::SortKey::Id,    false, "%i",     "JobID"},
        {"name",           "NAME",            24, true,  Slurmon::SortKey::Name,  false, "%j",     "JobName"},
        {"state",          "STATE",           12, false, Slurmon::SortKey::State, true,  "%T",     "State"},
        {"state_compact",  "ST",               3, false, Slurmon::SortKey::None,  true,  "%t",     nullptr},
        {"user",           "USER",            10, false, Slurmon::SortKey::None,  false, "%u",     "User"},
        {"uid",            "UID",              8, false, Slurmon::SortKey::None,  false, "%U",     "UID"},
        {"group",          "GROUP",           10, false, Slurmon::SortKey::None,  false, "%g",     "Group"},
        {"gid",            "GID",              8, false, Slurmon::SortKey::None,  false, "%G",     "GID"},
        {"account",        "ACCOUNT",         12, false, Slurmon::SortKey::None,  false, "%a",     "Account"},
        {"partition",      "PARTITION",       12, false, Slurmon::SortKey::None,  false, "%P",     "Partition"},
        {"qos",            "QOS",             10, false, Slurmon::SortKey::None,  false, "%q",     "QOS"},
        {"priority",       "PRIORITY",        10, false, Slurmon::SortKey::None,  false, "%Q",     "Priority"},
        {"nice",           "NICE",             6, false, Slurmon::SortKey::None,  false, "%y",     nullptr},
        {"time",           "TIME",            12, false, Slurmon::SortKey::Time,  false, "%M",     "Elapsed"},
        {"time_limit",     "TIME_LIMIT",      12, false, Slurmon::SortKey::None,  false, "%l",     "Timelimit"},
        {"time_left",      "TIME_LEFT",       12, false, Slurmon::SortKey::None,  false, "%L",     nullptr},
        {"submit_time",    "SUBMIT_TIME",     20, false, Slurmon::SortKey::None,  false, "%V",     "Submit"},
        {"start_time",     "START_TIME",      20, false, Slurmon::SortKey::None,  false, "%S",     "Start"},
        {"end_time",       "END_TIME",        20, false, Slurmon::SortKey::None,  false, "%e",     "End"},
        {"nodes",          "NODES",            6, false, Slurmon::SortKey::None,  false, "%D",     "NNodes"},
        {"nodelist",       "NODELIST/REASON", -1, true,  Slurmon::SortKey::None,  false, "%R",     "NodeList"},
        {"reason",         "REASON",          20, true,  Slurmon::SortKey::None,  false, "%r",     nullptr},
        {"min_cpus",       "MIN_CPUS",         8, false, Slurmon::SortKey::None,  false, "%c",     "ReqCPUS"},
        {"cpus",           "CPUS",             6, false, Slurmon::SortKey::None,  false, "%C",     "AllocCPUS"},
        {"min_memory",     "MIN_MEMORY",      12, false, Slurmon::SortKey::None,  false, "%m",     "ReqMem"},
        {"tres",           "TRES",            25, true,  Slurmon::SortKey::None,  false, "%b",     "ReqTRES"},
        {"features",       "FEATURES",        15, false, Slurmon::SortKey::None,  false, "%f",     nullptr},
        {"dependency",     "DEPENDENCY",      15, false, Slurmon::SortKey::None,  false, "%E",     nullptr},
        {"reservation",    "RESERVATION",     15, false, Slurmon::SortKey::None,  false, "%v",     "Reservation"},
        {"wckey",          "WCKEY",           10, false, Slurmon::SortKey::None,  false, "%w",     "Wckey"},
        {"licenses",       "LICENSES",        15, false, Slurmon::SortKey::None,  false, "%W",     nullptr},
        {"command",        "COMMAND",         30, true,  Slurmon::SortKey::None,  false, "%o",     nullptr},
        {"workdir",        "WORKDIR",         30, true,  Slurmon::SortKey::None,  false, "%Z",     "WorkDir"},
        {"exec_host",      "EXEC_HOST",       15, false, Slurmon::SortKey::None,  false, "%B",     nullptr},
        {"array_id",       "ARRAY_ID",        12, false, Slurmon::SortKey::None,  false, "%F",     nullptr},
        {"array_task",     "ARRAY_TASK",      10, false, Slurmon::SortKey::None,  false, "%K",     nullptr},
        {"sockets",        "SOCKETS",          7, false, Slurmon::SortKey::None,  false, "%H",     nullptr},
        {"cores",          "CORES",            5, false, Slurmon::SortKey::None,  false, "%I",     nullptr},
        {"threads",        "THREADS",          7, false, Slurmon::SortKey::None,  false, "%J",     nullptr},
        {"sct",            "S:C:T",            7, false, Slurmon::SortKey::None,  false, "%z",     nullptr},
        {"comment",        "COMMENT",         20, true,  Slurmon::SortKey::None,  false, "%k",     "Comment"},
        {"exit_code",      "EXIT_CODE",        9, false, Slurmon::SortKey::None,  false, nullptr,  "ExitCode"},
    };
    // clang-format on
    return cols;
}

// Resolve the configured column names into a display list, silently
// dropping any unknown keys. Falls back to the default set if the user
// left the array empty.
static std::vector<JobColumn>
resolve_columns(const std::vector<std::string> &names)
{
    const auto &all = all_job_columns();
    std::vector<JobColumn> out;
    for (const auto &n : names)
    {
        for (const auto &c : all)
        {
            if (n == c.key)
            {
                out.push_back(c);
                break;
            }
        }
    }
    if (out.empty())
    {
        for (const auto &n : {"id", "name", "state", "time"})
            for (const auto &c : all)
                if (std::string(c.key) == n)
                    out.push_back(c);
    }
    return out;
}

// Build all rows for the job table
ftxui::Elements
Slurmon::build_rows(const std::vector<const Job *> &jobs)
{
    using namespace ftxui;
    Elements rows;

    // Resolving columns walks the whole column table on every render; the
    // set only changes when config is (re)loaded, so cache it.
    static thread_local std::vector<std::string> cached_keys;
    static thread_local std::vector<JobColumn> cached_columns;
    if (cached_keys != m_config.job_view.columns)
    {
        cached_keys    = m_config.job_view.columns;
        cached_columns = resolve_columns(cached_keys);
    }
    const auto &columns = cached_columns;

    // Compute per-column widths (auto-size for base_width == -1).
    constexpr int kAutoPadding = 2;
    std::vector<int> widths(columns.size());
    for (size_t i = 0; i < columns.size(); ++i)
    {
        if (columns[i].base_width >= 0)
        {
            widths[i] = columns[i].base_width;
            continue;
        }
        int w = static_cast<int>(std::string(columns[i].label).size());
        for (const Job *j : jobs)
            w = std::max(w, static_cast<int>(j->get(columns[i].key).size()));
        widths[i] = w + kAutoPadding;
    }

    auto cell = [](const std::string &s, int w, bool flex_it)
    {
        auto el = text(" " + s) | size(WIDTH, EQUAL, w);
        if (flex_it)
            el = el | flex;
        return el;
    };

    // Header row with sort indicator on the active column.
    const char *arrow = m_sort_descending ? " ▼" : " ▲";
    Elements header;
    for (size_t i = 0; i < columns.size(); ++i)
    {
        if (i > 0)
            header.push_back(separator());
        std::string s = " " + std::string(columns[i].label);
        if (m_sort_key != SortKey::None && m_sort_key == columns[i].sort)
            s += arrow;
        auto el = text(s) | size(WIDTH, EQUAL, widths[i]) | bold;
        if (m_sort_key != SortKey::None && m_sort_key == columns[i].sort)
            el = el | underlined | color(Color::Yellow);
        if (columns[i].flex)
            el = el | flex;
        header.push_back(el);
    }
    rows.push_back(hbox(std::move(header)));
    rows.push_back(separator());

    // Data rows.
    for (size_t r = 0; r < jobs.size(); ++r)
    {
        const Job &j = *jobs[r];
        Elements row_cells;
        row_cells.reserve(columns.size() * 2);
        for (size_t i = 0; i < columns.size(); ++i)
        {
            if (i > 0)
                row_cells.push_back(separator());
            const std::string &val = j.get(columns[i].key);
            auto el                = cell(val, widths[i], columns[i].flex);
            if (columns[i].colored)
                el = el | state_color(j.state()) | bold;
            row_cells.push_back(el);
        }
        auto row = hbox(std::move(row_cells));
        if (static_cast<int>(r) == m_selected_row)
            row = row | inverted;
        rows.push_back(row);
    }

    return rows;
}

// Slurp all of a popen pipe's output into a string.
static std::string
slurp_pipe(FILE *fp)
{
    std::string out;
    std::array<char, 1024> buf;
    while (std::fgets(buf.data(), buf.size(), fp) != nullptr)
        out += buf.data();
    return out;
}

// Parse a pipe-delimited line into `keys` fields in order, assigning
// each token to the matching column key on the job. Missing trailing
// fields become empty strings.
static Job
parse_row(const std::string &line, const std::vector<const char *> &keys)
{
    Job j;
    size_t pos = 0;
    for (size_t i = 0; i < keys.size(); ++i)
    {
        auto next = line.find('|', pos);
        if (next == std::string::npos)
        {
            j.set(keys[i], line.substr(pos));
            for (size_t k = i + 1; k < keys.size(); ++k)
                j.set(keys[k], "");
            return j;
        }
        j.set(keys[i], line.substr(pos, next - pos));
        pos = next + 1;
    }
    return j;
}

// Fetch live jobs from the Slurm scheduler using squeue. Every column
// with a squeue format specifier is requested; the UI decides which to
// display.
std::vector<Job>
Slurmon::fetch_jobs()
{
    std::vector<Job> jobs;

    std::string fmt;
    std::vector<const char *> keys;
    for (const auto &c : all_job_columns())
    {
        if (!c.squeue_fmt)
            continue;
        if (!fmt.empty())
            fmt += "|";
        fmt += c.squeue_fmt;
        keys.push_back(c.key);
    }

    std::string cmd = "squeue --noheader -o \"" + fmt + "\"";
    if (!m_squeue_args.empty())
        cmd += " " + m_squeue_args;
    cmd += " 2>/dev/null";
    std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(cmd.c_str(), "r"),
                                                  pclose);
    if (!pipe)
        return jobs;

    std::string output = slurp_pipe(pipe.get());
    if (pclose(pipe.release()) != 0)
        return jobs;

    std::istringstream stream(output);
    std::string line;
    while (std::getline(stream, line))
    {
        if (line.empty())
            continue;
        jobs.push_back(parse_row(line, keys));
    }
    return jobs;
}

// Fetch historical jobs via sacct. Requests every column that has a
// sacct field name; columns without a sacct equivalent stay empty.
std::vector<Job>
Slurmon::fetch_history_jobs()
{
    std::vector<Job> jobs;

    std::string fields;
    std::vector<const char *> keys;
    for (const auto &c : all_job_columns())
    {
        if (!c.sacct_fmt)
            continue;
        if (!fields.empty())
            fields += ",";
        fields += c.sacct_fmt;
        keys.push_back(c.key);
    }

    std::string cmd = "sacct --noheader -X -P -o " + fields;
    if (!m_sacct_args.empty())
        cmd += " " + m_sacct_args;
    cmd += " 2>/dev/null";
    std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(cmd.c_str(), "r"),
                                                  pclose);
    if (!pipe)
        return jobs;

    std::string output = slurp_pipe(pipe.get());
    if (pclose(pipe.release()) != 0)
        return jobs;

    std::istringstream stream(output);
    std::string line;
    while (std::getline(stream, line))
    {
        if (line.empty())
            continue;
        jobs.push_back(parse_row(line, keys));
    }
    return jobs;
}

// Fetch the log paths for a given job ID using the scontrol command
Slurmon::LogPaths
Slurmon::fetch_log_paths(const std::string &job_id)
{
    LogPaths result;

    std::string cmd = "scontrol show job " + job_id + " 2>/dev/null";
    std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(cmd.c_str(), "r"),
                                                  pclose);
    if (!pipe)
        return result;

    std::array<char, 512> buffer;
    std::string output;
    while (std::fgets(buffer.data(), buffer.size(), pipe.get()) != nullptr)
        output += buffer.data();
    pclose(pipe.release());

    auto extract = [&](const std::string &key) -> std::string
    {
        auto pos = output.find(key);
        if (pos == std::string::npos)
            return {};
        pos += key.size();
        auto end = output.find_first_of(" \n\t", pos);
        return output.substr(pos, end - pos);
    };

    result.stdout_path = extract("StdOut=");
    result.stderr_path = extract("StdErr=");
    return result;
}

// Read the last `max_lines` lines from a file at the given path
std::string
Slurmon::read_tail(const std::string &path, size_t max_lines)
{
    std::ifstream in(path);
    if (!in)
        return {};

    std::deque<std::string> lines;
    std::string line;
    while (std::getline(in, line))
    {
        lines.push_back(std::move(line));
        if (lines.size() > max_lines)
            lines.pop_front();
    }

    std::string out;
    for (auto &l : lines)
    {
        out += l;
        out += '\n';
    }
    return out;
}

// Get the log paths for a given job ID, using a cache to avoid repeated calls
// to fetch_log_paths
const Slurmon::LogPaths &
Slurmon::log_paths_for(const std::string &job_id)
{
    auto it = m_log_paths_cache.find(job_id);
    if (it != m_log_paths_cache.end())
        return it->second;
    auto [ins, _] = m_log_paths_cache.emplace(job_id, fetch_log_paths(job_id));
    return ins->second;
}

// Expand a SLURM array-id expression like "12345_[1-5,7,9-11:2]" into
// concrete task ids ("12345_1", "12345_2", ...). Plain "12345" and single
// tasks "12345_3" are returned unchanged. Returns empty on parse failure.
static std::vector<std::string>
expand_array_ids(const std::string &id)
{
    std::vector<std::string> out;
    auto is_digits = [](const std::string &s)
    {
        if (s.empty())
            return false;
        for (char c : s)
            if (!std::isdigit(static_cast<unsigned char>(c)))
                return false;
        return true;
    };

    auto us = id.find('_');
    if (us == std::string::npos)
    {
        if (!is_digits(id))
            return {};
        out.push_back(id);
        return out;
    }
    std::string base = id.substr(0, us);
    std::string tail = id.substr(us + 1);
    if (!is_digits(base))
        return {};

    if (tail.size() < 2 || tail.front() != '[' || tail.back() != ']')
    {
        if (!is_digits(tail))
            return {};
        out.push_back(id);
        return out;
    }

    std::string inner = tail.substr(1, tail.size() - 2);
    size_t pos        = 0;
    while (pos <= inner.size())
    {
        auto comma      = inner.find(',', pos);
        std::string tok = inner.substr(
            pos, comma == std::string::npos ? std::string::npos : comma - pos);
        auto colon = tok.find(':');
        std::string range
            = colon == std::string::npos ? tok : tok.substr(0, colon);
        int step = 1;
        if (colon != std::string::npos)
        {
            std::string s = tok.substr(colon + 1);
            if (!is_digits(s))
                return {};
            step = std::stoi(s);
            if (step < 1)
                return {};
        }
        auto dash = range.find('-');
        if (dash == std::string::npos)
        {
            if (!is_digits(range))
                return {};
            out.push_back(base + "_" + range);
        }
        else
        {
            std::string a = range.substr(0, dash);
            std::string b = range.substr(dash + 1);
            if (!is_digits(a) || !is_digits(b))
                return {};
            int ai = std::stoi(a);
            int bi = std::stoi(b);
            if (ai > bi)
                return {};
            for (int i = ai; i <= bi; i += step)
                out.push_back(base + "_" + std::to_string(i));
        }
        if (comma == std::string::npos)
            break;
        pos = comma + 1;
    }
    return out;
}

// Cancel a job (or expanded array range) using scancel. Only numeric,
// underscore-separated ids are ever passed to the shell.
bool
Slurmon::cancel_job(const std::string &job_id)
{
    if (job_id.empty())
        return false;

    auto ids = expand_array_ids(job_id);
    if (ids.empty())
        return false;

    std::string cmd = "scancel";
    if (!m_scancel_args.empty())
        cmd += " " + m_scancel_args;
    for (const auto &x : ids)
    {
        for (char c : x)
        {
            if (!std::isdigit(static_cast<unsigned char>(c)) && c != '_')
                return false;
        }
        cmd += " " + x;
    }
    cmd += " >/dev/null 2>&1";
    return std::system(cmd.c_str()) == 0;
}
