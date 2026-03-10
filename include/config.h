/**
 * @file config.h
 * @brief DoIP Server configuration file parser
 *
 * Provides key=value configuration file support for the DoIP server.
 * Wraps doip_server_config_t with additional application-level settings.
 */

#ifndef DOIP_CONFIG_H
#define DOIP_CONFIG_H

#include "doip_server.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    doip_server_config_t server;        /* Identity + network (passed to doip_server_init) */
    char    blob_storage_dir[256];      /* Blob output path */
    uint32_t blob_max_size;             /* Max blob bytes */
    uint32_t transfer_timeout_sec;      /* Transfer timeout */
    char    bind_address_buf[64];       /* Owned storage for bind_address pointer */
    char    phonehome_config_path[256]; /* Path to phonehome.conf, empty = disabled */
    bool    daemon_mode;                /* Run as background daemon (-d) */
    char    pid_file[256];              /* PID file path for daemon mode */
    char    script_output_dir[256];     /* Directory for generated scripts */
} doip_app_config_t;

/**
 * @brief Initialize config with safe defaults
 *
 * server.bind_address = NULL (INADDR_ANY)
 * server.tcp_port = 0, server.udp_port = 0 (defaulted to 13400 by doip_server_init)
 * Identity fields match the original hardcoded values in main.c
 */
void doip_config_defaults(doip_app_config_t *config);

/**
 * @brief Load configuration from a key=value file
 * @param config   Config struct (should be initialized with defaults first)
 * @param path     Path to config file
 * @return 0 on success, -1 on file open error
 */
int doip_config_load(doip_app_config_t *config, const char *path);

/**
 * @brief Print configuration to stdout
 */
void doip_config_print(const doip_app_config_t *config);

#ifdef __cplusplus
}
#endif

#endif /* DOIP_CONFIG_H */
