#define _POSIX_C_SOURCE 200809L

/**
 * @file main.c
 * @brief DoIP Blob Server — receives data blobs from Fleet-Connect-1
 *
 * Handles UDS services:
 *   0x34 RequestDownload   — initiate blob transfer
 *   0x36 TransferData      — receive blob chunks
 *   0x37 RequestTransferExit — finalize, verify CRC-32, store to disk
 *   0x3E TesterPresent     — keepalive
 *   0x31 RoutineControl    — phone-home trigger
 *
 * Modes:
 *   Foreground (default) — interactive CLI when TTY, sleep loop otherwise
 *   Daemon (-d)          — double-fork, PID file, headless
 *
 * Usage: ./doip-server [-c config_file] [-d] [-v|-vv] [-q|-qq] [-l logfile] [bind_ip] [port]
 */

/* Version injected by Makefile via -DDOIP_SERVER_VERSION=... */
#ifndef DOIP_SERVER_VERSION
#define DOIP_SERVER_VERSION "0.0.0-dev"
#endif

#include "doip.h"
#include "doip_server.h"
#include "config.h"
#include "phonehome_handler.h"
#include "doip_log.h"
#include "cli.h"
#include "script_gen.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <fcntl.h>
#include <errno.h>
#include <pthread.h>
#include <sys/select.h>
#include <sys/time.h>

/* ============================================================================
 * Global Configuration
 * ========================================================================== */

static doip_app_config_t g_app_config;
static time_t g_server_start_time;   /* For uptime calculation in status queries */

/* ============================================================================
 * CRC-32 (standalone, polynomial 0xEDB88320 — same as zlib/IEEE 802.3)
 * ========================================================================== */

static uint32_t crc32_table[256];

static void crc32_init_table(void)
{
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t crc = i;
        for (int j = 0; j < 8; j++) {
            if (crc & 1)
                crc = (crc >> 1) ^ 0xEDB88320;
            else
                crc = crc >> 1;
        }
        crc32_table[i] = crc;
    }
}

/* Table must be initialized by crc32_init_table() before first call */
static uint32_t crc32_compute(const uint8_t *data, uint32_t len)
{
    uint32_t crc = 0xFFFFFFFF;
    for (uint32_t i = 0; i < len; i++)
        crc = (crc >> 8) ^ crc32_table[(crc ^ data[i]) & 0xFF];
    return crc ^ 0xFFFFFFFF;
}

/* ============================================================================
 * Transfer State (per-server, single concurrent transfer)
 * Protected by g_transfer_mutex — accessed from client handler threads
 * and the main loop timeout check.
 * ========================================================================== */

typedef struct {
    bool        active;
    uint32_t    memory_address;
    uint32_t    memory_size;        /* Expected total bytes */
    uint32_t    bytes_received;
    uint8_t     block_sequence;     /* Expected next BSC */
    uint16_t    max_block_length;   /* Advertised to client */
    uint8_t    *buffer;             /* Reassembly buffer */
    time_t      last_activity;      /* For timeout detection */
} transfer_state_t;

static transfer_state_t g_transfer;
static pthread_mutex_t g_transfer_mutex = PTHREAD_MUTEX_INITIALIZER;

/* ============================================================================
 * UDS Response Helper
 * ========================================================================== */

static int build_negative_response(uint8_t sid, uint8_t nrc,
                                   uint8_t *resp, uint32_t resp_size)
{
    if (resp_size < 3) return -1;
    resp[0] = 0x7F;
    resp[1] = sid;
    resp[2] = nrc;
    return 3;
}

/* ============================================================================
 * Blob Storage
 * ========================================================================== */

static int ensure_storage_dir(void)
{
    struct stat st;
    if (stat(g_app_config.blob_storage_dir, &st) == 0) {
        if (!S_ISDIR(st.st_mode)) {
            LOG_ERROR("%s exists but is not a directory",
                      g_app_config.blob_storage_dir);
            return -1;
        }
        return 0;
    }
    if (mkdir(g_app_config.blob_storage_dir, 0755) != 0 && errno != EEXIST) {
        LOG_ERROR("Failed to create %s: %s",
                  g_app_config.blob_storage_dir, strerror(errno));
        return -1;
    }
    return 0;
}

