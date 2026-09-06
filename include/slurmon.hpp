#pragma once

#include "argparse.hpp"
#include "config.hpp"
#include "job.hpp"
#include "toml.hpp"

#include <atomic>
#include <chrono>
#include <mutex>
#include <unordered_map>

class Slurmon
{
public:
    Slurmon(int argc, char **argv);
    ~Slurmon();
    void run();

    enum class ViewMode
    {
        Live,
        History,
    };

    enum class SortKey
    {
        None,
        Id,
        Name,
        State,
        Time,
    };

private:
    struct LogPaths
    {
        std::string stdout_path;
        std::string stderr_path;
    };

    void init_args() noexcept;
    void init_ui() noexcept;
    void init_config() noexcept;
    void parse_args() noexcept;
    ftxui::Elements build_rows(const std::vector<const Job *> &jobs);

    static std::vector<Job> fetch_jobs();
    static std::vector<Job> fetch_history_jobs();
    static LogPaths fetch_log_paths(const std::string &job_id);
    static std::string read_tail(const std::string &path, size_t max_lines);
    static bool cancel_job(const std::string &job_id);
    const LogPaths &log_paths_for(const std::string &job_id);

private:
    argparse::ArgumentParser m_argparse = argparse::ArgumentParser(
        "slurmon", SLURMON_VERSION, argparse::default_arguments::help);
    int m_selected_row = -1;
    int m_split_size   = -1;
    std::vector<Job> m_jobs;
    std::vector<const Job *> m_view_cache;
    bool m_view_dirty = true;
    std::mutex m_jobs_mutex;
    std::atomic<bool> m_running = true;

    ViewMode m_view_mode      = ViewMode::Live;
    SortKey m_sort_key        = SortKey::None;
    bool m_sort_descending    = false;
    bool m_loop_after_end     = false;
    bool m_show_footer        = true;
    bool m_show_stderr        = false;
    bool m_show_help_dialog   = false;
    bool m_search_mode        = false;
    bool m_show_cancel_dialog = false;

    std::chrono::steady_clock::time_point m_pending_g_time;

    std::string m_search_buffer;
    std::string m_search_query;
    std::string m_cancel_target_id;
    std::string m_cancel_target_name;
    std::string m_cancel_status;
    std::unordered_map<std::string, LogPaths> m_log_paths_cache;
    std::string m_config_path;

    Config m_config;
};
