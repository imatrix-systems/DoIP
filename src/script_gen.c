/**
 * @file script_gen.c
 * @brief Self-generation of operational scripts from embedded string literals
 *
 * Complete per-platform script variants selected at compile time via
 * PLATFORM_TORIZON / PLATFORM_UBUNTU. No template substitution engine.
 * Scripts read config files at runtime — no config values are embedded.
 */

#define _POSIX_C_SOURCE 200809L

#include "script_gen.h"
#include "doip_log.h"

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>

/* ============================================================================
 * Embedded Scripts — Phone-Home Keygen
 * ========================================================================== */

#ifdef PLATFORM_TORIZON

static const char KEYGEN_SCRIPT[] =
"#!/bin/sh\n"
"# phonehome-keygen.sh (Torizon/Dropbear)\n"
"# Generates DCU SSH key pair for phone-home function.\n"
"set -e\n"
"\n"
"PHONEHOME_DIR=\"/etc/phonehome\"\n"
"KEY_FILE=\"${PHONEHOME_DIR}/id_ed25519\"\n"
"SENTINEL=\"${PHONEHOME_DIR}/.keygen_complete\"\n"
"SERIAL_FILE=\"/etc/dcu-serial\"\n"
"LOG_TAG=\"phonehome-keygen\"\n"
"\n"
"log() { logger -t \"$LOG_TAG\" \"$1\"; }\n"
"\n"
"[ -f \"$SENTINEL\" ] && { log \"Keys already generated. Exiting.\"; exit 0; }\n"
"\n"
"install -d -m 700 -o root -g root \"$PHONEHOME_DIR\"\n"
"\n"
"ENTROPY=$(cat /proc/sys/kernel/random/entropy_avail)\n"
"if [ \"$ENTROPY\" -lt 256 ]; then\n"
"    log \"ERROR: Insufficient entropy ($ENTROPY bits). Cannot generate keys safely.\"\n"
"    exit 1\n"
"fi\n"
"\n"
"if [ ! -f \"$SERIAL_FILE\" ]; then\n"
"    log \"ERROR: Serial number file not found at $SERIAL_FILE\"\n"
"    exit 1\n"
"fi\n"
"\n"
"# Dropbear key generation\n"
"dropbearkey -t ed25519 -f \"$KEY_FILE\"\n"
"\n"
"# Extract public key (dropbear format)\n"
"dropbearkey -y -f \"$KEY_FILE\" | grep \"^ssh-\" > \"${KEY_FILE}.pub\"\n"
"\n"
"chmod 600 \"$KEY_FILE\"\n"
"chmod 644 \"${KEY_FILE}.pub\"\n"
"chown root:root \"$KEY_FILE\" \"${KEY_FILE}.pub\"\n"
"\n"
"touch \"$SENTINEL\"\n"
"chmod 600 \"$SENTINEL\"\n"
"\n"
"DCU_SERIAL=$(cat \"$SERIAL_FILE\" | tr -d '[:space:]')\n"
"log \"Key generation complete for DCU serial: $DCU_SERIAL\"\n"
"log \"Public key: $(cat ${KEY_FILE}.pub)\"\n";

#else /* PLATFORM_UBUNTU */

