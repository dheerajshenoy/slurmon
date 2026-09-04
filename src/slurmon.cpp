#include "slurmon.hpp"

Slurmon::Slurmon() : m_selected_row(-1) {}

Slurmon::~Slurmon()
{
    // Destructor implementation
}

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

    auto renderer = Renderer([&]
    {
        auto jobs = fetch_jobs();
        if (m_selected_row == -1 && !jobs.empty())
            m_selected_row = 0;
        auto rows = build_rows(jobs);

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
        if (event == Event::Character('j')
            && m_selected_row < static_cast<int>(m_jobs.size()) - 1)
        {
            m_selected_row++;
            return true;
        }

        if (event == Event::Character('k') && m_selected_row > 0)
        {
            m_selected_row--;
            return true;
        }

        if (event == Event::Character('q'))
        {
            m_running = false;
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

    // m_running = false;
}

ftxui::Element
Slurmon::build_row(const Job &job, bool selected)
{
    using namespace ftxui;
    auto row = hbox({
        text(job.id()) | size(WIDTH, EQUAL, 5),
        text(job.name()) | size(WIDTH, EQUAL, 20),
        text(job.state()) | size(WIDTH, EQUAL, 10),
        // text(job.user()) | flex,
        text(job.time()) | size(WIDTH, EQUAL, 10),
        // text(job.nodes()) | flex,
        // text(job.nodelist_or_reason()()) | flex,
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
    std::vector<Job> jobs = {
        Job("1", "DD", "running", "user1", "00:10:00", "1", "node1"),
        Job("2", "EE", "pending", "user2", "00:00:00", "1", "node2"),
        Job("3", "FF", "completed", "user3", "00:20:00", "1", "node3"),
    };

    m_jobs = jobs;

    return jobs;
}
