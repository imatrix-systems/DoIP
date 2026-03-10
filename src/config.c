/**
 * @file config.c
 * @brief Key=value configuration file parser for DoIP server
 */

#define _POSIX_C_SOURCE 200809L
#include "config.h"
#include "config_parse.h"
#include "doip_log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <limits.h>

/**
 * Parse colon-separated hex bytes (e.g. "00:1A:2B:3C:4D:5E")
 * @return 0 on success, -1 on error
 */
static int parse_hex_bytes(const char *str, uint8_t *out, int expected_len)
{
    char buf[64];
    if (strlen(str) >= sizeof(buf)) return -1;
    strcpy(buf, str);

    char *saveptr = NULL;
    char *token = strtok_r(buf, ":", &saveptr);
    int count = 0;

    while (token) {
        if (count >= expected_len) return -1;

        char *endptr;
        unsigned long val = strtoul(token, &endptr, 16);
        if (endptr == token || *endptr != '\0' || val > 0xFF)
            return -1;

        out[count++] = (uint8_t)val;
        token = strtok_r(NULL, ":", &saveptr);
    }

    return (count == expected_len) ? 0 : -1;
}

/**
 * Parse an unsigned integer with range checking
 * @return 0 on success, -1 on error
 */
static int parse_uint(const char *value, unsigned long *out, unsigned long max_val)
{
    char *endptr;
    unsigned long val = strtoul(value, &endptr, 0);
    if (endptr == value) return -1;

    /* Allow trailing whitespace */
    while (*endptr && isspace((unsigned char)*endptr))
        endptr++;
    if (*endptr != '\0') return -1;

    if (val > max_val) return -1;

    *out = val;
    return 0;
}

/* ============================================================================
 * Public API
 * ========================================================================== */

void doip_config_defaults(doip_app_config_t *config)
{
    memset(config, 0, sizeof(*config));

    /* Identity — matches original hardcoded values */
    memcpy(config->server.vin, "FC1BLOBSRV0000001", DOIP_VIN_LENGTH);
    config->server.logical_address = 0x0001;
    config->server.eid[0] = 0x00; config->server.eid[1] = 0x1A;
    config->server.eid[2] = 0x2B; config->server.eid[3] = 0x3C;
    config->server.eid[4] = 0x4D; config->server.eid[5] = 0x5E;
    memcpy(config->server.gid, config->server.eid, DOIP_GID_LENGTH);
    config->server.further_action = 0x00;
    config->server.vin_gid_sync_status = 0x00;

    /* Network — NULL/0 means doip_server_init() will apply its own defaults */
    config->server.bind_address = NULL;
    config->server.tcp_port = 0;
    config->server.udp_port = 0;
    config->server.max_tcp_connections = 4;
    config->server.max_data_size = DOIP_MAX_DIAGNOSTIC_SIZE;

    /* Application */
    strncpy(config->blob_storage_dir, "/tmp/doip_blobs", sizeof(config->blob_storage_dir) - 1);
    config->blob_storage_dir[sizeof(config->blob_storage_dir) - 1] = '\0';
    config->blob_max_size = 16 * 1024 * 1024;
    config->transfer_timeout_sec = 30;

    /* Daemon / CLI */
    config->daemon_mode = false;
    strncpy(config->pid_file, "/var/run/doip-server.pid", sizeof(config->pid_file) - 1);
    config->pid_file[sizeof(config->pid_file) - 1] = '\0';
    strncpy(config->script_output_dir, "/etc/doip/scripts", sizeof(config->script_output_dir) - 1);
    config->script_output_dir[sizeof(config->script_output_dir) - 1] = '\0';
}

