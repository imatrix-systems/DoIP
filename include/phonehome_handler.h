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

#ifdef __cplusplus
extern "C" {
#endif

/** Vendor-specific routine identifier for phone-home trigger */
#define ROUTINE_ID_PHONEHOME    0xF0A0

/** Phone-home configuration (loaded from /etc/phonehome/phonehome.conf) */
typedef struct {
    char bastion_host[254];         /* Default bastion hostname */
    char hmac_secret_path[256];     /* Path to 32-byte HMAC shared secret */
    char connect_script[256];       /* Path to phonehome-connect.sh */
    char lock_file[256];            /* Path to tunnel lock file */
} phonehome_config_t;

/**
 * @brief Load phone-home config from KEY=VALUE file
 * @param cfg   Config struct to populate (zeroed first)
 * @param path  Path to phonehome.conf
 * @return 0 on success, -1 on file open error or missing required keys
 *
 * Required keys: HMAC_SECRET_FILE, CONNECT_SCRIPT
 * Optional keys: BASTION_HOST (default: "bastion.example.com"),
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