static const char KEYGEN_SCRIPT[] =
"#!/bin/sh\n"
"# phonehome-keygen.sh (Ubuntu/OpenSSH)\n"
"# Generates DCU SSH key pair for phone-home function.\n"
"set -e\n"
"\n"
"PHONEHOME_DIR=\"/etc/phonehome\"\n"
"KEY_FILE=\"${PHONEHOME_DIR}/id_ed25519\"\n"
"SENTINEL=\"${PHONEHOME_DIR}/.keygen_complete\"\n"
"SERIAL_FILE=\"/etc/dcu-serial\"\n"
"LOG_TAG=\"phonehome-keygen\"\n"
"\n"
"log() { logger -t \"$LOG_TAG\" \"$1\"; }\n"
"\n"
"[ -f \"$SENTINEL\" ] && { log \"Keys already generated. Exiting.\"; exit 0; }\n"
"\n"
"install -d -m 700 -o root -g root \"$PHONEHOME_DIR\"\n"
"\n"
"ENTROPY=$(cat /proc/sys/kernel/random/entropy_avail)\n"
"if [ \"$ENTROPY\" -lt 256 ]; then\n"
"    log \"ERROR: Insufficient entropy ($ENTROPY bits). Cannot generate keys safely.\"\n"
"    exit 1\n"
"fi\n"
"\n"
"if [ ! -f \"$SERIAL_FILE\" ]; then\n"
"    log \"ERROR: Serial number file not found at $SERIAL_FILE\"\n"
"    exit 1\n"
"fi\n"
"DCU_SERIAL=$(cat \"$SERIAL_FILE\" | tr -d '[:space:]')\n"
"\n"
"ssh-keygen -t ed25519 -f \"$KEY_FILE\" -N \"\" -C \"dcu-${DCU_SERIAL}\"\n"
"\n"
"chmod 600 \"$KEY_FILE\"\n"
"chmod 644 \"${KEY_FILE}.pub\"\n"
"chown root:root \"$KEY_FILE\" \"${KEY_FILE}.pub\"\n"
"\n"
"touch \"$SENTINEL\"\n"
"chmod 600 \"$SENTINEL\"\n"
"\n"
"log \"Key generation complete for DCU serial: $DCU_SERIAL\"\n"
"log \"Public key: $(cat ${KEY_FILE}.pub)\"\n";

#endif /* PLATFORM */

/* ============================================================================
 * Embedded Scripts — Phone-Home Register
 * ========================================================================== */

#ifdef PLATFORM_TORIZON

static const char REGISTER_SCRIPT[] =
"#!/bin/sh\n"
"# phonehome-register.sh (Torizon/wget)\n"
"# Registers DCU public key with Bastion via HTTPS.\n"
"set -e\n"
"\n"
"PHONEHOME_DIR=\"/etc/phonehome\"\n"
"KEY_FILE=\"${PHONEHOME_DIR}/id_ed25519\"\n"
"SENTINEL=\"${PHONEHOME_DIR}/.registration_complete\"\n"
"SERIAL_FILE=\"/etc/dcu-serial\"\n"
"FC1_SERIAL_FILE=\"/etc/fc1-serial\"\n"
"BASTION_REG_URL=\"https://bastion.example.com/api/v1/devices/register\"\n"
"PROVISIONING_TOKEN_FILE=\"${PHONEHOME_DIR}/provisioning_token\"\n"
"CA_CERT=\"/etc/phonehome/bastion-ca.crt\"\n"
"MAX_RETRIES=10\n"
"RETRY_INTERVAL=60\n"
"LOG_TAG=\"phonehome-register\"\n"
"\n"
"log() { logger -t \"$LOG_TAG\" \"$1\"; }\n"
"\n"
"[ -f \"$SENTINEL\" ] && { log \"Already registered. Exiting.\"; exit 0; }\n"
"\n"
"DCU_SERIAL=$(cat \"$SERIAL_FILE\" | tr -d '[:space:]')\n"
"FC1_SERIAL=$(cat \"$FC1_SERIAL_FILE\" | tr -d '[:space:]')\n"
"PUBLIC_KEY=$(cat \"${KEY_FILE}.pub\")\n"
"PROV_TOKEN=$(cat \"$PROVISIONING_TOKEN_FILE\" | tr -d '[:space:]')\n"
"\n"
"case \"$DCU_SERIAL\" in *[!A-Za-z0-9-]*) log \"ERROR: DCU serial contains invalid chars\"; exit 1;; esac\n"
"case \"$FC1_SERIAL\" in *[!A-Za-z0-9-]*) log \"ERROR: FC1 serial contains invalid chars\"; exit 1;; esac\n"
"\n"
"RESP_FILE=$(mktemp /tmp/phonehome_reg_XXXXXX)\n"
"trap 'rm -f \"$RESP_FILE\"' EXIT\n"
"\n"
"JSON=\"{\\\"serial\\\": \\\"$DCU_SERIAL\\\", \\\"fc1_serial\\\": \\\"$FC1_SERIAL\\\", \\\"device_type\\\": \\\"DCU\\\", \\\"public_key\\\": \\\"$PUBLIC_KEY\\\"}\"\n"
"\n"
"attempt=0\n"
"while [ $attempt -lt $MAX_RETRIES ]; do\n"
"    attempt=$((attempt + 1))\n"
"    log \"Registration attempt $attempt of $MAX_RETRIES\"\n"
"\n"
"    if wget --ca-certificate=\"$CA_CERT\" \\\n"
"            --header=\"Authorization: Bearer $PROV_TOKEN\" \\\n"
"            --header=\"Content-Type: application/json\" \\\n"
"            --post-data=\"$JSON\" \\\n"
"            --timeout=30 \\\n"
"            -O \"$RESP_FILE\" \\\n"
"            \"$BASTION_REG_URL\" 2>/dev/null; then\n"
"        log \"Registration successful\"\n"
"        touch \"$SENTINEL\"\n"
"        # Securely delete provisioning token\n"
"        dd if=/dev/urandom of=\"$PROVISIONING_TOKEN_FILE\" bs=32 count=1 2>/dev/null\n"
"        rm -f \"$PROVISIONING_TOKEN_FILE\"\n"
"        rm -f \"$RESP_FILE\"\n"
"        exit 0\n"
"    else\n"
"        log \"Registration failed. Response: $(cat \"$RESP_FILE\" 2>/dev/null)\"\n"
"        sleep $RETRY_INTERVAL\n"
"    fi\n"
"done\n"
"\n"
"log \"ERROR: Registration failed after $MAX_RETRIES attempts.\"\n"
"exit 1\n";

