/**
 * @file phonehome_handler.c
 * @brief DCU Phone-Home RoutineControl handler implementation
 *
 * Handles UDS RoutineControl (SID 0x31, subFunc 0x01, routineId 0xF0A0)
 * to trigger reverse SSH tunnels. See DCU_PhoneHome_Specification.md.
 *
 * Security features:
 *   - HMAC-SHA256 authentication (standalone, no OpenSSL)
 *   - Constant-time HMAC comparison (prevents timing side-channel)
 *   - Nonce replay cache (64 entries, 300s TTL)
 *   - bastion_host DNS-character validation before execl()
 *   - HMAC secret file opened with O_NOFOLLOW (prevents symlink attacks)
 *   - HMAC secret cleared from memory on shutdown via explicit_bzero()
 *   - Atomic lock file creation (O_CREAT|O_EXCL) under mutex
 */

#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE  /* for explicit_bzero */

#include "phonehome_handler.h"
#include "hmac_sha256.h"
#include "config_parse.h"
#include "doip_log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/stat.h>

/* Torizon/musl may lack explicit_bzero — provide a safe fallback */
#ifdef PLATFORM_TORIZON
static void secure_zero(void *buf, size_t len)
{
    volatile unsigned char *p = (volatile unsigned char *)buf;
    while (len--) *p++ = 0;
}
#define explicit_bzero(buf, len) secure_zero(buf, len)
#endif

/* ============================================================================
 * Constants
 * ========================================================================== */

#define NONCE_LEN           8
#define HMAC_LEN            SHA256_DIGEST_SIZE  /* 32 */
#define MIN_PDU_LEN         (4 + NONCE_LEN + HMAC_LEN)  /* 44 bytes */
#define NONCE_OFFSET        4       /* After SID + subFunc + routineId(2) */
#define HMAC_OFFSET         (NONCE_OFFSET + NONCE_LEN)   /* 12 */
#define ARGS_OFFSET         (HMAC_OFFSET + HMAC_LEN)     /* 44 */
#define REPLAY_CACHE_SIZE   64
#define NONCE_TTL_SECS      300

/* NRC codes (ISO 14229) */
#define NRC_INCORRECT_MSG_LEN       0x13
#define NRC_BUSY_REPEAT_REQUEST     0x21
#define NRC_CONDITIONS_NOT_CORRECT  0x22
#define NRC_REQUEST_SEQUENCE_ERROR  0x24
#define NRC_REQUEST_OUT_OF_RANGE    0x31
#define NRC_INVALID_KEY             0x35

/* ============================================================================
 * State
 * ========================================================================== */

static uint8_t hmac_secret[SHA256_DIGEST_SIZE];
static int     hmac_loaded = 0;

/* Config pointers (set by phonehome_init, read-only after) */
static const phonehome_config_t *g_cfg = NULL;

/* Replay cache */
typedef struct {
    uint8_t nonce[NONCE_LEN];
    time_t  expiry;
} replay_entry_t;

static replay_entry_t replay_cache[REPLAY_CACHE_SIZE];
static unsigned int   replay_index = 0;
static pthread_mutex_t replay_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Fork serialization (protects lock-file-check + fork sequence) */
static pthread_mutex_t phonehome_fork_mutex = PTHREAD_MUTEX_INITIALIZER;

/* ============================================================================
 * NRC Helper
 * ========================================================================== */

static int build_nrc(uint8_t nrc, uint8_t *resp, uint32_t resp_size)
{
    if (resp_size < 3) return -1;
    resp[0] = 0x7F;
    resp[1] = 0x31;  /* SID for RoutineControl */
    resp[2] = nrc;
    return 3;
}

/* ============================================================================
 * Config Parser (uses shared cfg_trim_leading/cfg_trim_trailing from config_parse.h)
 * ========================================================================== */