int doip_config_load(doip_app_config_t *config, const char *path)
{
    FILE *fp = fopen(path, "r");
    if (!fp) return -1;

    char line[512];
    int line_num = 0;

    while (fgets(line, sizeof(line), fp)) {
        line_num++;

        /* Detect truncated lines (no newline and not at EOF) */
        size_t len = strlen(line);
        if (len > 0 && line[len - 1] != '\n' && !feof(fp)) {
            fprintf(stderr, "[Config] Warning: line %d too long, skipping\n", line_num);
            /* Discard remainder of the long line */
            int ch;
            while ((ch = fgetc(fp)) != '\n' && ch != EOF)
                ;
            continue;
        }

        /* Strip newline */
        if (len > 0 && line[len - 1] == '\n')
            line[len - 1] = '\0';

        /* Trim leading whitespace */
        char *p = cfg_trim_leading(line);

        /* Skip blank lines and comments */
        if (*p == '\0' || *p == '#')
            continue;

        /* Find '=' separator */
        char *eq = strchr(p, '=');
        if (!eq) {
            fprintf(stderr, "[Config] Warning: malformed line %d (no '=')\n", line_num);
            continue;
        }

        /* Split into key and value */
        *eq = '\0';
        char *key = p;
        char *value = eq + 1;

        cfg_trim_trailing(key);
        value = cfg_trim_leading(value);
        cfg_trim_trailing(value);

        /* Match key */
        unsigned long uval;

        if (strcmp(key, "vin") == 0) {
            if (strlen(value) != 17) {
                fprintf(stderr, "[Config] Warning: VIN must be exactly 17 chars (line %d)\n", line_num);
            } else {
                memcpy(config->server.vin, value, 17);
            }
        }
        else if (strcmp(key, "logical_address") == 0) {
            if (parse_uint(value, &uval, 65535) == 0)
                config->server.logical_address = (uint16_t)uval;
            else
                fprintf(stderr, "[Config] Warning: invalid logical_address (line %d)\n", line_num);
        }
        else if (strcmp(key, "eid") == 0) {
            if (parse_hex_bytes(value, config->server.eid, DOIP_EID_LENGTH) != 0)
                fprintf(stderr, "[Config] Warning: invalid EID format, expected XX:XX:XX:XX:XX:XX (line %d)\n", line_num);
        }
        else if (strcmp(key, "gid") == 0) {
            if (parse_hex_bytes(value, config->server.gid, DOIP_GID_LENGTH) != 0)
                fprintf(stderr, "[Config] Warning: invalid GID format, expected XX:XX:XX:XX:XX:XX (line %d)\n", line_num);
        }
        else if (strcmp(key, "further_action") == 0) {
            if (parse_uint(value, &uval, 255) == 0)
                config->server.further_action = (uint8_t)uval;
            else
                fprintf(stderr, "[Config] Warning: invalid further_action (line %d)\n", line_num);
        }
        else if (strcmp(key, "vin_gid_sync_status") == 0) {
            if (parse_uint(value, &uval, 255) == 0)
                config->server.vin_gid_sync_status = (uint8_t)uval;
            else
                fprintf(stderr, "[Config] Warning: invalid vin_gid_sync_status (line %d)\n", line_num);
        }
        else if (strcmp(key, "bind_address") == 0) {
            strncpy(config->bind_address_buf, value, sizeof(config->bind_address_buf) - 1);
            config->bind_address_buf[sizeof(config->bind_address_buf) - 1] = '\0';
            config->server.bind_address = config->bind_address_buf;
        }
        else if (strcmp(key, "tcp_port") == 0) {
            if (parse_uint(value, &uval, 65535) == 0)
                config->server.tcp_port = (uint16_t)uval;
            else
                fprintf(stderr, "[Config] Warning: invalid tcp_port (line %d)\n", line_num);
        }
        else if (strcmp(key, "udp_port") == 0) {
            if (parse_uint(value, &uval, 65535) == 0)
                config->server.udp_port = (uint16_t)uval;
            else
                fprintf(stderr, "[Config] Warning: invalid udp_port (line %d)\n", line_num);
        }
        else if (strcmp(key, "max_tcp_connections") == 0) {
            if (parse_uint(value, &uval, 255) == 0)
                config->server.max_tcp_connections = (uint8_t)uval;
            else
                fprintf(stderr, "[Config] Warning: invalid max_tcp_connections (line %d)\n", line_num);
        }
        else if (strcmp(key, "max_data_size") == 0) {
            if (parse_uint(value, &uval, UINT32_MAX) == 0)
                config->server.max_data_size = (uint32_t)uval;
            else
                fprintf(stderr, "[Config] Warning: invalid max_data_size (line %d)\n", line_num);
        }
        else if (strcmp(key, "blob_storage_dir") == 0) {
            if (strstr(value, "..") != NULL) {
                fprintf(stderr, "[Config] Warning: blob_storage_dir contains '..' (path traversal rejected, line %d)\n", line_num);
            } else {
                strncpy(config->blob_storage_dir, value, sizeof(config->blob_storage_dir) - 1);
                config->blob_storage_dir[sizeof(config->blob_storage_dir) - 1] = '\0';
            }
        }
        else if (strcmp(key, "blob_max_size") == 0) {
            if (parse_uint(value, &uval, UINT32_MAX) == 0)
                config->blob_max_size = (uint32_t)uval;
            else
                fprintf(stderr, "[Config] Warning: invalid blob_max_size (line %d)\n", line_num);
        }
        else if (strcmp(key, "transfer_timeout") == 0) {
            if (parse_uint(value, &uval, UINT32_MAX) == 0) {
                if (uval == 0) {
                    fprintf(stderr, "[Config] Warning: transfer_timeout=0 rejected (minimum 1), using default (line %d)\n", line_num);
                } else {
                    config->transfer_timeout_sec = (uint32_t)uval;
                }
            } else
                fprintf(stderr, "[Config] Warning: invalid transfer_timeout (line %d)\n", line_num);
        }
        else if (strcmp(key, "phonehome_config") == 0) {
            strncpy(config->phonehome_config_path, value, sizeof(config->phonehome_config_path) - 1);
            config->phonehome_config_path[sizeof(config->phonehome_config_path) - 1] = '\0';
        }
        else if (strcmp(key, "daemon_mode") == 0) {
            config->daemon_mode = (strcmp(value, "1") == 0 || strcmp(value, "true") == 0 ||
                                   strcmp(value, "yes") == 0);
        }
        else if (strcmp(key, "pid_file") == 0) {
            if (strstr(value, "..") != NULL) {
                fprintf(stderr, "[Config] Warning: pid_file contains '..' (path traversal rejected, line %d)\n", line_num);
            } else {
                strncpy(config->pid_file, value, sizeof(config->pid_file) - 1);
                config->pid_file[sizeof(config->pid_file) - 1] = '\0';
            }
        }
        else if (strcmp(key, "script_output_dir") == 0) {
            if (strstr(value, "..") != NULL) {
                fprintf(stderr, "[Config] Warning: script_output_dir contains '..' (path traversal rejected, line %d)\n", line_num);
            } else {
                strncpy(config->script_output_dir, value, sizeof(config->script_output_dir) - 1);
                config->script_output_dir[sizeof(config->script_output_dir) - 1] = '\0';
            }
        }
        else {
            fprintf(stderr, "[Config] Warning: unrecognized key '%s' (line %d)\n", key, line_num);
        }
    }

    fclose(fp);
    return 0;
}