static void save_blob(const uint8_t *data, uint32_t size, uint32_t addr)
{
    time_t now = time(NULL);
    struct tm tm_buf;
    struct tm *tm = localtime_r(&now, &tm_buf);
    if (!tm) {
        LOG_ERROR("localtime_r() failed");
        return;
    }

    char filename[512];
    snprintf(filename, sizeof(filename),
             "%s/%04d-%02d-%02d_%02d%02d%02d_addr_%08X_%ubytes.bin",
             g_app_config.blob_storage_dir,
             tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
             tm->tm_hour, tm->tm_min, tm->tm_sec,
             addr, size);

    FILE *f = fopen(filename, "wb");
    if (!f) {
        LOG_ERROR("Failed to open blob file: %s", strerror(errno));
        return;
    }

    size_t written = fwrite(data, 1, size, f);
    if (fclose(f) != 0) {
        LOG_ERROR("fclose failed: %s", strerror(errno));
    }

    if (written == size) {
        LOG_INFO("Saved: %s", filename);
    } else {
        LOG_WARN("Wrote %zu of %u bytes to %s", written, size, filename);
    }
}

/* ============================================================================
 * Transfer Cleanup (caller must hold g_transfer_mutex)
 * ========================================================================== */

static void transfer_cleanup_locked(void)
{
    if (g_transfer.buffer) {
        free(g_transfer.buffer);
        g_transfer.buffer = NULL;
    }
    g_transfer.active = false;
    g_transfer.bytes_received = 0;
    g_transfer.block_sequence = 1;
    g_transfer.memory_size = 0;
}

/* ============================================================================
 * UDS Service Handlers (caller must hold g_transfer_mutex)
 * ========================================================================== */

static int handle_request_download(const uint8_t *uds_data, uint32_t uds_len,
                                   uint8_t *response, uint32_t resp_size)
{
    uint8_t sid = 0x34;

    if (g_transfer.active) {
        LOG_WARN("RequestDownload rejected: transfer already active");
        return build_negative_response(sid, 0x70, response, resp_size); /* uploadDownloadNotAccepted */
    }

    /* Parse: SID(1) + dataFormatIdentifier(1) + addressAndLengthFormatIdentifier(1) + addr + size */
    if (uds_len < 4)
        return build_negative_response(sid, 0x13, response, resp_size); /* incorrectMessageLength */

    uint8_t addr_and_len_fmt = uds_data[2];
    uint8_t mem_size_len = (addr_and_len_fmt >> 4) & 0x0F;
    uint8_t mem_addr_len = addr_and_len_fmt & 0x0F;

    if (mem_addr_len == 0 || mem_addr_len > 4 || mem_size_len == 0 || mem_size_len > 4)
        return build_negative_response(sid, 0x31, response, resp_size); /* requestOutOfRange */

    if (uds_len < (uint32_t)(3 + mem_addr_len + mem_size_len))
        return build_negative_response(sid, 0x13, response, resp_size);

    /* Extract memory address */
    uint32_t mem_addr = 0;
    for (uint8_t i = 0; i < mem_addr_len; i++)
        mem_addr = (mem_addr << 8) | uds_data[3 + i];

    /* Extract memory size */
    uint32_t mem_size = 0;
    for (uint8_t i = 0; i < mem_size_len; i++)
        mem_size = (mem_size << 8) | uds_data[3 + mem_addr_len + i];

    /* Validate size */
    if (mem_size == 0 || mem_size > g_app_config.blob_max_size) {
        LOG_WARN("RequestDownload rejected: size %u exceeds max (%u)",
                 mem_size, g_app_config.blob_max_size);
        return build_negative_response(sid, 0x70, response, resp_size);
    }

    /* Allocate reassembly buffer */
    g_transfer.buffer = (uint8_t *)malloc(mem_size);
    if (!g_transfer.buffer) {
        LOG_ERROR("RequestDownload rejected: malloc(%u) failed", mem_size);
        return build_negative_response(sid, 0x70, response, resp_size);
    }

    /* Set up transfer state.
     * max_block_length must not exceed DOIP_MAX_DIAGNOSTIC_SIZE (4096) since
     * the DoIP transport layer caps UDS payloads at that size. */
    g_transfer.active = true;
    g_transfer.memory_address = mem_addr;
    g_transfer.memory_size = mem_size;
    g_transfer.bytes_received = 0;
    g_transfer.block_sequence = 1;
    g_transfer.max_block_length = DOIP_MAX_DIAGNOSTIC_SIZE;
    g_transfer.last_activity = time(NULL);

    LOG_INFO("RequestDownload accepted: addr=0x%08X, size=%u",
             mem_addr, mem_size);

    /* Build positive response: SID(0x74) + lengthFormatIdentifier + maxBlockLength */
    if (resp_size < 4)
        return -1;
    response[0] = 0x74;
    response[1] = 0x20;  /* 2 bytes for max block length */
    response[2] = (uint8_t)(g_transfer.max_block_length >> 8);
    response[3] = (uint8_t)(g_transfer.max_block_length & 0xFF);
    return 4;
}

