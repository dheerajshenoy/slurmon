#include "slurmon.hpp"

#include <chrono>
#include <deque>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <thread>

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

Slurmon::Slurmon() : m_selected_row(-1) {}

Slurmon::~Slurmon() {}

void
Slurmon::run()
{
    init_ui();
}

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
                m_jobs = std::move(fresh);
                if (m_selected_row >= static_cast<int>(m_jobs.size()))
                    m_selected_row = static_cast<int>(m_jobs.size()) - 1;
                if (m_selected_row < 0 && !m_jobs.empty())
                    m_selected_row = 0;
            }
            screen.PostEvent(Event::Custom);
        }
    });

    auto left_renderer = Renderer([&]
    {
        std::lock_guard<std::mutex> lk(m_jobs_mutex);
        if (m_selected_row == -1 && !m_jobs.empty())
            m_selected_row = 0;
        auto rows = build_rows(m_jobs);
        return window(text(" Jobs ") | bold, vbox(std::move(rows)));
    });

    auto right_renderer = Renderer([&]
    {
        std::lock_guard<std::mutex> lk(m_jobs_mutex);

        Element details;
        if (m_selected_row >= 0
            && m_selected_row < static_cast<int>(m_jobs.size()))
        {
            const auto &j = m_jobs[m_selected_row];
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
            && m_selected_row < static_cast<int>(m_jobs.size()))
        {
            const auto &j     = m_jobs[m_selected_row];
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
        if (m_show_footer)
        {
            root.push_back(
                text(std::string(
                         "j/k navigate, e toggle, F1 to toggle footer, ")
                     + (m_show_stderr ? "stdout" : "stderr") + ", q quit")
                | dim | center);
        }
        return vbox(std::move(root));
    });

    auto container = CatchEvent(renderer, [&](Event event)
    {
        if (event == Event::Custom)
            return true;

        if (event == Event::Character('j'))
        {
            std::lock_guard<std::mutex> lk(m_jobs_mutex);
            if (m_selected_row < static_cast<int>(m_jobs.size()) - 1)
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

const Slurmon::LogPaths &
Slurmon::log_paths_for(const std::string &job_id)
{
    auto it = m_log_paths_cache.find(job_id);
    if (it != m_log_paths_cache.end())
        return it->second;
    auto [ins, _] = m_log_paths_cache.emplace(job_id, fetch_log_paths(job_id));
    return ins->second;
}
