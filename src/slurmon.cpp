#include "slurmon.hpp"

#include <cctype>
#include <chrono>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <thread>

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

    {
        auto initial = fetch_jobs();
        std::lock_guard<std::mutex> lk(m_jobs_mutex);
        m_jobs = std::move(initial);
    }

    std::thread ticker([&]
    {
        while (m_running.load(std::memory_order_relaxed))
        {
            for (int i = 0; i < 50 && m_running.load(std::memory_order_relaxed);
                 ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));

            if (!m_running.load(std::memory_order_relaxed))
                break;

            auto fresh = fetch_jobs();
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

        return window(text(" Jobs ") | bold, vbox(build_rows(view)));
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
            details = vbox({
                field("ID:", text(j.id())),
                field("Name:", text(j.name())),
                field("State:",
                      text(j.state()) | state_color(j.state()) | bold),
                field("User:", text(j.user())),
                field("Time:", text(j.time())),
                field("Nodes:", text(j.nodes())),
                field("Nodelist:", text(j.nodelist_or_reason())),
            });
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

        return vbox({window(text(" Details ") | bold, details),
                     window(text(log_title) | bold, log_content) | flex});
    });

    auto split
        = ResizableSplitLeft(left_renderer, right_renderer, &m_split_size);

    auto renderer = Renderer(split, [&]
    {
        Elements root = {split->Render() | flex};
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
            root.push_back(text("/ search, ? help, F1 footer, q quit") | dim
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
                             binding("c", "cancel selected job"),
                             binding("/", "search (id/name/state/time)"),
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
            Elements dialog_children = {
                text("Cancel Job") | bold | center,
                separator(),
                text("ID:   " + m_cancel_target_id),
                text("Name: " + m_cancel_target_name),
                text(""),
                text("Really cancel this job?") | center,
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

        if (event == Event::Character('c'))
        {
            std::lock_guard<std::mutex> lk(m_jobs_mutex);
            auto view = filter_jobs(m_jobs, m_search_query);
            if (m_selected_row >= 0
                && m_selected_row < static_cast<int>(view.size()))
            {
                const auto &j        = view[m_selected_row];
                m_cancel_target_id   = j.id();
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

// Build a single row for the job table
ftxui::Element
Slurmon::build_row(const Job &job, bool selected)
{
    using namespace ftxui;

    auto cell = [](const std::string &s, int w)
    {
        return text(" " + s) | size(WIDTH, EQUAL, w);
    };

    auto row = hbox({
        cell(job.id(), 8),
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

    rows.push_back(build_row(Job("ID", "NAME", "STATE", "USER", "TIME", "NODES",
                                 "NODELIST/REASON"),
                             false)
                   | bold);
    rows.push_back(separator());

    for (size_t i = 0; i < jobs.size(); ++i)
    {
        rows.push_back(build_row(jobs.at(i), (int)i == m_selected_row));
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

// Cancel a job with the given job ID using the scancel command
bool
Slurmon::cancel_job(const std::string &job_id)
{
    if (job_id.empty())
        return false;
    for (char c : job_id)
    {
        if (!std::isdigit(static_cast<unsigned char>(c)) && c != '_'
            && c != '.')
            return false;
    }
    std::string cmd = "scancel " + job_id + " >/dev/null 2>&1";
    return std::system(cmd.c_str()) == 0;
}
