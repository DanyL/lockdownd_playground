// Quick & dirty depth aware logger
// Abusing compiler instrumentation capabilities to gain call depth awareness.
// Compile with "-finstrument-functions" flag enabled
//
// To "escape" the depth counter, use the following compiler attribute:
// __attribute__((no_instrument_function))

#include "log.h"
#include <libgen.h>
#include <math.h>
#include <stdarg.h>
#include <unistd.h>

const int LOG_PROGRESS_BAR_WIDTH = 40;
int g_log_depth     = 0;
int g_log_verbosity = 0;

__attribute__((no_instrument_function)) void
__cyg_profile_func_enter(void *des, void *src_call)
{
    g_log_depth++;
}

__attribute__((no_instrument_function)) void
__cyg_profile_func_exit(void *des, void *src_call)
{
    g_log_depth--;
}

__attribute__((no_instrument_function)) void
__indent_with_depth(FILE *stream, int use_colors, int color)
{
    (use_colors) ? fprintf(stream, " \e[%d;1m", color) : fprintf(stream, " ");
    for (int i = 1; i < g_log_depth; i++) fprintf(stream, "|---");
    fprintf(stream, (use_colors) ? "|\e[0m " : "| ");
}

__attribute__((no_instrument_function)) void
__log_with_depth(FILE *stream, const char *file, int linenum, const char *func, int color, const char *type, const char *fmt, ...)
{
    int use_colors = isatty(fileno(stream));

    __indent_with_depth(stream, use_colors, color);

    if (g_log_verbosity)
    {
        fprintf(stream, "[");
        if (2 < g_log_verbosity)
        {
            if (use_colors) fprintf(stream, "\e[%d;2m", color);
            fprintf(stream, "%s/", dirname((char *)file));
        }
        if (use_colors) fprintf(stream, "\e[%dm", color);
        fprintf(stream, "%s", basename((char *)file));
        if (use_colors) fprintf(stream, "\e[0m");
        fprintf(stream, ":");
        if (use_colors) fprintf(stream, "\e[%d;1m", color);
        fprintf(stream, "%s", func);
        if (use_colors) fprintf(stream, "\e[0m");
        fprintf(stream, "():");
        if (use_colors) fprintf(stream, "\e[%dm", color);
        fprintf(stream, "%d", linenum);
        if (use_colors) fprintf(stream, "\e[0m");
        fprintf(stream, "] ");
    }

    if (use_colors)
        fprintf(stream, "\e[%d;2m<\e[0m\e[%d;1m%s\e[0m\e[%d;2m>\e[0m: ", color, color, type, color);
    else
        fprintf(stream, "<%s>: ", type);

    va_list(args);
    va_start(args, fmt);
    vfprintf(stream, fmt, args);
    va_end(args);

    fprintf(stream, "\n");
}

__attribute__((no_instrument_function)) void
log_progress(double percentage, char *status, int error)
{
    double progress = 0;
    int completed = 0;

    // No progress bars for you
    if (!isatty(fileno(stdout))) return;

    fprintf(stdout, "\33[2K\r");

    progress = round(percentage / 100 * LOG_PROGRESS_BAR_WIDTH);

    fprintf(stdout, "%3.0f%% |", percentage);
    for (int i = 0; i < LOG_PROGRESS_BAR_WIDTH; i++)
        fprintf(stdout, "%s", (i < progress) ? "\e[37;7m \e[0m" : " ");

    fprintf(stdout, "|");
    if (100 == percentage) {
        completed = 1;
        if (NULL == status) status = "Complete";
    }

    if (NULL != status) fprintf(stdout, "\e[37;2m - %s\e[0m", status);
    if (completed || error) fputc('\n', stdout);

    fflush(stdout);
}
