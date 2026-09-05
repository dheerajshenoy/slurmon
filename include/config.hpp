#pragma once

struct Config
{
    struct JobList
    {
        bool show = true;
        bool loop_after_end
            = false; // Whether to loop the job list after reaching the end
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
};
