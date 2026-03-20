/**
 * @file cli.c
 * @brief DoIP Server interactive CLI (read-only commands)
 *
 * Commands: status, config, transfer, generate-scripts, help, quit/exit
 * All read-only — no runtime config mutation.
 */

#define _POSIX_C_SOURCE 200809L

#include "cli.h"
#include "script_gen.h"
#include "phonehome_handler.h"
#include "doip_log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <signal.h>
#include <sys/stat.h>

#ifndef DOIP_SERVER_VERSION
#define DOIP_SERVER_VERSION "unknown"
#endif

/* ============================================================================
 * Helpers
 * ========================================================================== */

static char *trim(char *s)
{
    while (*s && isspace((unsigned char)*s)) s++;
    size_t len = strlen(s);
    while (len > 0 && isspace((unsigned char)s[len - 1]))
        s[--len] = '\0';
    return s;
}

static void format_uptime(time_t seconds, char *buf, size_t buf_size)
{
    int days = (int)(seconds / 86400);
    int hours = (int)((seconds % 86400) / 3600);
    int mins = (int)((seconds % 3600) / 60);
    int secs = (int)(seconds % 60);

    if (days > 0)
        snprintf(buf, buf_size, "%dd %dh %dm %ds", days, hours, mins, secs);
    else if (hours > 0)
        snprintf(buf, buf_size, "%dh %dm %ds", hours, mins, secs);
    else if (mins > 0)
        snprintf(buf, buf_size, "%dm %ds", mins, secs);
    else
        snprintf(buf, buf_size, "%ds", secs);
}

/* ============================================================================
 * Command Handlers
 * ========================================================================== */

static void cmd_status(const cli_context_t *ctx)
{
    time_t now = time(NULL);
    time_t uptime = now - ctx->server_start_time;
    char uptime_str[64];
    format_uptime(uptime, uptime_str, sizeof(uptime_str));

    printf("  Server:     running\n");
    printf("  Uptime:     %s\n", uptime_str);
    printf("  Platform:   %s\n",
#ifdef PLATFORM_TORIZON
           "Torizon"
#else
           "Ubuntu"
#endif
    );
    printf("  Version:    %s\n", DOIP_SERVER_VERSION);
    printf("  Clients:    %d / %d\n",
           ctx->client_count,
           ctx->server->config.max_tcp_connections);
    printf("  Bind:       %s:%u (TCP+UDP)\n",
           ctx->config->server.bind_address ? ctx->config->server.bind_address : "0.0.0.0",
           ctx->config->server.tcp_port);
    printf("  Storage:    %s\n", ctx->config->blob_storage_dir);

    /* Transfer status */
    printf("  Transfer:   %s\n", ctx->transfer_active ? "active" : "none");
    if (ctx->transfer_active) {
        printf("    Progress: %u / %u bytes (block %u)\n",
               ctx->transfer_bytes_received,
               ctx->transfer_memory_size,
               ctx->transfer_block_sequence);
        time_t idle = now - ctx->transfer_last_activity;
        printf("    Idle:     %ld seconds\n", (long)idle);
    }

    /* Phone-home subsystem status */
    phonehome_status_t ph;
    phonehome_get_status(&ph);

    printf("\n  Phone-Home:\n");
    printf("    Enabled:    %s\n", ph.enabled ? "yes" : "no");
    if (ph.enabled) {
        printf("    HMAC:       %s\n", ph.hmac_loaded ? "loaded" : "not loaded");
        printf("    Bastion:    %s:%u\n", ph.bastion_host, ph.bastion_port ? ph.bastion_port : 22);
        printf("    SSH User:   %s\n", ph.ssh_user[0] ? ph.ssh_user : "(not set)");
        printf("    Client Key: %s\n", ph.client_key_installed ? "installed" : "not configured");
        printf("    Script:     %s\n", ph.connect_script);
        printf("    Lock File:  %s\n", ph.lock_file);

        /* Check if tunnel is currently active */
        struct stat lock_st;
        if (stat(ph.lock_file, &lock_st) == 0) {
            FILE *lf = fopen(ph.lock_file, "r");
            if (lf) {
                int pid = 0;
                if (fscanf(lf, "%d", &pid) == 1 && pid > 0) {
                    if (kill(pid, 0) == 0) {
                        printf("    Tunnel:     ACTIVE (PID %d)\n", pid);
                    } else {
                        printf("    Tunnel:     stale lock (PID %d dead)\n", pid);
                    }
                }
                fclose(lf);
            }
        } else {
            printf("    Tunnel:     inactive\n");
        }

        /* Check key files */
        struct stat kst;
        printf("    HMAC File:  %s\n",
               stat("/etc/phonehome/hmac_secret", &kst) == 0 ? "present" : "missing");
        printf("    SSH Key:    %s\n",
               stat("/etc/phonehome/id_ed25519", &kst) == 0 ? "present" : "missing");
        printf("    Known Hosts:%s\n",
               stat("/etc/phonehome/known_hosts", &kst) == 0 ? " present" : " missing");
        printf("    DCU Serial: ");
        FILE *sf = fopen("/etc/dcu-serial", "r");
        if (sf) {
            char sn[32] = {0};
            if (fgets(sn, sizeof(sn), sf)) {
                /* Strip newline */
                size_t sl = strlen(sn);
                while (sl > 0 && (sn[sl-1] == '\n' || sn[sl-1] == '\r'))
                    sn[--sl] = '\0';
                printf("%s\n", sn);
            } else {
                printf("(empty)\n");
            }
            fclose(sf);
        } else {
            printf("missing (/etc/dcu-serial)\n");
        }
    }
}

