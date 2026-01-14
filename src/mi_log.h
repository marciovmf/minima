#ifndef MI_LOG_H
#define MI_LOG_H

#include <stdx_common.h>
#include <stdx_log.h>

#define mi_info_fmt(msg, ...)     x_log_raw(stdout, XLOG_LEVEL_INFO, XLOG_COLOR_WHITE, XLOG_COLOR_BLACK, 0, msg, __VA_ARGS__, 0)
#define mi_warning_fmt(msg, ...)  x_log_raw(stdout, XLOG_LEVEL_INFO, XLOG_COLOR_YELLOW, XLOG_COLOR_BLACK, 0, msg, __VA_ARGS__, 0)
#define mi_error_fmt(msg, ...)    x_log_raw(stderr, XLOG_LEVEL_INFO, XLOG_COLOR_RED, XLOG_COLOR_BLACK, 0, msg, __VA_ARGS__, 0)

#define mi_info(msg) mi_info_fmt(msg, 0)
#define mi_warning(msg) mi_warning_fmt(msg, 0)
#define mi_error(msg) mi_error_fmt(msg, 0)

#endif //MI_LOG_H

