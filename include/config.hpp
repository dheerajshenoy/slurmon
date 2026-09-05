#pragma once

struct Config
{
    struct JobView
    {
        bool show = true;
        bool loop_after_end
            = false; // Whether to loop the job list after reaching the end
        int refresh_interval = 5; // Seconds between squeue refreshes
    } job_view;

    struct Footer
    {
        bool show = true;
    } footer;

    struct LogView
    {
        bool show        = true;
        bool error_first = false; // Whether to show the error log first when
                                  // opening the log viewer
    } log_view;

    struct DetailView
    {
        bool show      = true;
        bool show_id   = true;
        bool show_name = true;
        bool show_state = true;
        bool show_user = true;
        bool show_time = true;
        bool show_nodes = true;
        bool show_nodelist = true;
    } detail_view;
};
