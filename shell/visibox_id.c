/*
 * visibox_id.c — ID Generator & Validation for VisiBox
 *
 * Generates unique IDs for request_id, response_id, session_id, cursor.
 * Format: {prefix}_{8_hex_chars}
 * Example: "req_a1b2c3d4", "res_e5f6a7b8", "sess_1a2b3c4d"
 */

#include "visibox.h"
#include <stdio.h>

/* Monotonic counter for uniqueness */
static unsigned long visibox_id_counter = 0;

/* Simple LFSR-based randomness seeded from clock + counter */
static unsigned long visibox_lfsr_state = 0;

static void init_lfsr(void) {
    if (visibox_lfsr_state == 0) {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        visibox_lfsr_state = (unsigned long)(ts.tv_nsec ^ ts.tv_sec) | 1;
    }
}

static unsigned char next_hex_nibble(void) {
    /* xorshift32 */
    unsigned long x = visibox_lfsr_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    visibox_lfsr_state = x;

    /* Mix in counter to prevent repeats */
    visibox_id_counter++;
    return (unsigned char)("0123456789abcdef"[(x + visibox_id_counter) & 0xf]);
}

/*
 * visibox_generate_id — Generate a unique ID with given prefix.
 * out:    buffer of at least VISIBOX_ID_LEN bytes
 * prefix: e.g. "req", "res", "sess", "cur"
 *
 * Output format: {prefix}_{8_random_hex_chars}\0
 * Total: strlen(prefix) + 1 + 8 + 1 = at most 15+1 = 16 bytes for "req_a1b2c3d4"
 */
void visibox_generate_id(char *out, const char *prefix) {
    init_lfsr();

    int p;
    for (p = 0; prefix[p] && p < 8; p++)
        out[p] = prefix[p];

    out[p++] = '_';

    for (int i = 0; i < 8; i++)
        out[p++] = next_hex_nibble();

    out[p] = '\0';
}

/*
 * visibox_id_valid — Check if an ID string looks valid.
 * Returns 1 if valid, 0 if not.
 */
int visibox_id_valid(const char *id) {
    if (!id || !*id)
        return 0;

    /* Must contain at least one underscore */
    const char *us = strchr(id, '_');
    if (!us)
        return 0;

    /* After underscore, must have at least 4 hex chars */
    const char *hex = us + 1;
    int hex_len = 0;
    while (*hex) {
        if (!isxdigit((unsigned char)*hex))
            return 0;
        hex_len++;
        hex++;
    }

    return hex_len >= 4;
}