static int handle_transfer_data(const uint8_t *uds_data, uint32_t uds_len,
                                uint8_t *response, uint32_t resp_size)
{
    uint8_t sid = 0x36;

    if (!g_transfer.active)
        return build_negative_response(sid, 0x24, response, resp_size); /* requestSequenceError */

    if (uds_len < 2)
        return build_negative_response(sid, 0x13, response, resp_size);

    uint8_t block_seq = uds_data[1];

    /* Validate block sequence counter */
    if (block_seq != g_transfer.block_sequence) {
        LOG_WARN("TransferData wrong sequence: expected %u, got %u",
                 g_transfer.block_sequence, block_seq);
        return build_negative_response(sid, 0x73, response, resp_size); /* wrongBlockSequenceCounter */
    }

    uint32_t data_in_block = uds_len - 2;

    if (g_transfer.bytes_received + data_in_block > g_transfer.memory_size) {
        LOG_WARN("TransferData exceeds requested size");
        return build_negative_response(sid, 0x71, response, resp_size); /* requestOutOfRange */
    }

    /* Copy data into reassembly buffer */
    memcpy(&g_transfer.buffer[g_transfer.bytes_received], &uds_data[2], data_in_block);
    g_transfer.bytes_received += data_in_block;
    g_transfer.block_sequence++;
    g_transfer.last_activity = time(NULL);

    LOG_DEBUG("TransferData block %u: %u bytes (total: %u/%u)",
              block_seq, data_in_block,
              g_transfer.bytes_received, g_transfer.memory_size);

    /* Positive response */
    if (resp_size < 2)
        return -1;
    response[0] = 0x76;
    response[1] = block_seq;
    return 2;
}

static int handle_transfer_exit(const uint8_t *uds_data, uint32_t uds_len,
                                uint8_t *response, uint32_t resp_size)
{
    (void)uds_data;
    (void)uds_len;
    uint8_t sid = 0x37;

    if (!g_transfer.active)
        return build_negative_response(sid, 0x24, response, resp_size);

    LOG_INFO("TransferExit: %u/%u bytes received",
             g_transfer.bytes_received, g_transfer.memory_size);

    int result;

    /* Verify CRC-32: last 4 bytes of received data are the expected CRC */
    if (g_transfer.bytes_received >= 4) {
        uint32_t data_len = g_transfer.bytes_received - 4;
        uint32_t expected_crc;
        memcpy(&expected_crc, &g_transfer.buffer[data_len], 4);

        uint32_t computed_crc = crc32_compute(g_transfer.buffer, data_len);

        if (computed_crc == expected_crc) {
            LOG_INFO("CRC-32 verified: OK (0x%08X)", computed_crc);

            /* Save the actual data (without CRC suffix) */
            save_blob(g_transfer.buffer, data_len, g_transfer.memory_address);

            /* Positive response */
            if (resp_size < 1)
                result = -1;
            else {
                response[0] = 0x77;
                result = 1;
            }
        } else {
            LOG_ERROR("CRC-32 MISMATCH: computed=0x%08X, expected=0x%08X",
                      computed_crc, expected_crc);
            LOG_ERROR("Blob discarded (CRC failure)");
            result = build_negative_response(sid, 0x72, response, resp_size); /* generalProgrammingFailure */
        }
    } else {
        /* No CRC suffix — save as-is (shouldn't happen with proper client) */
        LOG_WARN("Blob too small for CRC verification (%u bytes)",
                 g_transfer.bytes_received);
        save_blob(g_transfer.buffer, g_transfer.bytes_received, g_transfer.memory_address);

        if (resp_size < 1)
            result = -1;
        else {
            response[0] = 0x77;
            result = 1;
        }
    }

    transfer_cleanup_locked();
    return result;
}

/* ============================================================================
 * Diagnostic Message Handler
 * ========================================================================== */

