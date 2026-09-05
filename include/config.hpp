#pragma once

struct Config
{
    struct JobList
    {
        bool show = true;
        bool loop_after_end
            = false; // Whether to loop the job list after reaching the end
        int refresh_interval = 5; // Seconds between squeue refreshes
    } job_list;

    struct Footer
    {
        bool show = true;
    } footer;

    struct LogViewer
    {
        bool show        = true;
        bool error_first = false; // Whether to show the error log first when
                                  // opening the log viewer
    } log_viewer;

    struct DetailsView
    {
        bool show      = true;
        bool show_id   = true;
        bool show_name = true;
        bool show_state = true;
        bool show_user = true;
        bool show_time = true;
        bool show_nodes = true;
        bool show_nodelist = true;
    } details_view;
};