int phonehome_config_load(phonehome_config_t *cfg, const char *path)
{
    memset(cfg, 0, sizeof(*cfg));

    /* Defaults */
    strncpy(cfg->bastion_host, "bastion.example.com", sizeof(cfg->bastion_host) - 1);
    strncpy(cfg->lock_file, "/var/run/phonehome.lock", sizeof(cfg->lock_file) - 1);

    FILE *fp = fopen(path, "r");
    if (!fp) {
        LOG_ERROR("phonehome: cannot open config '%s': %s", path, strerror(errno));
        return -1;
    }

    char line[512];
    int line_num = 0;

    while (fgets(line, sizeof(line), fp)) {
        line_num++;

        size_t len = strlen(line);
        if (len > 0 && line[len - 1] == '\n')
            line[len - 1] = '\0';

        char *p = cfg_trim_leading(line);
        if (*p == '\0' || *p == '#')
            continue;

        char *eq = strchr(p, '=');
        if (!eq) continue;

        *eq = '\0';
        char *key = p;
        char *value = eq + 1;
        cfg_trim_trailing(key);
        value = cfg_trim_leading(value);
        cfg_trim_trailing(value);

        if (strcmp(key, "BASTION_HOST") == 0) {
            strncpy(cfg->bastion_host, value, sizeof(cfg->bastion_host) - 1);
            cfg->bastion_host[sizeof(cfg->bastion_host) - 1] = '\0';
        }
        else if (strcmp(key, "HMAC_SECRET_FILE") == 0) {
            strncpy(cfg->hmac_secret_path, value, sizeof(cfg->hmac_secret_path) - 1);
            cfg->hmac_secret_path[sizeof(cfg->hmac_secret_path) - 1] = '\0';
        }
        else if (strcmp(key, "CONNECT_SCRIPT") == 0) {
            strncpy(cfg->connect_script, value, sizeof(cfg->connect_script) - 1);
            cfg->connect_script[sizeof(cfg->connect_script) - 1] = '\0';
        }
        else if (strcmp(key, "LOCK_FILE") == 0) {
            strncpy(cfg->lock_file, value, sizeof(cfg->lock_file) - 1);
            cfg->lock_file[sizeof(cfg->lock_file) - 1] = '\0';
        }
        /* Ignore other keys (BASTION_PORT, TUNNEL_TIMEOUT, etc. used by shell scripts) */
    }

    fclose(fp);

    /* Validate required keys */
    if (cfg->hmac_secret_path[0] == '\0') {
        LOG_ERROR("phonehome: config missing required key HMAC_SECRET_FILE");
        return -1;
    }
    if (cfg->connect_script[0] == '\0') {
        LOG_ERROR("phonehome: config missing required key CONNECT_SCRIPT");
        return -1;
    }

    return 0;
}

/* ============================================================================
 * Initialization
 * ========================================================================== */

int phonehome_init(const phonehome_config_t *cfg)
{
    if (!cfg) return -1;

    /* Open HMAC secret with O_NOFOLLOW to prevent symlink attacks */
    int fd = open(cfg->hmac_secret_path, O_RDONLY | O_NOFOLLOW);
    if (fd < 0) {
        LOG_ERROR("phonehome: cannot open HMAC secret '%s': %s",
                  cfg->hmac_secret_path, strerror(errno));
        return -1;
    }

    /* Reject world-readable secret files */
    struct stat st;
    if (fstat(fd, &st) != 0) {
        LOG_ERROR("phonehome: fstat HMAC secret failed: %s", strerror(errno));
        close(fd);
        return -1;
    }
    if (st.st_mode & (S_IROTH | S_IWOTH)) {
        LOG_ERROR("phonehome: HMAC secret '%s' is world-accessible (mode %04o) — refusing to load",
                  cfg->hmac_secret_path, (unsigned)(st.st_mode & 0777));
        close(fd);
        return -1;
    }

    FILE *fp = fdopen(fd, "rb");
    if (!fp) {
        LOG_ERROR("phonehome: fdopen failed: %s", strerror(errno));
        close(fd);
        return -1;
    }

    size_t n = fread(hmac_secret, 1, sizeof(hmac_secret), fp);
    fclose(fp);  /* closes fd too */

    if (n != sizeof(hmac_secret)) {
        LOG_ERROR("phonehome: HMAC secret must be exactly %zu bytes (got %zu)",
                  sizeof(hmac_secret), n);
        explicit_bzero(hmac_secret, sizeof(hmac_secret));
        return -1;
    }

    g_cfg = cfg;
    hmac_loaded = 1;

    /* Clear replay cache */
    memset(replay_cache, 0, sizeof(replay_cache));
    replay_index = 0;

    LOG_INFO("phonehome: initialized (HMAC secret loaded, script=%s)",
             cfg->connect_script);
    return 0;
}

/* ============================================================================
 * Replay Cache
 * ========================================================================== */

/** Check nonce against cache. Returns 0 if new, -1 if replay. Records nonce. */
static int check_and_record_nonce(const uint8_t *nonce)
{
    time_t now = time(NULL);

    pthread_mutex_lock(&replay_mutex);

    /* Search for matching unexpired nonce */
    for (int i = 0; i < REPLAY_CACHE_SIZE; i++) {
        if (replay_cache[i].expiry > now &&
            memcmp(replay_cache[i].nonce, nonce, NONCE_LEN) == 0) {
            pthread_mutex_unlock(&replay_mutex);
            return -1;  /* Replay detected */
        }
    }

    /* Record new nonce in circular buffer */
    int idx = replay_index % REPLAY_CACHE_SIZE;
    memcpy(replay_cache[idx].nonce, nonce, NONCE_LEN);
    replay_cache[idx].expiry = now + NONCE_TTL_SECS;
    replay_index++;

    pthread_mutex_unlock(&replay_mutex);
    return 0;
}