static int handle_diagnostic(doip_server_t *server,
                             uint16_t source_addr, uint16_t target_addr,
                             const uint8_t *uds_data, uint32_t uds_len,
                             uint8_t *response, uint32_t resp_size)
{
    (void)server;
    (void)target_addr;

    if (uds_len < 1 || resp_size < 3)
        return -1;

    uint8_t sid = uds_data[0];

    LOG_INFO("UDS request from 0x%04X: SID=0x%02X, len=%u",
             source_addr, sid, uds_len);

    int result;
    switch (sid) {
    case 0x34: /* RequestDownload */
        pthread_mutex_lock(&g_transfer_mutex);
        result = handle_request_download(uds_data, uds_len, response, resp_size);
        pthread_mutex_unlock(&g_transfer_mutex);
        break;

    case 0x36: /* TransferData */
        pthread_mutex_lock(&g_transfer_mutex);
        result = handle_transfer_data(uds_data, uds_len, response, resp_size);
        pthread_mutex_unlock(&g_transfer_mutex);
        break;

    case 0x37: /* RequestTransferExit */
        pthread_mutex_lock(&g_transfer_mutex);
        result = handle_transfer_exit(uds_data, uds_len, response, resp_size);
        pthread_mutex_unlock(&g_transfer_mutex);
        break;

    case 0x3E: /* TesterPresent — no transfer mutex needed */
        if (resp_size < 2) {
            result = -1;
        } else if (uds_len >= 2 && (uds_data[1] & 0x80)) {
            result = 0; /* suppressPosRspMsgIndicationBit */
        } else {
            response[0] = 0x7E;
            response[1] = 0x00;
            result = 2;
        }
        break;

    case 0x31: /* RoutineControl — phone-home, no transfer mutex */
        if (uds_len >= 4 && uds_data[1] == 0x01) {
            uint16_t rid = (uint16_t)((uds_data[2] << 8) | uds_data[3]);
            if (rid == ROUTINE_ID_PHONEHOME) {
                result = phonehome_handle_routine(uds_data, uds_len,
                                                  response, resp_size);
            } else if (rid == ROUTINE_ID_PHONEHOME_PROVISION) {
                result = phonehome_handle_provision(uds_data, uds_len,
                                                    response, resp_size);
            } else if (rid == ROUTINE_ID_PHONEHOME_STATUS) {
                result = phonehome_handle_status(uds_data, uds_len,
                                                  response, resp_size,
                                                  g_server_start_time);
            } else {
                result = build_negative_response(sid, 0x12, response, resp_size);
            }
        } else {
            result = build_negative_response(sid, 0x12, response, resp_size); /* subFunctionNotSupported */
        }
        break;

    default:
        result = build_negative_response(sid, 0x11, response, resp_size); /* serviceNotSupported */
        break;
    }

    return result;
}

/* ============================================================================
 * Routing Activation Handler
 * ========================================================================== */

static uint8_t handle_routing_activation(doip_server_t *server, int client_fd,
                                         uint16_t source_addr, uint8_t act_type)
{
    (void)server;
    (void)client_fd;
    LOG_INFO("Routing activation: SA=0x%04X, type=0x%02X — accepted",
             source_addr, act_type);
    return DOIP_ROUTING_ACTIVATION_SUCCESS;
}

/* ============================================================================
 * Signal Handler
 * ========================================================================== */

static volatile sig_atomic_t g_running = 1;

static void signal_handler(int sig)
{
    (void)sig;
    g_running = 0;
}

/* ============================================================================
 * Daemon Mode — PID File
 * ========================================================================== */

static int g_pid_fd = -1;  /* Kept open for flock lifetime */

static int write_pid_file(const char *path)
{
    g_pid_fd = open(path, O_WRONLY | O_CREAT | O_CLOEXEC, 0644);
    if (g_pid_fd < 0) {
        fprintf(stderr, "Cannot open PID file %s: %s\n", path, strerror(errno));
        return -1;
    }

    if (flock(g_pid_fd, LOCK_EX | LOCK_NB) < 0) {
        fprintf(stderr, "PID file %s locked — another instance running?\n", path);
        close(g_pid_fd);
        g_pid_fd = -1;
        return -1;
    }

    if (ftruncate(g_pid_fd, 0) != 0) {
        /* Non-fatal */
    }

    char buf[32];
    int n = snprintf(buf, sizeof(buf), "%d\n", getpid());
    if (n > 0) {
        ssize_t wr = write(g_pid_fd, buf, (size_t)n);
        (void)wr;
    }
    /* Keep fd open — flock released on close/exit */
    return 0;
}

