#pragma once

#include "job.hpp"

#include <atomic>
#include <mutex>

class Slurmon
{
public:
    Slurmon();
    ~Slurmon();
    void run();

private:
    void init_ui() noexcept;
    ftxui::Element build_row(const Job &job, bool selected);
    ftxui::Elements build_rows(const std::vector<Job> &jobs);
    static std::vector<Job> fetch_jobs();

    int m_selected_row;
    std::vector<Job> m_jobs;
    std::mutex m_jobs_mutex;
    std::atomic<bool> m_running{true};
    bool m_show_footer = true;
};
