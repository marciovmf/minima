#ifndef MI_LOG_H
#define MI_LOG_H

#include <stdx_common.h>
#include <stdx_log.h>

/* Formatted logging */
#define mi_info_fmt(fmt, ...)     x_log_raw(stdout, XLOG_LEVEL_INFO, XLOG_COLOR_WHITE, XLOG_COLOR_BLACK, 0, fmt, __VA_ARGS__, 0)
#define mi_warning_fmt(fmt, ...)  x_log_raw(stdout, XLOG_LEVEL_INFO, XLOG_COLOR_YELLOW, XLOG_COLOR_BLACK, 0, fmt, __VA_ARGS__, 0)
#define mi_error_fmt(fmt, ...)    x_log_raw(stderr, XLOG_LEVEL_INFO, XLOG_COLOR_RED, XLOG_COLOR_BLACK, 0, fmt, __VA_ARGS__, 0)

/* Non-formatted logging (safe for arbitrary strings) */
#define mi_info(msg)    mi_info_fmt("%s", (msg))
#define mi_warning(msg) mi_warning_fmt("%s", (msg))
#define mi_error(msg)   mi_error_fmt("%s", (msg))

#endif // MI_LOG_H
