#define _POSIX_C_SOURCE 200809L

/**
 * @file doip_log.c
 * @brief DoIP Server structured logging with file rotation
 *
 * Thread-safe logging to /var/FC-1-DOIP.log (configurable) and stderr.
 * Automatic rotation: 5 files max, 1 MB each.
 * Control character sanitization prevents terminal escape injection.
 */

#include "doip_log.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <errno.h>

/* ============================================================================
 * Constants
 * ========================================================================== */

#define LOG_LINE_MAX        1024          /**< Max formatted message length */
#define LOG_MAX_FILE_SIZE   (1024 * 1024) /**< 1 MB per file */
#define LOG_MAX_FILES       5             /**< active + 4 rotated */

/** @brief Level name strings — 5 chars wide, right-padded */
static const char *const LEVEL_NAMES[] = {"ERROR", "WARN ", "INFO ", "DEBUG"};

/* ============================================================================
 * Static State
 * ========================================================================== */

static struct {
    FILE               *fp;           /**< Current log file handle (NULL = console-only) */
    char                path[256];    /**< Log file path (owned copy) */
    doip_log_level_t    level;        /**< Configured level threshold */
    pthread_mutex_t     mutex;        /**< Thread safety for file I/O */
    uint32_t            current_size; /**< Bytes written to current file */
    _Atomic bool        initialized;  /**< Thread-safe init guard */
} g_log;

/* ============================================================================
 * sanitize_line — scrub control characters to prevent terminal injection
 * ========================================================================== */

/**
 * @brief Replace control characters with '?' in-place
 * @param line  Null-terminated string to sanitize
 *
 * Replaces bytes 0x00-0x08, 0x0B-0x1F, 0x7F with '?'.
 * Preserves 0x09 (tab) and 0x0A (newline).
 */
static void sanitize_line(char *line)
{
    for (char *p = line; *p != '\0'; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == 0x09 || c == 0x0A) continue;  /* keep tab and newline */
        if (c < 0x20 || c == 0x7F) *p = '?';
    }
}

/* ============================================================================
 * rotate_log_locked — caller must hold g_log.mutex
 * ========================================================================== */

/**
 * @brief Rotate log files: .log -> .log.1 -> .log.2 -> ... -> .log.4
 *
 * Closes current file, shifts existing rotated files up by one index,
 * deletes the oldest (.log.4), and opens a fresh active file.
 * Falls back to console-only if the new file cannot be opened.
 */
static void rotate_log_locked(void)
{
    /* Close current file */
    if (g_log.fp) {
        fclose(g_log.fp);
        g_log.fp = NULL;
    }

    /* Shift rotated files: .4 <- .3 <- .2 <- .1 <- active */
    char old_name[300];
    char new_name[300];

    for (int i = LOG_MAX_FILES - 1; i >= 1; i--) {
        snprintf(new_name, sizeof(new_name), "%s.%d", g_log.path, i);

        /* Delete oldest */
        if (i == LOG_MAX_FILES - 1) {
            remove(new_name);  /* ignore ENOENT */
        }

        /* Build source name */
        if (i == 1) {
            snprintf(old_name, sizeof(old_name), "%s", g_log.path);
        } else {
            snprintf(old_name, sizeof(old_name), "%s.%d", g_log.path, i - 1);
        }

        if (rename(old_name, new_name) != 0 && errno != ENOENT) {
            fprintf(stderr, "doip_log: rename %s -> %s failed: %s\n",
                    old_name, new_name, strerror(errno));
        }
    }

    /* Open fresh active file */
    g_log.fp = fopen(g_log.path, "w");
    if (g_log.fp) {
        fchmod(fileno(g_log.fp), 0640);
    } else {
        fprintf(stderr, "doip_log: rotation failed, file logging lost\n");
    }

    g_log.current_size = 0;
}

/* ============================================================================
 * Public API
 * ========================================================================== */

/**
 * @brief Initialize the logging system
 *
 * Opens the log file in append mode, creates parent directory if needed,
 * and sets permissions to 0640.
 *
 * @param log_file_path  Path to log file (NULL for console-only)
 * @param level          Log level threshold (DOIP_LOG_ERROR..DOIP_LOG_DEBUG)
 * @return 0 on success, -1 if mutex init fails
 */
