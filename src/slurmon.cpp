#include "slurmon.hpp"

#include <chrono>
#include <iostream>
#include <memory>
#include <sstream>
#include <thread>

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

    auto renderer = Renderer([&]
    {
        std::lock_guard<std::mutex> lk(m_jobs_mutex);
        if (m_selected_row == -1 && !m_jobs.empty())
            m_selected_row = 0;
        auto rows = build_rows(m_jobs);

        auto left_pane = vbox({rows}) | border;

        auto right_pane = vbox({window(text("Details") | bold,
                                       text("select a job to see details")),
                                window(text("Logs") | bold,
                                       text("select a job to see logs"))
                                    | flex})
                          | flex;

        auto footer = m_show_footer
                          ? (text("j/k to navigate, q to quit") | dim | center)
                          : text("");

        return vbox({hbox({
                         left_pane,
                         right_pane,
                     }) | flex,
                     footer});
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
    auto row = hbox({
        text(job.id()) | size(WIDTH, EQUAL, 5),
        text(job.name()) | size(WIDTH, EQUAL, 20),
        text(job.state()) | size(WIDTH, EQUAL, 10),
        text(job.time()) | size(WIDTH, EQUAL, 10),
    });

    if (selected)
        row = row | inverted;

    return row;
}

ftxui::Elements
Slurmon::build_rows(const std::vector<Job> &jobs)
{
    ftxui::Elements rows;

    rows.push_back(build_row(Job("ID", "NAME", "STATE", "USER", "TIME", "NODES",
                                 "NODELIST/REASON"),
                             false)
                   | ftxui::bold | ftxui::underlined);

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