#else /* PLATFORM_UBUNTU */

static const char REGISTER_SCRIPT[] =
"#!/bin/sh\n"
"# phonehome-register.sh (Ubuntu/curl)\n"
"# Registers DCU public key with Bastion via HTTPS.\n"
"set -e\n"
"\n"
"PHONEHOME_DIR=\"/etc/phonehome\"\n"
"KEY_FILE=\"${PHONEHOME_DIR}/id_ed25519\"\n"
"SENTINEL=\"${PHONEHOME_DIR}/.registration_complete\"\n"
"SERIAL_FILE=\"/etc/dcu-serial\"\n"
"FC1_SERIAL_FILE=\"/etc/fc1-serial\"\n"
"BASTION_REG_URL=\"https://bastion.example.com/api/v1/devices/register\"\n"
"PROVISIONING_TOKEN_FILE=\"${PHONEHOME_DIR}/provisioning_token\"\n"
"CA_CERT=\"/etc/phonehome/bastion-ca.crt\"\n"
"MAX_RETRIES=10\n"
"RETRY_INTERVAL=60\n"
"LOG_TAG=\"phonehome-register\"\n"
"\n"
"log() { logger -t \"$LOG_TAG\" \"$1\"; }\n"
"\n"
"[ -f \"$SENTINEL\" ] && { log \"Already registered. Exiting.\"; exit 0; }\n"
"\n"
"DCU_SERIAL=$(cat \"$SERIAL_FILE\" | tr -d '[:space:]')\n"
"FC1_SERIAL=$(cat \"$FC1_SERIAL_FILE\" | tr -d '[:space:]')\n"
"PUBLIC_KEY=$(cat \"${KEY_FILE}.pub\")\n"
"PROV_TOKEN=$(cat \"$PROVISIONING_TOKEN_FILE\" | tr -d '[:space:]')\n"
"\n"
"case \"$DCU_SERIAL\" in *[!A-Za-z0-9-]*) log \"ERROR: DCU serial contains invalid chars\"; exit 1;; esac\n"
"case \"$FC1_SERIAL\" in *[!A-Za-z0-9-]*) log \"ERROR: FC1 serial contains invalid chars\"; exit 1;; esac\n"
"\n"
"RESP_FILE=$(mktemp /tmp/phonehome_reg_XXXXXX)\n"
"trap 'rm -f \"$RESP_FILE\"' EXIT\n"
"\n"
"attempt=0\n"
"while [ $attempt -lt $MAX_RETRIES ]; do\n"
"    attempt=$((attempt + 1))\n"
"    log \"Registration attempt $attempt of $MAX_RETRIES\"\n"
"\n"
"    HTTP_STATUS=$(curl -s -o \"$RESP_FILE\" -w \"%%{http_code}\" \\\n"
"        --cacert \"$CA_CERT\" \\\n"
"        --max-time 30 \\\n"
"        -X POST \"$BASTION_REG_URL\" \\\n"
"        -H \"Authorization: Bearer $PROV_TOKEN\" \\\n"
"        -H \"Content-Type: application/json\" \\\n"
"        -d \"{\n"
"            \\\"serial\\\": \\\"$DCU_SERIAL\\\",\n"
"            \\\"fc1_serial\\\": \\\"$FC1_SERIAL\\\",\n"
"            \\\"device_type\\\": \\\"DCU\\\",\n"
"            \\\"public_key\\\": \\\"$PUBLIC_KEY\\\"\n"
"        }\")\n"
"\n"
"    if [ \"$HTTP_STATUS\" = \"200\" ] || [ \"$HTTP_STATUS\" = \"201\" ]; then\n"
"        log \"Registration successful (HTTP $HTTP_STATUS)\"\n"
"        touch \"$SENTINEL\"\n"
"        shred -u \"$PROVISIONING_TOKEN_FILE\" 2>/dev/null || rm -f \"$PROVISIONING_TOKEN_FILE\"\n"
"        rm -f \"$RESP_FILE\"\n"
"        exit 0\n"
"    else\n"
"        log \"Registration failed (HTTP $HTTP_STATUS). Response: $(cat \"$RESP_FILE\")\"\n"
"        sleep $RETRY_INTERVAL\n"
"    fi\n"
"done\n"
"\n"
"log \"ERROR: Registration failed after $MAX_RETRIES attempts.\"\n"
"exit 1\n";