int doip_log_init(const char *log_file_path, doip_log_level_t level)
{
    memset(&g_log, 0, sizeof(g_log));
    atomic_store(&g_log.initialized, false);
    g_log.level = level;

    /* Initialize mutex — critical, fail hard if broken */
    if (pthread_mutex_init(&g_log.mutex, NULL) != 0) {
        fprintf(stderr, "doip_log: mutex init failed\n");
        return -1;
    }

    if (log_file_path != NULL) {
        /* Path traversal check */
        if (strstr(log_file_path, "..") != NULL) {
            fprintf(stderr, "doip_log: path contains '..', rejected: %s\n",
                    log_file_path);
            /* Fall through to console-only */
        } else {
            strncpy(g_log.path, log_file_path, sizeof(g_log.path) - 1);
            g_log.path[sizeof(g_log.path) - 1] = '\0';

            /* Create parent directory if needed */
            char dir_buf[256];
            strncpy(dir_buf, g_log.path, sizeof(dir_buf) - 1);
            dir_buf[sizeof(dir_buf) - 1] = '\0';
            char *last_slash = strrchr(dir_buf, '/');
            if (last_slash && last_slash != dir_buf) {
                *last_slash = '\0';
                mkdir(dir_buf, 0755);  /* ignore EEXIST */
            }

            /* Open log file in append mode */
            g_log.fp = fopen(g_log.path, "a");
            if (g_log.fp) {
                fchmod(fileno(g_log.fp), 0640);
                /* Get current file size for rotation tracking */
                fseek(g_log.fp, 0, SEEK_END);
                long pos = ftell(g_log.fp);
                g_log.current_size = (pos >= 0) ? (uint32_t)pos : 0;
            } else {
                fprintf(stderr, "doip_log: cannot open %s: %s\n",
                        g_log.path, strerror(errno));
                /* Continue with console-only */
            }
        }
    }

    atomic_store(&g_log.initialized, true);
    LOG_INFO("=== DoIP Log started (level=%s) ===",
             LEVEL_NAMES[(int)level]);
    return 0;
}

/**
 * @brief Shut down the logging system
 *
 * Precondition: all server threads must be stopped (doip_server_destroy()
 * has returned) before calling this function.
 */
void doip_log_shutdown(void)
{
    if (!atomic_load(&g_log.initialized)) return;

    LOG_INFO("=== DoIP Log stopped ===");

    pthread_mutex_lock(&g_log.mutex);
    atomic_store(&g_log.initialized, false);

    if (g_log.fp) {
        fflush(g_log.fp);
        fclose(g_log.fp);
        g_log.fp = NULL;
    }

    pthread_mutex_unlock(&g_log.mutex);
    pthread_mutex_destroy(&g_log.mutex);
}

/**
 * @brief Log a message at the specified level
 *
 * Thread-safe. Writes to both the log file (if open) and stderr.
 * Messages below the configured level are discarded before formatting.
 * Timestamps use ISO 8601 local time with millisecond resolution.
 *
 * @param level  Log level for this message
 * @param fmt    printf-style format string
 * @param ...    Format arguments
 */
void doip_log(doip_log_level_t level, const char *fmt, ...)
{
    /* Fast path: skip if not initialized or level filtered */
    if (!atomic_load(&g_log.initialized)) return;
    if (level > g_log.level) return;

    /* Build timestamp outside mutex for minimal lock time */
    char timestamp[64];
    struct timeval tv;
    gettimeofday(&tv, NULL);

    struct tm tm_buf;
    struct tm *tm = localtime_r(&tv.tv_sec, &tm_buf);
    if (tm) {
        snprintf(timestamp, sizeof(timestamp),
                 "%04d-%02d-%02dT%02d:%02d:%02d.%03d",
                 tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
                 tm->tm_hour, tm->tm_min, tm->tm_sec,
                 (int)(tv.tv_usec / 1000));
    } else {
        snprintf(timestamp, sizeof(timestamp), "0000-00-00T00:00:00.000");
    }

    /* Format message */
    char msg[LOG_LINE_MAX];
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);

    /* Truncation indicator */
    if (n >= (int)sizeof(msg)) {
        msg[sizeof(msg) - 4] = '.';
        msg[sizeof(msg) - 3] = '.';
        msg[sizeof(msg) - 2] = '.';
    }

    /* Build full line: [timestamp] [LEVEL] message */
    char line[LOG_LINE_MAX + 128];
    int level_idx = (level >= 0 && level <= DOIP_LOG_DEBUG) ? (int)level : 0;
    snprintf(line, sizeof(line), "[%s] [%s] %s\n",
             timestamp, LEVEL_NAMES[level_idx], msg);

    /* Sanitize control characters */
    sanitize_line(line);

    /* Write to file under mutex */
    pthread_mutex_lock(&g_log.mutex);
    if (g_log.fp) {
        if (fputs(line, g_log.fp) == EOF) {
            clearerr(g_log.fp);
        } else {
            fflush(g_log.fp);
            g_log.current_size += (uint32_t)strlen(line);
            if (g_log.current_size >= LOG_MAX_FILE_SIZE) {
                rotate_log_locked();
            }
        }
    }
    pthread_mutex_unlock(&g_log.mutex);

    /* Write to console (stderr) — outside mutex, stderr has own lock */
    fputs(line, stderr);
}
