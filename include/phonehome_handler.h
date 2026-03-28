/**
 * @file phonehome_handler.h
 * @brief DCU Phone-Home RoutineControl handler
 *
 * Handles UDS RoutineControl (SID 0x31) with routineIdentifier 0xF0A0
 * to trigger reverse SSH tunnels per DCU_PhoneHome_Specification.md.
 */

#ifndef PHONEHOME_HANDLER_H
#define PHONEHOME_HANDLER_H

#include <stdint.h>
#include <stdbool.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Vendor-specific routine identifier for phone-home trigger */
#define ROUTINE_ID_PHONEHOME            0xF0A0

/** Vendor-specific routine identifier for phone-home provisioning */
#define ROUTINE_ID_PHONEHOME_PROVISION  0xF0A1

/** Vendor-specific routine identifier for provisioning status query */
#define ROUTINE_ID_PHONEHOME_STATUS     0xF0A2

/** Phone-home configuration (loaded from /etc/phonehome/phonehome.conf) */
typedef struct {
    char bastion_host[254];         /* Bastion hostname (from FC-1 provision or config) */
    uint16_t bastion_port;          /* Bastion SSH port (from FC-1 provision, 0=default) */
    char hmac_secret_path[256];     /* Path to 32-byte HMAC shared secret */
    char connect_script[256];       /* Path to phonehome-connect.sh */
    char lock_file[256];            /* Path to tunnel lock file */
    char bastion_client_key[512];   /* Bastion SSH client pubkey (installed in imatrix authorized_keys) */
    char ssh_user[64];              /* SSH user for bastion inbound connection (default: imatrix) */
    char ssh_ca_pubkey[512];        /* SSH CA public key (required — phone-home disabled without it) */
} phonehome_config_t;

/** Phone-home status snapshot (for CLI display) */
typedef struct {
    bool enabled;                   /* phonehome_init succeeded */
    bool hmac_loaded;               /* HMAC secret loaded in memory */
    char bastion_host[254];         /* Current bastion hostname */
    uint16_t bastion_port;          /* Current bastion port */
    char ssh_user[64];              /* SSH user for inbound connections */
    bool client_key_installed;      /* Bastion client key in authorized_keys */
    char connect_script[256];       /* Path to connect script */
    char lock_file[256];            /* Path to lock file */
} phonehome_status_t;

/**
 * @brief Get current phone-home subsystem status (for CLI)
 * @param status  Output struct (zeroed first)
 */
void phonehome_get_status(phonehome_status_t *status);

/**
 * @brief Ensure the SSH service user exists and authorized_keys is configured.
 *
 * Creates the 'imatrix' system user (or whatever ssh_user is configured)
 * if it doesn't exist. Sets up ~/.ssh/authorized_keys with the bastion
 * client public key so the Bastion web-ssh app can authenticate through
 * the reverse tunnel.
 *
 * Called from main() during server startup, after config is loaded.
 *
 * @param cfg  Phone-home config with ssh_user and bastion_client_key
 * @return 0 on success, -1 on failure (non-fatal — server continues)
 */
int phonehome_ensure_ssh_user(const phonehome_config_t *cfg);

/**
 * @brief Load phone-home config from KEY=VALUE file
 * @param cfg   Config struct to populate (zeroed first)
 * @param path  Path to phonehome.conf
 * @return 0 on success, -1 on file open error or missing required keys
 *
 * Required keys: HMAC_SECRET_FILE, CONNECT_SCRIPT
 * Optional keys: BASTION_HOST (default: "bastion-dev.imatrixsys.com"),
 *                LOCK_FILE (default: "/var/run/phonehome.lock")
 */
int phonehome_config_load(phonehome_config_t *cfg, const char *path);

/**
 * @brief Initialize phone-home subsystem
 * @param cfg  Configuration (must remain valid for lifetime of handler)
 * @return 0 on success, -1 on failure (phone-home disabled, server continues)
 *
 * Loads HMAC secret from cfg->hmac_secret_path using O_NOFOLLOW.
 * Logs strerror(errno) on failure to distinguish ENOENT/EACCES/ELOOP.
 */
int phonehome_init(const phonehome_config_t *cfg);

/**
 * @brief Handle RoutineControl 0x31/0x01/0xF0A0
 * @param uds_data   Full UDS request (starting with SID 0x31)
 * @param uds_len    Length of UDS request
 * @param response   Buffer for UDS response
 * @param resp_size  Size of response buffer
 * @return UDS response length, or negative on internal error
 *
 * Thread-safe. Uses internal replay_mutex and phonehome_fork_mutex.
 * Does NOT require g_transfer_mutex to be held by caller.
 */
int phonehome_handle_routine(const uint8_t *uds_data, uint32_t uds_len,
                             uint8_t *response, uint32_t resp_size);

/**
 * @brief Handle phone-home provisioning (routineId 0xF0A1)
 *
 * Receives HMAC secret from FC-1 and writes it to disk, then loads
 * it into memory. On a fresh DCU where phonehome_init() was never
 * called, also initializes g_cfg with default paths so that
 * subsequent phonehome_handle_routine() calls have valid config.
 *
 * @param uds_data   Full UDS request (starting with SID 0x31)
 * @param uds_len    Length of UDS request (must be 40 bytes)
 * @param response   Buffer for UDS response
 * @param resp_size  Size of response buffer
 * @return UDS response length, or negative on internal error
 *
 * Thread-safe. Uses internal hmac_mutex.
 */
int phonehome_handle_provision(const uint8_t *uds_data, uint32_t uds_len,
                                uint8_t *response, uint32_t resp_size);

/**
 * @brief Handle provisioning status query (routineId 0xF0A2)
 *
 * Returns current provisioning state so the FC-1 can detect DCU restarts
 * and re-provision if needed. Response includes status byte, uptime,
 * and tunnel state.
 *
 * @param uds_data   Full UDS request (starting with SID 0x31)
 * @param uds_len    Length of UDS request
 * @param response   Buffer for UDS response
 * @param resp_size  Size of response buffer
 * @param server_start_time  Server start time for uptime calculation
 * @return UDS response length, or negative on internal error
 */
int phonehome_handle_status(const uint8_t *uds_data, uint32_t uds_len,
                             uint8_t *response, uint32_t resp_size,
                             time_t server_start_time);

/**
 * @brief Cleanup phone-home subsystem
 *
 * Clears HMAC secret from memory via explicit_bzero().
 * Call after all server threads have exited.
 */
void phonehome_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif /* PHONEHOME_HANDLER_H */