void doip_config_print(const doip_app_config_t *config)
{
    LOG_INFO("Configuration:");
    LOG_INFO("  VIN:               %.17s", (const char *)config->server.vin);
    LOG_INFO("  Logical address:   0x%04X", config->server.logical_address);
    LOG_INFO("  EID:               %02X:%02X:%02X:%02X:%02X:%02X",
             config->server.eid[0], config->server.eid[1], config->server.eid[2],
             config->server.eid[3], config->server.eid[4], config->server.eid[5]);
    LOG_INFO("  GID:               %02X:%02X:%02X:%02X:%02X:%02X",
             config->server.gid[0], config->server.gid[1], config->server.gid[2],
             config->server.gid[3], config->server.gid[4], config->server.gid[5]);
    LOG_INFO("  Further action:    0x%02X", config->server.further_action);
    LOG_INFO("  VIN/GID sync:      0x%02X", config->server.vin_gid_sync_status);
    LOG_INFO("  Bind address:      %s",
             config->server.bind_address ? config->server.bind_address : "(any)");
    LOG_INFO("  TCP port:          %u%s", config->server.tcp_port,
             config->server.tcp_port == 0 ? " (default 13400)" : "");
    LOG_INFO("  UDP port:          %u%s", config->server.udp_port,
             config->server.udp_port == 0 ? " (default 13400)" : "");
    LOG_INFO("  Max TCP conns:     %u", config->server.max_tcp_connections);
    LOG_INFO("  Max data size:     %u", config->server.max_data_size);
    LOG_INFO("  Blob storage:      %s", config->blob_storage_dir);
    LOG_INFO("  Blob max size:     %u bytes (%u MB)",
             config->blob_max_size, config->blob_max_size / (1024 * 1024));
    LOG_INFO("  Transfer timeout:  %u seconds", config->transfer_timeout_sec);
    LOG_INFO("  Phone-home config: %s",
             config->phonehome_config_path[0] ? config->phonehome_config_path : "(disabled)");
    LOG_INFO("  Daemon mode:       %s", config->daemon_mode ? "yes" : "no");
    LOG_INFO("  PID file:          %s", config->pid_file);
    LOG_INFO("  Script output dir: %s", config->script_output_dir);
}