static void remove_pid_file(const char *path)
{
    if (g_pid_fd >= 0) {
        close(g_pid_fd);  /* Releases flock */
        g_pid_fd = -1;
    }
    unlink(path);
}

/* ============================================================================
 * Daemon Mode — Double-Fork
 * ========================================================================== */

static int g_daemon_pipe_fd = -1;  /* Write end of status pipe, kept for deferred signaling */

/**
 * @brief Double-fork daemonize. Returns pipe write fd via g_daemon_pipe_fd.
 *
 * The caller MUST write a success byte (0) to g_daemon_pipe_fd after the
 * server has successfully bound its ports, then close it. On any error
 * before that, write a failure byte (1) and close.
 *
 * This ensures the original parent exits 0 only when the daemon is fully
 * operational, not just alive.
 */
static int daemonize(const char *pid_file_path)
{
    /* Create status pipe: grandchild writes success/failure back to parent */
    int status_pipe[2];
    if (pipe(status_pipe) < 0) {
        perror("pipe");
        return -1;
    }

    /* First fork */
    pid_t pid1 = fork();
    if (pid1 < 0) {
        perror("fork");
        close(status_pipe[0]);
        close(status_pipe[1]);
        return -1;
    }
    if (pid1 > 0) {
        /* Parent: wait for child status */
        close(status_pipe[1]);
        char status = 1;
        ssize_t n = read(status_pipe[0], &status, 1);
        close(status_pipe[0]);
        _exit((n == 1 && status == 0) ? 0 : 1);
    }

    /* First child: create new session */
    close(status_pipe[0]);  /* Close read end in child */
    setsid();
    signal(SIGHUP, SIG_IGN);

    /* Second fork — prevent reacquiring controlling terminal */
    pid_t pid2 = fork();
    if (pid2 < 0) {
        char fail = 1;
        ssize_t wr = write(status_pipe[1], &fail, 1);
        (void)wr;
        close(status_pipe[1]);
        _exit(1);
    }
    if (pid2 > 0) {
        /* First child exits — grandchild continues */
        close(status_pipe[1]);
        _exit(0);
    }

    /* Grandchild: the actual daemon */
    if (chdir("/") != 0) { /* best effort */ }
    umask(027);

    /* Redirect stdio to /dev/null */
    int devnull = open("/dev/null", O_RDWR);
    if (devnull >= 0) {
        dup2(devnull, STDIN_FILENO);
        dup2(devnull, STDOUT_FILENO);
        dup2(devnull, STDERR_FILENO);
        if (devnull > 2) close(devnull);
    }

    /* Write PID file */
    if (write_pid_file(pid_file_path) != 0) {
        char fail = 1;
        ssize_t wr = write(status_pipe[1], &fail, 1);
        (void)wr;
        close(status_pipe[1]);
        _exit(1);
    }

    /* Keep pipe write fd open — caller signals success after server starts */
    g_daemon_pipe_fd = status_pipe[1];

    return 0;
}

/** Signal daemon startup result to waiting parent process */
static void daemon_signal_parent(bool success)
{
    if (g_daemon_pipe_fd >= 0) {
        char status = success ? 0 : 1;
        ssize_t wr = write(g_daemon_pipe_fd, &status, 1);
        (void)wr;
        close(g_daemon_pipe_fd);
        g_daemon_pipe_fd = -1;
    }
}

/* ============================================================================
 * Main
 * ========================================================================== */

static void print_usage(const char *prog)
{
    fprintf(stderr, "Usage: %s [-c config_file] [-d] [-v|-vv] [-q|-qq] [-l logfile] [--fresh] [bind_ip] [port]\n", prog);
    fprintf(stderr, "\n");
    fprintf(stderr, "  -c config  - Configuration file (default: doip-server.conf in CWD)\n");
    fprintf(stderr, "  -d         - Run as background daemon\n");
    fprintf(stderr, "  -v         - Verbose (INFO+DEBUG)\n");
    fprintf(stderr, "  -vv        - Extra verbose (all messages)\n");
    fprintf(stderr, "  -q         - Quiet (ERROR+WARN only)\n");
    fprintf(stderr, "  -qq        - Very quiet (ERROR only)\n");
    fprintf(stderr, "  -l path    - Log file path (default: /var/FC-1-DOIP.log)\n");
    fprintf(stderr, "  --fresh    - Delete and regenerate all scripts/configs (clean start)\n");
    fprintf(stderr, "  bind_ip    - IP address to bind to (overrides config)\n");
    fprintf(stderr, "  port       - TCP/UDP port (overrides config)\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "DoIP Blob Server — receives data blobs from Fleet-Connect-1\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "Interactive CLI commands (foreground mode with TTY):\n");
    fprintf(stderr, "  status, config, transfer, generate-scripts, help, quit\n");
}