/* ============================================================================
 * bastion_host Validation
 * ========================================================================== */

/** Validate hostname against DNS-legal characters [a-zA-Z0-9._-] */
static int validate_hostname(const char *host, size_t len)
{
    if (len == 0 || len > 253) return -1;
    for (size_t i = 0; i < len; i++) {
        char c = host[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_'))
            return -1;
    }
    return 0;
}

/* ============================================================================
 * Lock File Management
 * ========================================================================== */

/**
 * Check if tunnel is already active via lock file.
 * Returns: 0 = no active tunnel, -1 = active tunnel, -2 = error
 */
static int check_lock_file(const char *path)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        if (errno == ENOENT) return 0;  /* No lock file = no tunnel */
        return -2;  /* Permission error or other */
    }

    char pid_buf[32] = {0};
    ssize_t n = read(fd, pid_buf, sizeof(pid_buf) - 1);
    close(fd);

    if (n <= 0) {
        /* Empty or unreadable lock file — treat as stale */
        unlink(path);
        return 0;
    }

    long pid = strtol(pid_buf, NULL, 10);
    if (pid <= 0) {
        unlink(path);
        return 0;
    }

    /* Check if process is alive */
    if (kill((pid_t)pid, 0) == 0) {
        return -1;  /* Process alive = tunnel active */
    }

    /* Process dead — remove stale lock */
    unlink(path);
    return 0;
}

/* ============================================================================
 * Request Handler
 * ========================================================================== */