static void cmd_config(const cli_context_t *ctx)
{
    const doip_app_config_t *c = ctx->config;

    printf("  VIN:               %.17s\n", (const char *)c->server.vin);
    printf("  Logical address:   0x%04X\n", c->server.logical_address);
    printf("  EID:               %02X:%02X:%02X:%02X:%02X:%02X\n",
           c->server.eid[0], c->server.eid[1], c->server.eid[2],
           c->server.eid[3], c->server.eid[4], c->server.eid[5]);
    printf("  GID:               %02X:%02X:%02X:%02X:%02X:%02X\n",
           c->server.gid[0], c->server.gid[1], c->server.gid[2],
           c->server.gid[3], c->server.gid[4], c->server.gid[5]);
    printf("  Bind address:      %s\n",
           c->server.bind_address ? c->server.bind_address : "(any)");
    printf("  TCP port:          %u\n", c->server.tcp_port);
    printf("  UDP port:          %u\n", c->server.udp_port);
    printf("  Max TCP conns:     %u\n", c->server.max_tcp_connections);
    printf("  Max data size:     %u\n", c->server.max_data_size);
    printf("  Blob storage:      %s\n", c->blob_storage_dir);
    printf("  Blob max size:     %u bytes (%u MB)\n",
           c->blob_max_size, c->blob_max_size / (1024 * 1024));
    printf("  Transfer timeout:  %u seconds\n", c->transfer_timeout_sec);
    printf("  Phone-home config: %s\n",
           c->phonehome_config_path[0] ? c->phonehome_config_path : "(disabled)");
    printf("  PID file:          %s\n", c->pid_file);
    printf("  Script output dir: %s\n", c->script_output_dir);
}

static void cmd_transfer(const cli_context_t *ctx)
{
    if (!ctx->transfer_active) {
        printf("  No active transfer.\n");
        return;
    }

    time_t now = time(NULL);
    time_t idle = now - ctx->transfer_last_activity;
    uint32_t pct = 0;
    if (ctx->transfer_memory_size > 0)
        pct = (ctx->transfer_bytes_received * 100) / ctx->transfer_memory_size;

    printf("  Transfer active:\n");
    printf("    Bytes:    %u / %u (%u%%)\n",
           ctx->transfer_bytes_received, ctx->transfer_memory_size, pct);
    printf("    Block:    %u (next expected)\n", ctx->transfer_block_sequence);
    printf("    Idle:     %ld seconds\n", (long)idle);
    printf("    Timeout:  %u seconds\n", ctx->config->transfer_timeout_sec);
}

static void cmd_generate_scripts(const cli_context_t *ctx, const char *arg)
{
    const char *dir;
    if (arg && arg[0] != '\0') {
        dir = arg;
    } else {
        dir = ctx->config->script_output_dir;
    }

    printf("  Writing scripts to: %s\n", dir);
    int n = script_gen_write_to(dir);
    if (n < 0) {
        printf("  Error: could not write scripts.\n");
    } else {
        printf("  %d of %d scripts written.\n", n, script_gen_count());
    }
}

static void cmd_help(void)
{
    printf("  Available commands:\n");
    printf("    status              Show server status and uptime\n");
    printf("    config              Display current configuration\n");
    printf("    transfer            Show active transfer progress\n");
    printf("    generate-scripts [dir]  Write operational scripts\n");
    printf("    help                Show this help\n");
    printf("    quit / exit         Shut down the server\n");
}

/* ============================================================================
 * Public API
 * ========================================================================== */

void cli_print_prompt(void)
{
    printf("doip> ");
    fflush(stdout);
}

int cli_process_input(cli_context_t *ctx)
{
    char line[256];
    if (fgets(line, sizeof(line), stdin) == NULL) {
        /* EOF — treat as quit */
        printf("\n");
        return 1;
    }

    char *cmd = trim(line);
    if (cmd[0] == '\0')
        return 0;  /* Empty line */

    if (strcmp(cmd, "status") == 0) {
        cmd_status(ctx);
    }
    else if (strcmp(cmd, "config") == 0) {
        cmd_config(ctx);
    }
    else if (strcmp(cmd, "transfer") == 0) {
        cmd_transfer(ctx);
    }
    else if (strncmp(cmd, "generate-scripts", 16) == 0) {
        const char *arg = cmd + 16;
        while (*arg && isspace((unsigned char)*arg)) arg++;
        cmd_generate_scripts(ctx, arg);
    }
    else if (strcmp(cmd, "help") == 0 || strcmp(cmd, "?") == 0) {
        cmd_help();
    }
    else if (strcmp(cmd, "quit") == 0 || strcmp(cmd, "exit") == 0) {
        printf("  Shutting down...\n");
        *ctx->running = 0;
        return 1;
    }
    else {
        printf("  Unknown command: %s (type 'help' for commands)\n", cmd);
    }

    return 0;
}
