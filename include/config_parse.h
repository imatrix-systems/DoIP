/**
 * @file config_parse.h
 * @brief Shared KEY=VALUE config parsing helpers
 */

#ifndef CONFIG_PARSE_H
#define CONFIG_PARSE_H

#include <string.h>
#include <ctype.h>

/** Trim leading whitespace (returns pointer into same buffer) */
static inline char *cfg_trim_leading(char *s)
{
    while (*s && isspace((unsigned char)*s)) s++;
    return s;
}

/** Trim trailing whitespace in-place */
static inline void cfg_trim_trailing(char *s)
{
    size_t len = strlen(s);
    if (len == 0) return;
    char *end = s + len - 1;
    while (end >= s && isspace((unsigned char)*end))
        *end-- = '\0';
}

#endif /* CONFIG_PARSE_H */
