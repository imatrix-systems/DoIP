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
#include <stdbool.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/wait.h>

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
#define NRC_GENERAL_PROGRAMMING_FAILURE 0x72

/* Provisioning constants */
#define PROVISION_PDU_MIN           40  /* 4 header + 4 SN + 32 HMAC (minimum) */
#define PROVISION_PDU_BASTION_OFF   42  /* bastion hostname starts at byte 42 */
#define HMAC_SECRET_PATH            "/etc/phonehome/hmac_secret"
#define HMAC_SECRET_DIR             "/etc/phonehome"
#define HMAC_SECRET_TMP             HMAC_SECRET_PATH ".tmp"

/* ============================================================================
 * State
 * ========================================================================== */

static uint8_t hmac_secret[SHA256_DIGEST_SIZE];
static int     hmac_loaded = 0;

/* Config pointers (set by phonehome_init or phonehome_handle_provision) */
static const phonehome_config_t *g_cfg = NULL;

/**
 * Default config for fresh DCU where phonehome_init() was never called.
 * Populated by phonehome_handle_provision() so subsequent trigger
 * calls have valid g_cfg->bastion_host, lock_file, connect_script.
 */
static phonehome_config_t g_provision_cfg;

/** Mutex protecting hmac_secret[] and hmac_loaded reads/writes.
 *  Provision handler takes write access, trigger handler takes read access.
 *  Also protects g_cfg assignment in provision handler. */