int main(int argc, char *argv[])
{
    const char *config_file = NULL;
    const char *cli_bind_ip = NULL;
    const char *cli_log_path = NULL;
    uint16_t cli_port = 0;
    bool cli_port_set = false;
    bool cli_daemon = false;
    bool cli_fresh = false;
    int verbosity = DOIP_LOG_INFO;  /* default */

    /* Parse arguments */
    int i = 1;
    while (i < argc) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        }
        if (strcmp(argv[i], "-d") == 0) {
            cli_daemon = true;
            i++;
            continue;
        }
        if (strcmp(argv[i], "-c") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Error: -c requires a config file path\n");
                return 1;
            }
            config_file = argv[++i];
            i++;
            continue;
        }
        if (strcmp(argv[i], "-l") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Error: -l requires a log file path\n");
                return 1;
            }
            cli_log_path = argv[++i];
            i++;
            continue;
        }
        if (strcmp(argv[i], "-vv") == 0) {
            verbosity = DOIP_LOG_DEBUG;
            i++;
            continue;
        }
        if (strcmp(argv[i], "-v") == 0) {
            verbosity = DOIP_LOG_DEBUG;
            i++;
            continue;
        }
        if (strcmp(argv[i], "-qq") == 0) {
            verbosity = DOIP_LOG_ERROR;
            i++;
            continue;
        }
        if (strcmp(argv[i], "-q") == 0) {
            verbosity = DOIP_LOG_WARN;
            i++;
            continue;
        }
        if (strcmp(argv[i], "--fresh") == 0) {
            cli_fresh = true;
            i++;
            continue;
        }
        /* Positional: bind_ip then port */
        if (!cli_bind_ip) {
            cli_bind_ip = argv[i];
        } else if (!cli_port_set) {
            char *endptr;
            unsigned long val = strtoul(argv[i], &endptr, 10);
            if (endptr == argv[i] || *endptr != '\0' || val > 65535) {
                fprintf(stderr, "Error: invalid port '%s'\n", argv[i]);
                return 1;
            }
            cli_port = (uint16_t)val;
            cli_port_set = true;
        }
        i++;
    }

    /* Load configuration BEFORE daemonize (need pid_file path) */
    doip_config_defaults(&g_app_config);

    /*
     * Auto-create doip-server.conf in CWD if missing (before config load).
     * The full ensure_defaults (scripts + phonehome config) runs after
     * logger init so its output is visible in logs.
     */
    script_gen_ensure_defaults_config();

    if (config_file) {
        if (doip_config_load(&g_app_config, config_file) != 0) {
            fprintf(stderr, "Cannot open config file: %s\n", config_file);
            return 1;
        }
    } else {
        /* Try default config in CWD, soft fail */
        doip_config_load(&g_app_config, "doip-server.conf");
    }

    /* CLI -d overrides config */
    if (cli_daemon)
        g_app_config.daemon_mode = true;

    /* CLI overrides for network */
    if (cli_bind_ip) {
        strncpy(g_app_config.bind_address_buf, cli_bind_ip,
                sizeof(g_app_config.bind_address_buf) - 1);
        g_app_config.bind_address_buf[sizeof(g_app_config.bind_address_buf) - 1] = '\0';
        g_app_config.server.bind_address = g_app_config.bind_address_buf;
    }
    if (cli_port_set) {
        g_app_config.server.tcp_port = cli_port;
        g_app_config.server.udp_port = cli_port;
    }

    /*
     * Daemonize BEFORE any threads, mutexes, or file handles.
     * POSIX: fork() in a multi-threaded process only duplicates the calling
     * thread — any mutexes held by other threads are permanently locked.
     */
    if (g_app_config.daemon_mode) {
        if (daemonize(g_app_config.pid_file) != 0) {
            fprintf(stderr, "Failed to daemonize\n");
            return 1;
        }
    }

    /* Initialize logger after daemonize (needs working stderr/file) */
    const char *log_path = cli_log_path ? cli_log_path : "/var/FC-1-DOIP.log";
    if (doip_log_init(log_path, (doip_log_level_t)verbosity) != 0) {
        fprintf(stderr, "Warning: logger init failed, continuing with stderr only\n");
    }

    LOG_INFO("========================================");
    LOG_INFO(" DoIP Blob Server v%s", DOIP_SERVER_VERSION);
