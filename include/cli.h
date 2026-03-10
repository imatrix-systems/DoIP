/**
 * @file cli.h
 * @brief DoIP Server interactive CLI (read-only commands)
 *
 * Provides status, config display, transfer progress, and script generation
 * commands. All commands are read-only — config changes require editing the
 * config file and restarting.
 *
 * CLI is only active when stdin is a TTY (isatty). Non-TTY mode (backgrounded
 * by test harness, piped, etc.) uses the original sleep loop.
 */

#ifndef DOIP_CLI_H
#define DOIP_CLI_H

#include "config.h"
#include "doip_server.h"
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Context passed to CLI from main loop
 *
 * Transfer fields are snapshots taken under g_transfer_mutex by the caller.
 * Server and config pointers are read-only.
 */
typedef struct {
    const doip_app_config_t *config;
    const doip_server_t     *server;
    volatile sig_atomic_t   *running;
    time_t                   server_start_time;

    /* Snapshot fields (updated by caller under respective mutexes) */
    int      client_count;              /* Snapshot of server->num_clients */
    bool     transfer_active;
    uint32_t transfer_bytes_received;
    uint32_t transfer_memory_size;
    uint8_t  transfer_block_sequence;
    time_t   transfer_last_activity;
} cli_context_t;

/**
 * @brief Process one line of CLI input
 * @param ctx  CLI context with current state snapshots
 * @return 0 to continue, 1 if quit/exit requested
 *
 * Reads one line from stdin (assumes select() has indicated readiness).
 * Parses and dispatches the command. EOF on stdin is treated as quit.
 */
int cli_process_input(cli_context_t *ctx);

/**
 * @brief Print the CLI prompt
 */
void cli_print_prompt(void);

#ifdef __cplusplus
}
#endif

#endif /* DOIP_CLI_H */
