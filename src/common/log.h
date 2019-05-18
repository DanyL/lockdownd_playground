// Quick & dirty depth aware logger
// Abusing compiler instrumentation capabilities to gain call depth awareness.
// Compile with "-finstrument-functions" flag enabled
//
// To "escape" the depth counter, use the following compiler attribute:
// __attribute__((no_instrument_function))

#pragma once

#include <stdio.h>

extern int g_log_depth;
extern int g_log_verbosity;

void
__indent_with_depth(FILE *stream, int use_colors, int color);

void
__log_with_depth(FILE *stream, const char *file, int linenum, const char *func, int color, const char *type, const char *fmt, ...);

void
log_progress(double percentage, char *status, int error);

#define __log_with_depth_wrapper(stream, color, type, fmt, ...) \
__log_with_depth(stream, __FILE__, __LINE__, __func__, color, type, fmt, ##__VA_ARGS__)

#define log_info(fmt, ...) __log_with_depth_wrapper(stdout, 36, "INFO", fmt, ##__VA_ARGS__)
#define log_warning(fmt, ...) __log_with_depth_wrapper(stdout, 33, "WARNING", fmt, ##__VA_ARGS__)
#define log_error(fmt, ...) __log_with_depth_wrapper(stderr, 31, "ERROR", fmt, ##__VA_ARGS__)
#define log_debug(fmt, ...) if (1 < g_log_verbosity) __log_with_depth_wrapper(stderr, 35, "DEBUG", fmt, ##__VA_ARGS__)