int phonehome_handle_routine(const uint8_t *uds_data, uint32_t uds_len,
                             uint8_t *response, uint32_t resp_size)
{
    /* Precondition: caller verified SID=0x31, subFunc=0x01, routineId=0xF0A0 */

    if (!hmac_loaded) {
        LOG_WARN("phonehome: trigger rejected — HMAC secret not loaded");
        return build_nrc(NRC_CONDITIONS_NOT_CORRECT, response, resp_size);
    }

    if (uds_len < MIN_PDU_LEN) {
        LOG_WARN("phonehome: trigger rejected — PDU too short (%u < %d)",
                 uds_len, MIN_PDU_LEN);
        return build_nrc(NRC_INCORRECT_MSG_LEN, response, resp_size);
    }

    const uint8_t *nonce = uds_data + NONCE_OFFSET;
    const uint8_t *rx_hmac = uds_data + HMAC_OFFSET;

    /* Compute expected HMAC */
    uint8_t exp_hmac[HMAC_LEN];
    hmac_sha256(hmac_secret, sizeof(hmac_secret), nonce, NONCE_LEN, exp_hmac);

    /* Constant-time comparison */
    if (hmac_sha256_compare(rx_hmac, exp_hmac, HMAC_LEN) != 0) {
        LOG_WARN("phonehome: HMAC verification failed — possible spoofing attempt");
        return build_nrc(NRC_INVALID_KEY, response, resp_size);
    }

    /* Replay protection */
    if (check_and_record_nonce(nonce) != 0) {
        LOG_WARN("phonehome: replayed nonce detected");
        return build_nrc(NRC_REQUEST_SEQUENCE_ERROR, response, resp_size);
    }

    /* Parse optional args: bastion_host (null-terminated) + port (uint16 BE) */
    char bastion_host[254];
    strncpy(bastion_host, g_cfg->bastion_host, sizeof(bastion_host) - 1);
    bastion_host[sizeof(bastion_host) - 1] = '\0';
    uint16_t remote_port = 0;

    if (uds_len > ARGS_OFFSET) {
        size_t remaining = uds_len - ARGS_OFFSET;
        const uint8_t *args = uds_data + ARGS_OFFSET;
        size_t host_len = strnlen((const char *)args, remaining < 253 ? remaining : 253);
        if (host_len > 0 && host_len < 254) {
            /* Validate hostname characters before using */
            if (validate_hostname((const char *)args, host_len) != 0) {
                LOG_WARN("phonehome: invalid bastion_host characters in request");
                return build_nrc(NRC_REQUEST_OUT_OF_RANGE, response, resp_size);
            }
            memcpy(bastion_host, args, host_len);
            bastion_host[host_len] = '\0';
            size_t port_offset = host_len + 1;
            if (remaining >= port_offset + 2) {
                remote_port = (uint16_t)((args[port_offset] << 8) |
                                          args[port_offset + 1]);
            }
        }
    }

    /* Format nonce as hex string for script argument */
    char nonce_hex[NONCE_LEN * 2 + 1];
    for (int i = 0; i < NONCE_LEN; i++)
        snprintf(nonce_hex + i * 2, 3, "%02x", nonce[i]);

    char port_str[6];
    snprintf(port_str, sizeof(port_str), "%u", remote_port);

    /*
     * Serialize lock-file-check + fork under phonehome_fork_mutex.
     * This prevents TOCTOU races when multiple clients send triggers
     * concurrently.
     */
    pthread_mutex_lock(&phonehome_fork_mutex);

    /* Check if tunnel is already active */
    int lock_status = check_lock_file(g_cfg->lock_file);
    if (lock_status == -1) {
        pthread_mutex_unlock(&phonehome_fork_mutex);
        LOG_INFO("phonehome: trigger rejected — tunnel already active");
        return build_nrc(NRC_BUSY_REPEAT_REQUEST, response, resp_size);
    }
    if (lock_status == -2) {
        pthread_mutex_unlock(&phonehome_fork_mutex);
        LOG_WARN("phonehome: cannot check lock file: %s", strerror(errno));
        return build_nrc(NRC_CONDITIONS_NOT_CORRECT, response, resp_size);
    }

    /* Create lock file atomically */
    int lock_fd = open(g_cfg->lock_file, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
    if (lock_fd < 0) {
        pthread_mutex_unlock(&phonehome_fork_mutex);
        if (errno == EEXIST) {
            LOG_INFO("phonehome: trigger rejected — tunnel already active (race)");
            return build_nrc(NRC_BUSY_REPEAT_REQUEST, response, resp_size);
        }
        LOG_WARN("phonehome: cannot create lock file: %s", strerror(errno));
        return build_nrc(NRC_CONDITIONS_NOT_CORRECT, response, resp_size);
    }

    LOG_INFO("phonehome: trigger accepted. Bastion=%s Port=%s Nonce=%s",
             bastion_host, port_str, nonce_hex);

    /*
     * Spawn phone-home script as detached process.
     *
     * Safety: fork() from a multi-threaded process is POSIX-defined safe
     * when the child immediately calls execl() (async-signal-safe).
     * The child replaces its entire process image, so no mutex state
     * from other threads is accessed.
     */
    pid_t pid = fork();
    if (pid == 0) {
        /* Child: detach and exec */
        setsid();
        /* The connect script will overwrite the lock file with its own PID */
        execl(g_cfg->connect_script, "phonehome-connect.sh",
              bastion_host, port_str, nonce_hex, (char *)NULL);
        /* exec failed — remove lock file and exit */
        unlink(g_cfg->lock_file);
        _exit(1);
    }

    if (pid < 0) {
        /* Fork failed — clean up lock file */
        close(lock_fd);
        unlink(g_cfg->lock_file);
        pthread_mutex_unlock(&phonehome_fork_mutex);
        LOG_ERROR("phonehome: fork() failed: %s", strerror(errno));
        return build_nrc(NRC_CONDITIONS_NOT_CORRECT, response, resp_size);
    }

    /* Parent: write child PID to lock file, then close */
    char pid_buf[32];
    int pid_len = snprintf(pid_buf, sizeof(pid_buf), "%d\n", (int)pid);
    if (pid_len > 0) {
        /* Best-effort write; lock file exists regardless */
        ssize_t wr = write(lock_fd, pid_buf, (size_t)pid_len);
        (void)wr;
    }
    close(lock_fd);

    pthread_mutex_unlock(&phonehome_fork_mutex);

    /* Positive response: routineRunning (tunnel initiating asynchronously) */
    if (resp_size < 5) return -1;
    response[0] = 0x71;    /* RoutineControl positive response */
    response[1] = 0x01;    /* subFunction: startRoutine */
    response[2] = 0xF0;    /* routineIdentifier high byte */
    response[3] = 0xA0;    /* routineIdentifier low byte */
    response[4] = 0x02;    /* routineStatus: routineRunning */
    return 5;
}

/* ============================================================================
 * Shutdown
 * ========================================================================== */

void phonehome_shutdown(void)
{
    if (hmac_loaded) {
        explicit_bzero(hmac_secret, sizeof(hmac_secret));
        hmac_loaded = 0;
    }
    g_cfg = NULL;
    LOG_INFO("phonehome: shutdown (HMAC secret cleared)");
}