#endif /* PLATFORM */

/* ============================================================================
 * Embedded Scripts — Phone-Home Connect (same on both platforms)
 * ========================================================================== */

static const char CONNECT_SCRIPT[] =
"#!/bin/sh\n"
"# phonehome-connect.sh\n"
"# Opens a reverse SSH tunnel to the Bastion server.\n"
"# Called with: phonehome-connect.sh <bastion_host> <remote_port> <nonce>\n"
"set -e\n"
"\n"
"BASTION_HOST=\"${1:-bastion.example.com}\"\n"
"REMOTE_PORT=\"${2:-0}\"\n"
"NONCE=\"$3\"\n"
"LOCAL_SSH_PORT=22\n"
"PHONEHOME_DIR=\"/etc/phonehome\"\n"
"KEY_FILE=\"${PHONEHOME_DIR}/id_ed25519\"\n"
"KNOWN_HOSTS=\"${PHONEHOME_DIR}/known_hosts\"\n"
"SERIAL_FILE=\"/etc/dcu-serial\"\n"
"DCU_SERIAL=$(cat \"$SERIAL_FILE\" | tr -d '[:space:]')\n"
"LOG_TAG=\"phonehome-connect\"\n"
"TUNNEL_TIMEOUT=3600\n"
"LOCK_FILE=\"/var/run/phonehome.lock\"\n"
"\n"
"log() { logger -t \"$LOG_TAG\" \"$1\"; }\n"
"\n"
"if [ -f \"$LOCK_FILE\" ]; then\n"
"    PID=$(cat \"$LOCK_FILE\")\n"
"    if kill -0 \"$PID\" 2>/dev/null; then\n"
"        log \"Tunnel already active (PID $PID). Ignoring duplicate trigger.\"\n"
"        exit 0\n"
"    fi\n"
"fi\n"
"\n"
"echo $$ > \"$LOCK_FILE\"\n"
"trap 'rm -f \"$LOCK_FILE\"; log \"Tunnel closed.\"' EXIT\n"
"\n"
"log \"Phone-home triggered. Serial=$DCU_SERIAL Nonce=$NONCE Bastion=$BASTION_HOST RemotePort=$REMOTE_PORT\"\n"
"\n"
"[ -f \"$KEY_FILE\" ] || { log \"ERROR: Private key not found at $KEY_FILE\"; exit 1; }\n"
"[ -f \"$KNOWN_HOSTS\" ] || { log \"ERROR: known_hosts not found at $KNOWN_HOSTS\"; exit 1; }\n"
"\n"
"timeout \"$TUNNEL_TIMEOUT\" ssh \\\n"
"    -N -T \\\n"
"    -i \"$KEY_FILE\" \\\n"
"    -o StrictHostKeyChecking=yes \\\n"
"    -o UserKnownHostsFile=\"$KNOWN_HOSTS\" \\\n"
"    -o ExitOnForwardFailure=yes \\\n"
"    -o ServerAliveInterval=30 \\\n"
"    -o ServerAliveCountMax=3 \\\n"
"    -o BatchMode=yes \\\n"
"    -o ConnectTimeout=30 \\\n"
"    -R \"${REMOTE_PORT}:localhost:${LOCAL_SSH_PORT}\" \\\n"
"    \"phonehome-${DCU_SERIAL}@${BASTION_HOST}\" \\\n"
"    || log \"SSH tunnel exited with code $?\"\n";