static pthread_mutex_t hmac_mutex = PTHREAD_MUTEX_INITIALIZER;

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
    strncpy(cfg->bastion_host, "bastion-dev.imatrixsys.com", sizeof(cfg->bastion_host) - 1);
    strncpy(cfg->lock_file, "/etc/phonehome/phonehome.lock", sizeof(cfg->lock_file) - 1);
    strncpy(cfg->ssh_user, "imatrix", sizeof(cfg->ssh_user) - 1);

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
        else if (strcmp(key, "BASTION_CLIENT_KEY") == 0) {
            strncpy(cfg->bastion_client_key, value, sizeof(cfg->bastion_client_key) - 1);
            cfg->bastion_client_key[sizeof(cfg->bastion_client_key) - 1] = '\0';
        }
        else if (strcmp(key, "SSH_USER") == 0) {
            strncpy(cfg->ssh_user, value, sizeof(cfg->ssh_user) - 1);
            cfg->ssh_user[sizeof(cfg->ssh_user) - 1] = '\0';
        }
        else if (strcmp(key, "BASTION_PORT") == 0) {
            int port = atoi(value);
            if (port > 0 && port <= 65535)
                cfg->bastion_port = (uint16_t)port;
        }
        /* Ignore other keys (TUNNEL_TIMEOUT, etc. used by shell scripts) */
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

    /* Require bastion client key — without it the bastion web-ssh app
     * cannot authenticate through the reverse tunnel to the DCU.
     * The key must be set in phonehome.conf BASTION_CLIENT_KEY or
     * delivered by the FC-1 provisioning PDU. */
    if (cfg->bastion_client_key[0] == '\0') {
        LOG_ERROR("phonehome: BASTION_CLIENT_KEY not configured in %s",
                  "phonehome.conf");
        LOG_ERROR("phonehome: Get the key from the bastion server: "
                  "cat /opt/web-ssh-bastion/bastion_key.pub");
        LOG_ERROR("phonehome: Phone-home DISABLED — bastion cannot authenticate to DCU without this key");
        explicit_bzero(hmac_secret, sizeof(hmac_secret));
        return -1;
    }
    if (strncmp(cfg->bastion_client_key, "ssh-", 4) != 0) {
        LOG_ERROR("phonehome: BASTION_CLIENT_KEY invalid format (must start with 'ssh-')");
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

    /*
     * Copy HMAC secret under lock to avoid data race with
     * phonehome_handle_provision() which may update the secret
     * from another client thread.
     */
    uint8_t local_secret[SHA256_DIGEST_SIZE];
    const phonehome_config_t *cfg;
    pthread_mutex_lock(&hmac_mutex);
    if (!hmac_loaded) {
        pthread_mutex_unlock(&hmac_mutex);
        LOG_WARN("phonehome: trigger rejected — HMAC secret not loaded");
        return build_nrc(NRC_CONDITIONS_NOT_CORRECT, response, resp_size);
    }
    memcpy(local_secret, hmac_secret, sizeof(local_secret));
    cfg = g_cfg;
    pthread_mutex_unlock(&hmac_mutex);

    if (uds_len < MIN_PDU_LEN) {
        LOG_WARN("phonehome: trigger rejected — PDU too short (%u < %d)",
                 uds_len, MIN_PDU_LEN);
        explicit_bzero(local_secret, sizeof(local_secret));
        return build_nrc(NRC_INCORRECT_MSG_LEN, response, resp_size);
    }

    const uint8_t *nonce = uds_data + NONCE_OFFSET;
    const uint8_t *rx_hmac = uds_data + HMAC_OFFSET;

    /*
     * Compute expected HMAC using local copy of secret.
     * Legacy PDU (44 bytes): HMAC(secret, nonce)
     * Extended PDU (>44 bytes): HMAC(secret, nonce || args_bytes)
     *
     * This ensures the bastion hostname (if present) is authenticated,
     * preventing a LAN attacker from substituting the tunnel destination.
     */
    uint8_t exp_hmac[HMAC_LEN];
    if (uds_len > ARGS_OFFSET) {
        /* Extended PDU: HMAC covers nonce + bastion args */
        uint32_t args_len = uds_len - ARGS_OFFSET;
        uint8_t hmac_data[NONCE_LEN + 256];
        uint32_t hmac_data_len = NONCE_LEN + args_len;
        if (hmac_data_len > sizeof(hmac_data)) {
            LOG_WARN("phonehome: PDU args too large (%u bytes)", args_len);
            explicit_bzero(local_secret, sizeof(local_secret));
            return build_nrc(NRC_INCORRECT_MSG_LEN, response, resp_size);
        }
        memcpy(hmac_data, nonce, NONCE_LEN);
        memcpy(hmac_data + NONCE_LEN, uds_data + ARGS_OFFSET, args_len);
        hmac_sha256(local_secret, sizeof(local_secret),
                    hmac_data, hmac_data_len, exp_hmac);
    } else {
        /* Legacy PDU: HMAC covers nonce only (backward compatible) */
        hmac_sha256(local_secret, sizeof(local_secret),
                    nonce, NONCE_LEN, exp_hmac);
    }

    /* Clear secret copy — no longer needed after HMAC computation */
    explicit_bzero(local_secret, sizeof(local_secret));

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
    strncpy(bastion_host, cfg->bastion_host, sizeof(bastion_host) - 1);
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
    int lock_status = check_lock_file(cfg->lock_file);
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
    int lock_fd = open(cfg->lock_file, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
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
        execl(cfg->connect_script, "phonehome-connect.sh",
              bastion_host, port_str, nonce_hex, (char *)NULL);
        /* exec failed — remove lock file and exit */
        unlink(cfg->lock_file);
        _exit(1);
    }

    if (pid < 0) {
        /* Fork failed — clean up lock file */
        close(lock_fd);
        unlink(cfg->lock_file);
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
 * Provisioning Handler
 * ========================================================================== */

/**
 * @brief Handle phone-home provisioning (routineId 0xF0A1)
 *
 * Receives HMAC secret from FC-1 via DoIP and:
 *   1. Writes it to /etc/phonehome/hmac_secret (atomic temp+rename)
 *   2. Loads it into memory under hmac_mutex
 *   3. Initializes g_cfg with defaults if this is a fresh DCU
 *
 * PDU layout (40 bytes fixed):
 *   [0]     SID 0x31
 *   [1]     subFunc 0x01
 *   [2-3]   routineId 0xF0A1
 *   [4-7]   CAN controller SN (big-endian uint32)
 *   [8-39]  HMAC secret (32 bytes)
 *
 * Thread-safe via hmac_mutex.
 *
 * @param uds_data   Full UDS request
 * @param uds_len    Length of UDS request
 * @param response   Buffer for UDS response
 * @param resp_size  Size of response buffer
 * @return UDS response length, or negative on internal error
 */
int phonehome_handle_provision(const uint8_t *uds_data, uint32_t uds_len,
                                uint8_t *response, uint32_t resp_size)
{
    /* Validate minimum PDU length */
    if (uds_len < PROVISION_PDU_MIN) {
        LOG_WARN("phonehome: provision rejected — bad PDU length (%u, need >= %d)",
                 uds_len, PROVISION_PDU_MIN);
        return build_nrc(NRC_INCORRECT_MSG_LEN, response, resp_size);
    }

    /* Extract CAN controller serial number (big-endian uint32) */
    uint32_t can_sn = ((uint32_t)uds_data[4] << 24) |
                      ((uint32_t)uds_data[5] << 16) |
                      ((uint32_t)uds_data[6] <<  8) |
                      ((uint32_t)uds_data[7]);
    if (can_sn == 0) {
        LOG_WARN("phonehome: provision rejected — CAN SN is zero");
        return build_nrc(NRC_REQUEST_OUT_OF_RANGE, response, resp_size);
    }

    /* Create /etc/phonehome/ directory if it doesn't exist */
    if (mkdir(HMAC_SECRET_DIR, 0700) != 0 && errno != EEXIST) {
        LOG_ERROR("phonehome: cannot create %s: %s", HMAC_SECRET_DIR, strerror(errno));
        return build_nrc(NRC_GENERAL_PROGRAMMING_FAILURE, response, resp_size);
    }

    /*
     * Write HMAC secret to file atomically:
     *   1. Remove any stale temp file (prevents symlink attacks)
     *   2. Create new temp file with O_EXCL|O_NOFOLLOW (mode 0600)
     *   3. Write 32 bytes + fsync
     *   4. Rename to final path (atomic on POSIX)
     */
    unlink(HMAC_SECRET_TMP);

    int fd = open(HMAC_SECRET_TMP, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
    if (fd < 0) {
        LOG_ERROR("phonehome: cannot create temp file %s: %s",
                  HMAC_SECRET_TMP, strerror(errno));
        return build_nrc(NRC_GENERAL_PROGRAMMING_FAILURE, response, resp_size);
    }

    const uint8_t *secret_data = uds_data + 8;
    ssize_t written = write(fd, secret_data, SHA256_DIGEST_SIZE);
    if (written != SHA256_DIGEST_SIZE) {
        LOG_ERROR("phonehome: write failed (%zd/%d): %s",
                  written, SHA256_DIGEST_SIZE, strerror(errno));
        close(fd);
        unlink(HMAC_SECRET_TMP);
        return build_nrc(NRC_GENERAL_PROGRAMMING_FAILURE, response, resp_size);
    }

    if (fsync(fd) != 0) {
        LOG_ERROR("phonehome: fsync failed: %s", strerror(errno));
        close(fd);
        unlink(HMAC_SECRET_TMP);
        return build_nrc(NRC_GENERAL_PROGRAMMING_FAILURE, response, resp_size);
    }
    close(fd);

    if (rename(HMAC_SECRET_TMP, HMAC_SECRET_PATH) != 0) {
        LOG_ERROR("phonehome: rename failed: %s", strerror(errno));
        unlink(HMAC_SECRET_TMP);
        return build_nrc(NRC_GENERAL_PROGRAMMING_FAILURE, response, resp_size);
    }

    /*
     * Load HMAC secret into memory under lock.
     * Set g_cfg BEFORE hmac_loaded to avoid a race window where
     * a trigger thread sees hmac_loaded=1 but g_cfg is still NULL.
     */
    pthread_mutex_lock(&hmac_mutex);

    /* Initialize g_cfg — extract bastion hostname from PDU if present */
    memset(&g_provision_cfg, 0, sizeof(g_provision_cfg));
    strncpy(g_provision_cfg.hmac_secret_path, HMAC_SECRET_PATH,
            sizeof(g_provision_cfg.hmac_secret_path) - 1);
    strncpy(g_provision_cfg.connect_script, "/usr/sbin/phonehome-connect.sh",
            sizeof(g_provision_cfg.connect_script) - 1);
    strncpy(g_provision_cfg.lock_file, "/etc/phonehome/phonehome.lock",
            sizeof(g_provision_cfg.lock_file) - 1);

    /* Extract bastion hostname + optional client key from extended PDU.
     *
     * Layout after offset 40:
     *   [40-41]  Bastion port (big-endian)
     *   [42+]    Bastion hostname (null-terminated)
     *   [42+N+1] Bastion client pubkey (null-terminated, optional)
     */
    strncpy(g_provision_cfg.ssh_user, "imatrix", sizeof(g_provision_cfg.ssh_user) - 1);

    if (uds_len > PROVISION_PDU_BASTION_OFF) {
        uint16_t bastion_port = ((uint16_t)uds_data[40] << 8) | uds_data[41];
        const char *bastion = (const char *)&uds_data[PROVISION_PDU_BASTION_OFF];
        size_t bastion_len = strnlen(bastion, uds_len - PROVISION_PDU_BASTION_OFF);

        if (bastion_len > 0 && bastion_len < sizeof(g_provision_cfg.bastion_host)) {
            strncpy(g_provision_cfg.bastion_host, bastion,
                    sizeof(g_provision_cfg.bastion_host) - 1);
            g_provision_cfg.bastion_port = bastion_port;
            LOG_INFO("phonehome: bastion=%s:%u (from FC-1 provision)",
                     g_provision_cfg.bastion_host, bastion_port);
        }

        /* Check for bastion client pubkey after the hostname null terminator */
        size_t client_key_off = PROVISION_PDU_BASTION_OFF + bastion_len + 1;
        if (client_key_off < uds_len) {
            const char *client_key = (const char *)&uds_data[client_key_off];
            size_t key_len = strnlen(client_key, uds_len - client_key_off);
            if (key_len > 10 && key_len < sizeof(g_provision_cfg.bastion_client_key) &&
                strncmp(client_key, "ssh-", 4) == 0) {
                strncpy(g_provision_cfg.bastion_client_key, client_key,
                        sizeof(g_provision_cfg.bastion_client_key) - 1);
                LOG_INFO("phonehome: bastion client key received (%zu bytes)", key_len);
            }
        }
    } else {
        strncpy(g_provision_cfg.bastion_host, "bastion-dev.imatrixsys.com",
                sizeof(g_provision_cfg.bastion_host) - 1);
        LOG_WARN("phonehome: no bastion in PDU, using default");
    }
    g_cfg = &g_provision_cfg;

    memcpy(hmac_secret, secret_data, SHA256_DIGEST_SIZE);
    hmac_loaded = 1;

    pthread_mutex_unlock(&hmac_mutex);

    LOG_INFO("phonehome: provisioned (CAN SN %u, bastion %s)",
             can_sn, g_provision_cfg.bastion_host);

    /* Ensure SSH service user exists and bastion client key is installed.
     * This creates the 'imatrix' user and writes the bastion's client pubkey
     * to authorized_keys so the bastion web-ssh app can authenticate. */
    if (g_provision_cfg.bastion_client_key[0] != '\0') {
        phonehome_ensure_ssh_user(&g_provision_cfg);
    }

    /*
     * Generate SSH keys if not present, then include public key in response.
     * The FC-1 will register this key with the bastion on our behalf
     * (the DCU cannot authenticate directly with the bastion API).
     */
    {
        const char *key_dir  = "/etc/phonehome";
        const char *key_path = "/etc/phonehome/id_ed25519";
        const char *pub_path = "/etc/phonehome/id_ed25519.pub";
        char sn_comment[32];
        snprintf(sn_comment, sizeof(sn_comment), "dcu-%u", can_sn);

        /* Generate key pair if not already present */
        struct stat kst;
        if (stat(key_path, &kst) != 0) {
            /* Ensure directory exists */
            if (mkdir(key_dir, 0700) != 0 && errno != EEXIST) {
                LOG_WARN("phonehome: cannot create %s: %s", key_dir, strerror(errno));
            }

            pid_t kpid = fork();
            if (kpid == 0) {
                execl("/usr/bin/ssh-keygen", "ssh-keygen",
                      "-t", "ed25519", "-f", key_path,
                      "-N", "", "-C", sn_comment, (char *)NULL);
                _exit(1);
            }
            if (kpid > 0) {
                int wstatus;
                waitpid(kpid, &wstatus, 0);
                if (WIFEXITED(wstatus) && WEXITSTATUS(wstatus) == 0) {
                    chmod(key_path, 0600);
                    LOG_INFO("phonehome: SSH key pair generated for %s", sn_comment);
                } else {
                    LOG_WARN("phonehome: ssh-keygen failed (status=%d)", wstatus);
                }
            }
        }

        /* Read public key and include in response */
        char pubkey_buf[384] = {0};
        size_t pubkey_len = 0;
        int pubfd = open(pub_path, O_RDONLY);
        if (pubfd >= 0) {
            ssize_t nr = read(pubfd, pubkey_buf, sizeof(pubkey_buf) - 1);
            close(pubfd);
            if (nr > 0) {
                pubkey_buf[nr] = '\0';
                /* Strip trailing newline */
                while (nr > 0 && (pubkey_buf[nr-1] == '\n' || pubkey_buf[nr-1] == '\r'))
                    pubkey_buf[--nr] = '\0';
                pubkey_len = (size_t)nr;
            }
        }

        /*
         * Positive response with public key appended.
         * Format: 71 01 F0 A1 00 <pubkey_string_null_terminated>
         * The FC-1 extracts the pubkey and registers it with the bastion.
         */
        size_t resp_len = 5 + pubkey_len + (pubkey_len > 0 ? 1 : 0); /* +1 for null */
        if (resp_size < resp_len) return -1;

        response[0] = 0x71;    /* RoutineControl positive response */
        response[1] = 0x01;    /* subFunction: startRoutine */
        response[2] = 0xF0;    /* routineIdentifier high byte */
        response[3] = 0xA1;    /* routineIdentifier low byte */
        response[4] = 0x00;    /* routineStatus: routineAccepted */

        if (pubkey_len > 0) {
            memcpy(response + 5, pubkey_buf, pubkey_len + 1); /* include null */
            LOG_INFO("phonehome: returning pubkey (%zu bytes) in response for FC-1 registration",
                     pubkey_len);
        } else {
            LOG_WARN("phonehome: no public key available — FC-1 cannot register DCU");
        }

        return (int)resp_len;
    }
}

/* ============================================================================
 * Provisioning Status Query (UDS 0xF0A2 — called by FC-1 health check)
 * ========================================================================== */

int phonehome_handle_status(const uint8_t *uds_data, uint32_t uds_len,
                             uint8_t *response, uint32_t resp_size,
                             time_t server_start_time)
{
    (void)uds_data;
    (void)uds_len;

    if (resp_size < 10) return -1;

    /*
     * Evaluate provisioning state:
     *   0x00 = fully provisioned (HMAC + bastion key + SSH keys)
     *   0x01 = HMAC not loaded (needs full provisioning)
     *   0x02 = HMAC OK but bastion client key not installed
     *   0x03 = HMAC OK, key installed, but SSH keypair missing
     */
    uint8_t status = 0x00;

    if (!hmac_loaded) {
        status = 0x01;
    } else if (g_cfg == NULL || g_cfg->bastion_client_key[0] == '\0') {
        status = 0x02;
    } else {
        struct stat st;
        if (stat("/etc/phonehome/id_ed25519", &st) != 0) {
            status = 0x03;
        }
    }

    /* Verify imatrix user authorized_keys has content */
    if (status == 0x00) {
        struct stat ak;
        if (stat("/home/imatrix/.ssh/authorized_keys", &ak) != 0 || ak.st_size == 0) {
            status = 0x02;
        }
    }

    /* Build response: 71 01 F0 A2 <status> <uptime:4> <tunnel:1> */
    response[0] = 0x71;    /* RoutineControl positive response */
    response[1] = 0x01;    /* subFunction: startRoutine */
    response[2] = 0xF0;    /* routineIdentifier high */
    response[3] = 0xA2;    /* routineIdentifier low */
    response[4] = status;

    /* DCU uptime in seconds (big-endian) */
    uint32_t up = (uint32_t)(time(NULL) - server_start_time);
    response[5] = (uint8_t)(up >> 24);
    response[6] = (uint8_t)(up >> 16);
    response[7] = (uint8_t)(up >>  8);
    response[8] = (uint8_t)(up);

    /* Tunnel active flag */
    struct stat lock;
    response[9] = (stat("/etc/phonehome/phonehome.lock", &lock) == 0) ? 1 : 0;

    LOG_INFO("phonehome: status query — status=0x%02X, uptime=%us, tunnel=%s",
             status, up, response[9] ? "active" : "inactive");

    return 10;
}

/* ============================================================================
 * Status Query (for CLI)
 * ========================================================================== */

void phonehome_get_status(phonehome_status_t *status)
{
    memset(status, 0, sizeof(*status));

    if (g_cfg != NULL) {
        status->enabled = true;
        status->hmac_loaded = (hmac_loaded != 0);
        strncpy(status->bastion_host, g_cfg->bastion_host, sizeof(status->bastion_host) - 1);
        status->bastion_port = g_cfg->bastion_port;
        strncpy(status->ssh_user, g_cfg->ssh_user, sizeof(status->ssh_user) - 1);
        status->client_key_installed = (g_cfg->bastion_client_key[0] != '\0');
        strncpy(status->connect_script, g_cfg->connect_script, sizeof(status->connect_script) - 1);
        strncpy(status->lock_file, g_cfg->lock_file, sizeof(status->lock_file) - 1);
    }
}

/* ============================================================================
 * SSH User Setup
 * ========================================================================== */

int phonehome_ensure_ssh_user(const phonehome_config_t *cfg)
{
    if (!cfg || cfg->ssh_user[0] == '\0') {
        LOG_WARN("phonehome: no ssh_user configured, skipping user setup");
        return -1;
    }

    const char *user = cfg->ssh_user;
    char home_dir[128];
    char ssh_dir[160];
    char auth_keys_path[192];

    snprintf(home_dir, sizeof(home_dir), "/home/%s", user);
    snprintf(ssh_dir, sizeof(ssh_dir), "/home/%s/.ssh", user);
    snprintf(auth_keys_path, sizeof(auth_keys_path), "/home/%s/.ssh/authorized_keys", user);

    /* Check if user's home directory exists — proxy for user existence */
    struct stat st;
    if (stat(home_dir, &st) != 0) {
        /* User doesn't exist — create it */
        char cmd[256];
        snprintf(cmd, sizeof(cmd),
                 "useradd -m -s /bin/bash '%s' 2>/dev/null", user);
        int rc = system(cmd);
        if (rc != 0) {
            /* Try adduser for BusyBox/Alpine systems */
            snprintf(cmd, sizeof(cmd),
                     "adduser -D -s /bin/bash '%s' 2>/dev/null", user);
            rc = system(cmd);
            (void)rc;
        }
        if (stat(home_dir, &st) != 0) {
            LOG_ERROR("phonehome: failed to create user '%s'", user);
            return -1;
        }
        LOG_INFO("phonehome: created SSH user '%s'", user);
    } else {
        LOG_INFO("phonehome: SSH user '%s' already exists", user);
    }

    /* Ensure .ssh directory exists with correct permissions.
     * Use sudo if not running as root — the server may run as a
     * non-root user (e.g. development deployment) but needs to
     * write to another user's home directory. */
    bool use_sudo = (getuid() != 0);

    if (stat(ssh_dir, &st) != 0) {
        if (use_sudo) {
            char cmd[256];
            snprintf(cmd, sizeof(cmd),
                     "sudo mkdir -p '%s' && sudo chmod 700 '%s'",
                     ssh_dir, ssh_dir);
            int rc = system(cmd);
            if (rc != 0) {
                LOG_ERROR("phonehome: cannot create %s (sudo rc=%d)", ssh_dir, rc);
                return -1;
            }
        } else {
            if (mkdir(ssh_dir, 0700) != 0) {
                LOG_ERROR("phonehome: cannot create %s: %s", ssh_dir, strerror(errno));
                return -1;
            }
        }
    }
    if (!use_sudo) {
        chmod(ssh_dir, 0700);
    }

    /* Get user's uid/gid for chown */
    char id_cmd[256];
    int uid = -1, gid = -1;

    snprintf(id_cmd, sizeof(id_cmd), "id -u '%s' 2>/dev/null", user);
    FILE *fp = popen(id_cmd, "r");
    if (fp) { int r_ = fscanf(fp, "%d", &uid); (void)r_; pclose(fp); }

    snprintf(id_cmd, sizeof(id_cmd), "id -g '%s' 2>/dev/null", user);
    fp = popen(id_cmd, "r");
    if (fp) { int r_ = fscanf(fp, "%d", &gid); (void)r_; pclose(fp); }

    if (uid >= 0 && gid >= 0) {
        if (use_sudo) {
            char cmd[256];
            snprintf(cmd, sizeof(cmd),
                     "sudo chown %d:%d '%s'", uid, gid, ssh_dir);
            int r_ = system(cmd); (void)r_;
        } else {
            int r_ = chown(ssh_dir, (uid_t)uid, (gid_t)gid); (void)r_;
        }
    }

    /* Install bastion client key in authorized_keys if configured */
    if (cfg->bastion_client_key[0] != '\0') {
        /* Check if key is already present */
        bool key_present = false;
        FILE *check = fopen(auth_keys_path, "r");
        if (check) {
            char line[600];
            while (fgets(line, sizeof(line), check)) {
                if (strstr(line, cfg->bastion_client_key) != NULL) {
                    key_present = true;
                    break;
                }
            }
            fclose(check);
        }

        if (!key_present) {
            if (use_sudo) {
                /* Write via sudo tee — server doesn't own the target directory */
                char cmd[512];
                snprintf(cmd, sizeof(cmd),
                         "echo '%s' | sudo tee -a '%s' > /dev/null "
                         "&& sudo chmod 600 '%s'",
                         cfg->bastion_client_key, auth_keys_path, auth_keys_path);
                if (uid >= 0 && gid >= 0) {
                    char chown_cmd[128];
                    snprintf(chown_cmd, sizeof(chown_cmd),
                             " && sudo chown %d:%d '%s'", uid, gid, auth_keys_path);
                    strncat(cmd, chown_cmd, sizeof(cmd) - strlen(cmd) - 1);
                }
                int rc = system(cmd);
                if (rc != 0) {
                    LOG_ERROR("phonehome: cannot write %s (sudo rc=%d)", auth_keys_path, rc);
                    return -1;
                }
                LOG_INFO("phonehome: installed bastion client key in %s (via sudo)", auth_keys_path);
            } else {
                FILE *ak = fopen(auth_keys_path, "a");
                if (ak) {
                    fprintf(ak, "%s\n", cfg->bastion_client_key);
                    fclose(ak);
                    chmod(auth_keys_path, 0600);
                    if (uid >= 0 && gid >= 0) {
                        int r_ = chown(auth_keys_path, (uid_t)uid, (gid_t)gid); (void)r_;
                    }
                    LOG_INFO("phonehome: installed bastion client key in %s", auth_keys_path);
                } else {
                    LOG_ERROR("phonehome: cannot write %s: %s", auth_keys_path, strerror(errno));
                    return -1;
                }
            }
        } else {
            LOG_INFO("phonehome: bastion client key already in %s", auth_keys_path);
        }
    } else {
        LOG_ERROR("phonehome: no BASTION_CLIENT_KEY — cannot install bastion auth key");
    }

    return 0;
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
