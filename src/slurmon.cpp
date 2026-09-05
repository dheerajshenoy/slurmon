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

// Check if a job matches the search query (case-insensitive)
static bool
matches_query(const Job &j, const std::string &q_lower)
{
    if (q_lower.empty())
        return true;
    auto lower = [](std::string s)
    {
        for (auto &c : s)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    };
    auto contains = [&](const std::string &s)
    {
        return lower(s).find(q_lower) != std::string::npos;
    };
    return contains(j.id()) || contains(j.name()) || contains(j.state())
           || contains(j.time());
}

// Filter jobs based on the search query (case-insensitive)
static std::vector<Job>
filter_jobs(const std::vector<Job> &jobs, const std::string &query)
{
    if (query.empty())
        return jobs;
    std::string q;
    q.reserve(query.size());
    for (char c : query)
        q.push_back(
            static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    std::vector<Job> out;
    out.reserve(jobs.size());
    for (const auto &j : jobs)
        if (matches_query(j, q))
            out.push_back(j);
    return out;
}

// Forward-declared here; defined near cancel_job.
static std::vector<std::string>
expand_array_ids(const std::string &id);

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

    {
        auto initial = fetch_current();
        std::lock_guard<std::mutex> lk(m_jobs_mutex);
        m_jobs = std::move(initial);
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
                m_jobs   = std::move(fresh);
                auto vsz = static_cast<int>(
                    filter_jobs(m_jobs, m_search_query).size());
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
        auto view = filter_jobs(m_jobs, m_search_query);
        if (m_selected_row == -1 && !view.empty())
            m_selected_row = 0;
        if (m_selected_row >= static_cast<int>(view.size()))
            m_selected_row
                = view.empty() ? -1 : static_cast<int>(view.size()) - 1;

        std::string title = m_view_mode == ViewMode::History
                                ? " Jobs (history) "
                                : " Jobs ";
        return window(text(title) | bold, vbox(build_rows(view)));
    });

    auto right_renderer = Renderer([&]
    {
        std::lock_guard<std::mutex> lk(m_jobs_mutex);
        auto view = filter_jobs(m_jobs, m_search_query);

        Element details;
        if (m_selected_row >= 0
            && m_selected_row < static_cast<int>(view.size()))
        {
            const auto &j = view[m_selected_row];
            auto field    = [](const std::string &label, Element val)
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
            const auto &j     = view[m_selected_row];
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
                text("/ search, t history, ? help, F1 footer, q quit") | dim
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
                auto vsz = static_cast<int>(
                    filter_jobs(m_jobs, m_search_query).size());
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
            m_selected_row = m_jobs.empty() ? -1 : 0;
            return true;
        }

        if (event == Event::Escape && !m_search_query.empty())
        {
            m_search_query.clear();
            std::lock_guard<std::mutex> lk(m_jobs_mutex);
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
                    m_jobs   = std::move(fresh);
                    auto vsz = static_cast<int>(
                        filter_jobs(m_jobs, m_search_query).size());
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
            auto view = filter_jobs(m_jobs, m_search_query);
            if (m_selected_row >= 0
                && m_selected_row < static_cast<int>(view.size()))
            {
                const auto &j = view[m_selected_row];
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
                = static_cast<int>(filter_jobs(m_jobs, m_search_query).size());
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
                    filter_jobs(m_jobs, m_search_query).size());
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
                    filter_jobs(m_jobs, m_search_query).size());
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
                = static_cast<int>(filter_jobs(m_jobs, m_search_query).size());
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

// Build a single row for the job table
ftxui::Element
Slurmon::build_row(const Job &job, bool selected, int id_width)
{
    using namespace ftxui;

    auto cell = [](const std::string &s, int w)
    {
        return text(" " + s) | size(WIDTH, EQUAL, w);
    };

    auto row = hbox({
        cell(job.id(), id_width),
        separator(),
        cell(job.name(), 24) | flex,
        separator(),
        cell(job.state(), 12) | state_color(job.state()) | bold,
        separator(),
        cell(job.time(), 12),
    });

    if (selected)
        row = row | inverted;

    return row;
}

// Build all rows for the job table
ftxui::Elements
Slurmon::build_rows(const std::vector<Job> &jobs)
{
    using namespace ftxui;
    Elements rows;

    constexpr int kIdPadding = 2;
    int id_width             = static_cast<int>(std::string("ID").size());
    for (const auto &j : jobs)
        id_width = std::max(id_width, static_cast<int>(j.id().size()));
    id_width += kIdPadding;

    rows.push_back(build_row(Job("ID", "NAME", "STATE", "USER", "TIME", "NODES",
                                 "NODELIST/REASON"),
                             false, id_width)
                   | bold);
    rows.push_back(separator());

    for (size_t i = 0; i < jobs.size(); ++i)
    {
        rows.push_back(
            build_row(jobs.at(i), (int)i == m_selected_row, id_width));
    }

    return rows;
}

// Fetch jobs from the Slurm scheduler using the squeue command
std::vector<Job>
Slurmon::fetch_jobs()
{
    std::vector<Job> jobs;

    std::array<char, 128> buffer;
    std::string output;

    std::unique_ptr<FILE, decltype(&pclose)> pipe(
        popen("squeue --noheader -o \"%i|%j|%T|%u|%M|%D|%R\"", "r"), pclose);
    if (!pipe)
    {
        std::cerr << "Failed to run squeue command" << std::endl;
        return jobs;
    }

    while (std::fgets(buffer.data(), buffer.size(), pipe.get()) != nullptr)
    {
        output += buffer.data();
    }

    int exit_code = pclose(pipe.release());
    if (exit_code != 0)
    {
        std::cerr << "squeue command failed with exit code: " << exit_code
                  << std::endl;
        return jobs;
    }

    std::istringstream stream(output);
    std::string line;

    while (std::getline(stream, line))
    {
        std::istringstream line_stream(line);
        std::string id, name, state, user, time, nodes, nodelist_or_reason;

        if (std::getline(line_stream, id, '|')
            && std::getline(line_stream, name, '|')
            && std::getline(line_stream, state, '|')
            && std::getline(line_stream, user, '|')
            && std::getline(line_stream, time, '|')
            && std::getline(line_stream, nodes, '|')
            && std::getline(line_stream, nodelist_or_reason))
        {
            jobs.emplace_back(id, name, state, user, time, nodes,
                              nodelist_or_reason);
        }
    }

    return jobs;
}

// Fetch historical jobs from the SLURM accounting database using sacct.
// Shows jobs completed since 00:00 today (sacct's default window).
std::vector<Job>
Slurmon::fetch_history_jobs()
{
    std::vector<Job> jobs;

    std::unique_ptr<FILE, decltype(&pclose)> pipe(
        popen("sacct --noheader -X -P "
              "-o JobID,JobName,State,User,Elapsed,NNodes,NodeList "
              "2>/dev/null",
              "r"),
        pclose);
    if (!pipe)
        return jobs;

    std::array<char, 512> buffer;
    std::string output;
    while (std::fgets(buffer.data(), buffer.size(), pipe.get()) != nullptr)
        output += buffer.data();

    int exit_code = pclose(pipe.release());
    if (exit_code != 0)
        return jobs;

    std::istringstream stream(output);
    std::string line;
    while (std::getline(stream, line))
    {
        std::istringstream ls(line);
        std::string id, name, state, user, time, nodes, nodelist;
        if (std::getline(ls, id, '|') && std::getline(ls, name, '|')
            && std::getline(ls, state, '|') && std::getline(ls, user, '|')
            && std::getline(ls, time, '|') && std::getline(ls, nodes, '|')
            && std::getline(ls, nodelist))
        {
            jobs.emplace_back(id, name, state, user, time, nodes, nodelist);
        }
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
