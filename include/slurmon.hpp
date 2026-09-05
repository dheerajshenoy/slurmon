#pragma once

#include "job.hpp"

#include <atomic>
#include <chrono>
#include <mutex>
#include <unordered_map>

class Slurmon
{
public:
    Slurmon();
    ~Slurmon();
    void run();

private:
    struct LogPaths
    {
        std::string stdout_path;
        std::string stderr_path;
    };

    void init_ui() noexcept;
    ftxui::Element build_row(const Job &job, bool selected);
    ftxui::Elements build_rows(const std::vector<Job> &jobs);
    static std::vector<Job> fetch_jobs();
    static LogPaths fetch_log_paths(const std::string &job_id);
    static std::string read_tail(const std::string &path, size_t max_lines);
    static bool cancel_job(const std::string &job_id);

    const LogPaths &log_paths_for(const std::string &job_id);

    int m_selected_row;
    std::vector<Job> m_jobs;
    std::mutex m_jobs_mutex;
    std::atomic<bool> m_running{true};
    bool m_show_footer = true;
    bool m_show_stderr = false;
    int m_split_size   = 60;
    std::chrono::steady_clock::time_point m_pending_g_time{};
    bool m_show_help_dialog   = false;
    bool m_show_cancel_dialog = false;
    std::string m_cancel_target_id;
    std::string m_cancel_target_name;
    std::string m_cancel_status;
    std::unordered_map<std::string, LogPaths> m_log_paths_cache;
};