/* ============================================================================
 * Embedded Scripts — DoIP Server systemd unit
 * ========================================================================== */

static const char DOIP_SYSTEMD_UNIT[] =
"[Unit]\n"
"Description=DoIP Blob Server\n"
"After=network.target\n"
"Wants=network.target\n"
"\n"
"[Service]\n"
"Type=forking\n"
"ExecStart=/usr/sbin/doip-server -d -c /etc/doip/doip-server.conf\n"
"PIDFile=/var/run/doip-server.pid\n"
"Restart=on-failure\n"
"RestartSec=5\n"
"StandardOutput=journal\n"
"StandardError=journal\n"
"\n"
"[Install]\n"
"WantedBy=multi-user.target\n";

/* ============================================================================
 * Embedded Scripts — DoIP Server init.d script
 * ========================================================================== */

static const char DOIP_INITD_SCRIPT[] =
"#!/bin/sh\n"
"### BEGIN INIT INFO\n"
"# Provides:          doip-server\n"
"# Required-Start:    $network $syslog\n"
"# Required-Stop:     $network $syslog\n"
"# Default-Start:     2 3 4 5\n"
"# Default-Stop:      0 1 6\n"
"# Short-Description: DoIP Blob Server\n"
"# Description:       Receives data blobs from Fleet-Connect-1 via DoIP/UDS.\n"
"### END INIT INFO\n"
"\n"
"DAEMON=\"/usr/sbin/doip-server\"\n"
"DAEMON_ARGS=\"-d -c /etc/doip/doip-server.conf\"\n"
"PIDFILE=\"/var/run/doip-server.pid\"\n"
"NAME=\"doip-server\"\n"
"\n"
"case \"$1\" in\n"
"    start)\n"
"        echo \"Starting $NAME...\"\n"
"        $DAEMON $DAEMON_ARGS\n"
"        ;;\n"
"    stop)\n"
"        echo \"Stopping $NAME...\"\n"
"        if [ -f \"$PIDFILE\" ]; then\n"
"            kill $(cat \"$PIDFILE\") 2>/dev/null\n"
"            rm -f \"$PIDFILE\"\n"
"        fi\n"
"        ;;\n"
"    restart)\n"
"        $0 stop\n"
"        sleep 1\n"
"        $0 start\n"
"        ;;\n"
"    status)\n"
"        if [ -f \"$PIDFILE\" ] && kill -0 $(cat \"$PIDFILE\") 2>/dev/null; then\n"
"            echo \"$NAME is running (PID $(cat \"$PIDFILE\"))\"\n"
"        else\n"
"            echo \"$NAME is not running\"\n"
"        fi\n"
"        ;;\n"
"    *)\n"
"        echo \"Usage: $0 {start|stop|restart|status}\"\n"
"        exit 1\n"
"        ;;\n"
"esac\n"
"\n"
"exit 0\n";