#ifdef DOIP_BUILD_DATE
    LOG_INFO(" Built: %s", DOIP_BUILD_DATE);
#endif
    LOG_INFO("========================================");

    if (config_file) {
        LOG_INFO("Loaded config: %s", config_file);
    } else {
        LOG_INFO("Using default/CWD config");
    }

    doip_config_print(&g_app_config);

    /*
     * --fresh: delete generated scripts/configs so they get regenerated
     * from the embedded templates. Ensures on-disk files match the binary.
     */
    if (cli_fresh) {
        LOG_INFO("--fresh: removing generated files for clean regeneration");
        unlink("/usr/sbin/phonehome-connect.sh");
        unlink("/usr/sbin/phonehome-keygen.sh");
        unlink("/usr/sbin/phonehome-register.sh");
        unlink("/etc/phonehome/phonehome.conf");
        LOG_INFO("--fresh: removed scripts and phonehome config");
    }

    /*
     * Kill any lingering phonehome tunnel processes and clean up.
     * No tunnel should be active when the server is (re)starting.
     * Zombies from previous connect script runs are also reaped.
     */
    {
        FILE *lockfp = fopen("/etc/phonehome/phonehome.lock", "r");
        if (lockfp) {
            int old_pid = 0;
            if (fscanf(lockfp, "%d", &old_pid) == 1 && old_pid > 1) {
                if (kill(old_pid, 0) == 0) {
                    LOG_INFO("Killing stale phonehome process (PID %d)", old_pid);
                    kill(old_pid, SIGTERM);
                    usleep(500000);  /* 500ms grace */
                    if (kill(old_pid, 0) == 0) {
                        kill(old_pid, SIGKILL);
                        LOG_INFO("Force-killed phonehome PID %d", old_pid);
                    }
                }
            }
            fclose(lockfp);
        }
        /* Also kill any orphaned phonehome-connect processes by name */
        system("pkill -f phonehome-connect 2>/dev/null");

        if (unlink("/etc/phonehome/phonehome.lock") == 0) {
            LOG_INFO("Removed stale phone-home lock file");
        }
    }

    /*
     * Auto-create missing phone-home scripts and config files.
     * Runs after logger init so output is visible in logs.
     * Only creates files that don't exist — never overwrites.
     */
    script_gen_ensure_defaults();

    /* Initialize phone-home subsystem (optional — failure degrades gracefully) */
    static phonehome_config_t phonehome_cfg;
    if (g_app_config.phonehome_config_path[0]) {
        if (phonehome_config_load(&phonehome_cfg, g_app_config.phonehome_config_path) == 0) {
            if (phonehome_init(&phonehome_cfg) == 0) {
                LOG_INFO("Phone-home capability enabled");
            } else {
                LOG_WARN("Phone-home disabled (init failed)");
            }
        } else {
            LOG_WARN("Phone-home disabled (config load failed)");
        }
    } else {
        LOG_INFO("Phone-home not configured (no phonehome_config in doip-server.conf)");
    }

    /* Ensure SSH service user exists for bastion inbound connections */
    if (phonehome_cfg.ssh_user[0]) {
        phonehome_ensure_ssh_user(&phonehome_cfg);
    }

    /* Initialize CRC-32 lookup table */
    crc32_init_table();

    /* Initialize transfer state */
    memset(&g_transfer, 0, sizeof(g_transfer));

    /* Ensure storage directory exists */
    if (ensure_storage_dir() != 0) {
        LOG_WARN("Storage directory '%s' unavailable",
                 g_app_config.blob_storage_dir);
    }

    /* Initialize server */
    doip_server_t server;
    doip_result_t ret = doip_server_init(&server, &g_app_config.server);
    if (ret != DOIP_OK) {
        LOG_ERROR("Failed to init server: %s", doip_result_str(ret));
        daemon_signal_parent(false);
        if (g_app_config.daemon_mode)
            remove_pid_file(g_app_config.pid_file);
        doip_log_shutdown();
        return 1;
    }

    /* Register this entity as a target */
    ret = doip_server_register_target(&server, g_app_config.server.logical_address);
    if (ret != DOIP_OK) {
        LOG_ERROR("Failed to register target: %s", doip_result_str(ret));
        doip_server_destroy(&server);
        daemon_signal_parent(false);
        if (g_app_config.daemon_mode)
            remove_pid_file(g_app_config.pid_file);
        doip_log_shutdown();
        return 1;
    }

    /* Set callbacks */
    doip_server_set_routing_callback(&server, handle_routing_activation);
    doip_server_set_diagnostic_callback(&server, handle_diagnostic);

    /* Set up signal handlers before starting threads */
    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    /* Start server (binds ports, spawns threads) */
    ret = doip_server_start(&server);
    if (ret != DOIP_OK) {
        LOG_ERROR("Failed to start server: %s", doip_result_str(ret));
        doip_server_destroy(&server);
        daemon_signal_parent(false);
        if (g_app_config.daemon_mode)
            remove_pid_file(g_app_config.pid_file);
        doip_log_shutdown();
        return 1;
    }

    /* Server is now running and ports are bound — signal daemon parent */
    daemon_signal_parent(true);

    time_t start_time = time(NULL);
    g_server_start_time = start_time;

    LOG_INFO("Max blob size: %u bytes (%u MB)",
             g_app_config.blob_max_size, g_app_config.blob_max_size / (1024 * 1024));
    LOG_INFO("Transfer timeout: %u seconds", g_app_config.transfer_timeout_sec);
    LOG_INFO("Storage: %s/", g_app_config.blob_storage_dir);
    LOG_INFO("Server running.%s",
             g_app_config.daemon_mode ? " (daemon mode)" : " Press Ctrl+C to stop.");

    /* Send initial announcement */
    if (doip_server_send_announcement(&server) != DOIP_OK)
        LOG_WARN("Initial announcement failed");

    /* Determine if interactive CLI should be active.
     * CLI is only enabled when:
     *   1. Not in daemon mode
     *   2. stdin is a TTY (prevents SIGTTIN when backgrounded by test harness)
     */
    bool interactive = !g_app_config.daemon_mode && isatty(STDIN_FILENO);

    /* Set up CLI context */
    cli_context_t cli_ctx;
    memset(&cli_ctx, 0, sizeof(cli_ctx));
    cli_ctx.config = &g_app_config;
    cli_ctx.server = &server;
    cli_ctx.running = &g_running;
    cli_ctx.server_start_time = start_time;

    if (interactive) {
        cli_print_prompt();
    }

    /* Main loop */
    while (g_running) {
        /* Check for transfer timeout */
        pthread_mutex_lock(&g_transfer_mutex);
        if (g_transfer.active) {
            time_t now = time(NULL);
            if (difftime(now, g_transfer.last_activity) > (double)g_app_config.transfer_timeout_sec) {
                LOG_WARN("Transfer timed out, aborting");
                transfer_cleanup_locked();
            }
        }
        /* Snapshot transfer state for CLI (while lock held) */
        cli_ctx.transfer_active = g_transfer.active;
        cli_ctx.transfer_bytes_received = g_transfer.bytes_received;
        cli_ctx.transfer_memory_size = g_transfer.memory_size;
        cli_ctx.transfer_block_sequence = g_transfer.block_sequence;
        cli_ctx.transfer_last_activity = g_transfer.last_activity;
        pthread_mutex_unlock(&g_transfer_mutex);

        /* Snapshot client count (under clients_mutex) */
        pthread_mutex_lock(&server.clients_mutex);
        cli_ctx.client_count = server.num_clients;
        pthread_mutex_unlock(&server.clients_mutex);

        if (interactive) {
            /* select() on stdin with 1s timeout */
            fd_set fds;
            FD_ZERO(&fds);
            FD_SET(STDIN_FILENO, &fds);
            struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
            int sel = select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv);
            if (sel > 0) {
                if (cli_process_input(&cli_ctx) != 0)
                    break;  /* quit requested */
                cli_print_prompt();
            }
        } else {
            sleep(1);  /* daemon/non-TTY mode */
        }
    }

    LOG_INFO("Shutting down...");

    /* Clean up any in-progress transfer */
    pthread_mutex_lock(&g_transfer_mutex);
    transfer_cleanup_locked();
    pthread_mutex_unlock(&g_transfer_mutex);

    doip_server_destroy(&server);
    phonehome_shutdown();

    if (g_app_config.daemon_mode)
        remove_pid_file(g_app_config.pid_file);

    LOG_INFO("Server stopped.");
    doip_log_shutdown();

    return 0;
}
