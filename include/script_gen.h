/**
 * @file script_gen.h
 * @brief Self-generation of operational scripts from embedded templates
 *
 * The DoIP server embeds its operational scripts as C string literals,
 * selected at compile time based on PLATFORM_TORIZON or PLATFORM_UBUNTU.
 * This ensures scripts always match the running server version and platform.
 */

#ifndef SCRIPT_GEN_H
#define SCRIPT_GEN_H

#include "config.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Write all embedded scripts to the output directory
 * @param config   App config (uses script_output_dir)
 * @return Number of scripts successfully written, negative on fatal error
 *
 * Creates output directory if it doesn't exist.
 * Writes each script with O_NOFOLLOW and fchmod().
 * Reports per-file errors via LOG_WARN, continues on individual failures.
 */
int script_gen_write_all(const doip_app_config_t *config);

/**
 * @brief Write all embedded scripts to a specified directory (overrides config)
 * @param output_dir  Directory to write scripts to
 * @return Number of scripts successfully written, negative on fatal error
 */
int script_gen_write_to(const char *output_dir);

/**
 * @brief Get the number of embedded scripts available
 */
int script_gen_count(void);

#ifdef __cplusplus
}
#endif

#endif /* SCRIPT_GEN_H */