/* ============================================================================
 * Script Table
 * ========================================================================== */

typedef struct {
    const char *filename;
    const char *content;
    mode_t      mode;
} script_entry_t;

static const script_entry_t SCRIPTS[] = {
    { "phonehome-keygen.sh",     KEYGEN_SCRIPT,     0755 },
    { "phonehome-register.sh",   REGISTER_SCRIPT,   0755 },
    { "phonehome-connect.sh",    CONNECT_SCRIPT,    0755 },
    { "doip-server.service",     DOIP_SYSTEMD_UNIT, 0644 },
    { "doip-server-initd",       DOIP_INITD_SCRIPT, 0755 },
};

#define SCRIPT_COUNT  (int)(sizeof(SCRIPTS) / sizeof(SCRIPTS[0]))

/* ============================================================================
 * Public API
 * ========================================================================== */

static int write_script(const char *dir, const script_entry_t *entry)
{
    char path[512];
    int n = snprintf(path, sizeof(path), "%s/%s", dir, entry->filename);
    if (n < 0 || (size_t)n >= sizeof(path)) {
        LOG_WARN("script_gen: path too long for %s", entry->filename);
        return -1;
    }

    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0);
    if (fd < 0) {
        LOG_WARN("script_gen: cannot create %s: %s", path, strerror(errno));
        return -1;
    }

    if (fchmod(fd, entry->mode) != 0) {
        LOG_WARN("script_gen: fchmod %s failed: %s", path, strerror(errno));
        close(fd);
        return -1;
    }

    size_t len = strlen(entry->content);
    const char *p = entry->content;
    while (len > 0) {
        ssize_t w = write(fd, p, len);
        if (w < 0) {
            if (errno == EINTR) continue;
            LOG_WARN("script_gen: write to %s failed: %s", path, strerror(errno));
            close(fd);
            return -1;
        }
        p += w;
        len -= (size_t)w;
    }

    close(fd);
    LOG_INFO("script_gen: wrote %s (%zu bytes, mode %04o)",
             path, strlen(entry->content), (unsigned)entry->mode);
    return 0;
}

static int ensure_dir(const char *dir)
{
    struct stat st;
    if (stat(dir, &st) == 0) {
        if (!S_ISDIR(st.st_mode)) {
            LOG_ERROR("script_gen: %s exists but is not a directory", dir);
            return -1;
        }
        return 0;
    }
    if (mkdir(dir, 0755) != 0 && errno != EEXIST) {
        LOG_ERROR("script_gen: cannot create directory %s: %s", dir, strerror(errno));
        return -1;
    }
    return 0;
}

int script_gen_write_all(const doip_app_config_t *config)
{
    if (!config) return -1;
    return script_gen_write_to(config->script_output_dir);
}

int script_gen_write_to(const char *output_dir)
{
    if (!output_dir || output_dir[0] == '\0') {
        LOG_ERROR("script_gen: no output directory specified");
        return -1;
    }

    if (ensure_dir(output_dir) != 0)
        return -1;

    int written = 0;
    for (int i = 0; i < SCRIPT_COUNT; i++) {
        if (write_script(output_dir, &SCRIPTS[i]) == 0)
            written++;
    }

    LOG_INFO("script_gen: %d of %d scripts written to %s",
             written, SCRIPT_COUNT, output_dir);
    return written;
}

int script_gen_count(void)
{
    return SCRIPT_COUNT;
}

