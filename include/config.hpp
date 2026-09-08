#pragma once

#include <string>
#include <vector>

struct Config
{
    struct JobView
    {
        bool show = true;
        bool loop_after_end
            = false; // Whether to loop the job list after reaching the end
        int refresh_interval = 5; // Seconds between squeue refreshes
        std::string sort_by  = "none"; // none|id|name|state|time
        bool sort_descending = false;
        // Columns to display, in order. Recognised keys:
        // id, name, state, user, time, nodes, nodelist
        std::vector<std::string> columns = {"id", "name", "state", "time"};
        // Fraction (0.05 - 0.95) of the terminal width given to the job list
        // pane at startup; the remainder goes to details/logs.
        double split_fraction = 0.5;
        // When true, every column is sized to fit its widest value (plus
        // the header), overriding the per-column base widths.
        bool fit_content_width = true;
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
        bool show = true;
        // Fields shown in the details pane, in order. Same key set as
        // job_view.columns (see all_job_columns()).
        std::vector<std::string> columns
            = {"id", "name", "state", "user", "time", "nodes", "nodelist"};
    } detail_view;
};
