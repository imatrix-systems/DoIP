/**
 * @file doip_log.h
 * @brief DoIP Server structured logging with file rotation
 *
 * Thread-safe leveled logging to file and stderr.
 * Log rotation: 5 files, 1 MB each.
 */

#ifndef DOIP_LOG_H
#define DOIP_LOG_H

#include <stdarg.h>

/** @brief Log level threshold — messages below configured level are discarded */
typedef enum {
    DOIP_LOG_ERROR = 0,   /**< Always shown — fatal or unrecoverable */
    DOIP_LOG_WARN  = 1,   /**< Recoverable problems */
    DOIP_LOG_INFO  = 2,   /**< Normal operations (default) */
    DOIP_LOG_DEBUG = 3,   /**< Protocol-level detail */
} doip_log_level_t;

/**
 * @brief Initialize the logging system
 * @param log_file_path  Path to log file (NULL for console-only)
 * @param level          Log level threshold
 * @return 0 on success, -1 on critical failure (mutex init)
 * @note Path must not contain ".." (path traversal rejected)
 */
int doip_log_init(const char *log_file_path, doip_log_level_t level);

/**
 * @brief Shut down the logging system, flush and close log file
 * @note MUST be called only after all server threads have exited
 *       (i.e., after doip_server_destroy() returns)
 */
void doip_log_shutdown(void);

/**
 * @brief Log a message at the specified level
 * @param level  Log level for this message
 * @param fmt    printf-style format string
 * @param ...    Format arguments
 *
 * Thread-safe. Messages below the configured level are discarded
 * before formatting (no wasted cycles).
 */
void doip_log(doip_log_level_t level, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

/** @name Convenience macros */
/**@{*/
#define LOG_ERROR(...)  doip_log(DOIP_LOG_ERROR, __VA_ARGS__)
#define LOG_WARN(...)   doip_log(DOIP_LOG_WARN,  __VA_ARGS__)
#define LOG_INFO(...)   doip_log(DOIP_LOG_INFO,  __VA_ARGS__)
#define LOG_DEBUG(...)  doip_log(DOIP_LOG_DEBUG, __VA_ARGS__)
/**@}*/

#endif /* DOIP_LOG_H */
