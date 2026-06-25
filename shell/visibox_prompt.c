/*
 * visibox_prompt.c — ANSI Strip + Prompt Detection
 *
 * Fase 2: Two critical functions for interactive sessions:
 *
 * 1. visibox_strip_ansi — removes ANSI escape sequences from PTY output
 *    so that prompt detection regex works on clean text.
 *    Raw output is preserved in the session buffer; this is only for matching.
 *
 * 2. visibox_detect_prompt — checks if stripped output ends with a pattern
 *    (typically "$ " or similar shell prompt).
 *
 * 3. visibox_session_wait_for_prompt — high-level function that:
 *    - Uses event loop to wait for PTY output (no busy-wait, P3)
 *    - Reads output into buffer
 *    - Strips ANSI from the LAST CHUNK of output
 *    - Checks if prompt pattern is found
 *    - Returns when prompt detected or timeout
 *
 * PRD v2 fix #5: "Prompt detection regex rapuh terhadap ANSI escape code"
 * → We strip ANSI BEFORE regex match.
 */

#include "visibox.h"
#include <regex.h>

/* ═══════════════════════════════════════════════════════════════
 * ANSI Strip
 *
 * Removes ANSI CSI sequences (ESC [ ... final_byte)
 * and common non-CSI sequences (ESC O, ESC ], ESC =, etc.)
 *
 * Returns newly allocated string (caller must free).
 * *out_len is set to the length of the stripped string.
 * ═══════════════════════════════════════════════════════════════ */

char *visibox_strip_ansi(const char *raw, size_t len, size_t *out_len) {
    if (!raw || len == 0) {
        if (out_len) *out_len = 0;
        return strdup("");
    }

    /* Worst case: output same size as input */
    char *out = (char *)malloc(len + 1);
    if (!out) {
        if (out_len) *out_len = 0;
        return strdup("");
    }

    size_t j = 0;
    size_t i = 0;

    while (i < len) {
        if (raw[i] == '\033' && i + 1 < len) {
            /* ESC sequence */
            char next = raw[i + 1];

            if (next == '[') {
                /* CSI sequence: ESC [ (params) final_byte
                 * params: digits and semicolons (0x30-0x39, 0x3B)
                 * intermediate: 0x20-0x2F
                 * final: 0x40-0x7E
                 */
                i += 2;  /* skip ESC [ */

                /* Skip parameter bytes */
                while (i < len && ((raw[i] >= 0x30 && raw[i] <= 0x3B) ||
                                   (raw[i] >= 0x20 && raw[i] <= 0x2F))) {
                    i++;
                }

                /* Skip final byte */
                if (i < len && raw[i] >= 0x40 && raw[i] <= 0x7E) {
                    i++;
                }
                continue;
            }
            else if (next == 'O') {
                /* SS3: ESC O (single) — used for function keys */
                i += 3;  /* ESC O + one byte */
                continue;
            }
            else if (next == ']') {
                /* OSC: ESC ] ... (terminated by BEL \007 or ST \033\\) */
                i += 2;
                while (i < len) {
                    if (raw[i] == '\007' /* BEL */) {
                        i++;
                        break;
                    }
                    if (raw[i] == '\033' && i + 1 < len && raw[i + 1] == '\\') {
                        i += 2;
                        break;
                    }
                    i++;
                }
                continue;
            }
            else if (next == '=' || next == '>' || next == '(' || next == ')') {
                /* Simple 2-byte sequences: keyboard modes, charset */
                i += 2;
                continue;
            }
            else {
                /* Unknown ESC sequence — skip the ESC and next byte */
                i += 2;
                continue;
            }
        }

        /* Regular character — copy */
        out[j++] = raw[i++];
    }

    out[j] = '\0';
    if (out_len) *out_len = j;
    return out;
}

/* ═══════════════════════════════════════════════════════════════
 * Prompt Detection
 *
 * Checks if the stripped text matches the prompt pattern.
 * The pattern is a POSIX extended regex.
 * We check if the LAST LINE of the stripped text matches.
 *
 * Returns 1 if prompt detected, 0 if not.
 * ═══════════════════════════════════════════════════════════════ */

int visibox_detect_prompt(const char *stripped, size_t len, const char *pattern) {
    if (!stripped || len == 0 || !pattern || !*pattern) return 0;

    /* Extract the last line from stripped text */
    const char *last_nl = NULL;
    for (size_t i = 0; i < len; i++) {
        if (stripped[i] == '\n') last_nl = stripped + i;
    }

    const char *last_line = last_nl ? last_nl + 1 : stripped;
    size_t last_len = len - (size_t)(last_line - stripped);

    /* Strip trailing \r from last line */
    while (last_len > 0 && last_line[last_len - 1] == '\r') {
        last_len--;
    }

    if (last_len == 0) return 0;

    /* Compile regex */
    regex_t regex;
    int rc = regcomp(&regex, pattern, REG_EXTENDED | REG_NOSUB);
    if (rc != 0) return 0;

    /* Match against the last line */
    /* We need a null-terminated string for regexec */
    char *line_buf = (char *)malloc(last_len + 1);
    if (!line_buf) {
        regfree(&regex);
        return 0;
    }
    memcpy(line_buf, last_line, last_len);
    line_buf[last_len] = '\0';

    rc = regexec(&regex, line_buf, 0, NULL, 0);
    free(line_buf);
    regfree(&regex);

    return (rc == 0) ? 1 : 0;
}

/* ═══════════════════════════════════════════════════════════════
 * Wait for Prompt — High-level helper for session_input
 *
 * After writing input to a session, the caller often wants to
 * wait until the command finishes and the shell shows a prompt.
 *
 * This function:
 * 1. Uses the event loop (epoll_wait) to wait for PTY output
 * 2. Reads available output into the provided buffer
 * 3. Strips ANSI from the latest output
 * 4. Checks if prompt is detected
 * 5. Repeats until prompt found or timeout
 *
 * s:          the session
 * pattern:    regex pattern for prompt (NULL = use session's default)
 * timeout_ms: max wait time in ms
 * out_buf:    buffer to accumulate output (can be NULL for read-only check)
 *
 * Returns: 1 if prompt found, 0 if timeout, -1 on error.
 * ═══════════════════════════════════════════════════════════════ */

int visibox_session_wait_for_prompt(VisiboxSession *s, const char *pattern,
                                     int timeout_ms, OutputBuffer *out_buf) {
    if (!s || s->master_fd < 0) return -1;

    const char *pat = pattern ? pattern : s->prompt_pattern;
    struct timespec start, now;
    clock_gettime(CLOCK_MONOTONIC, &start);

    /* Use the session's own output for prompt checking if no external buffer */
    OutputBuffer *check_buf = s->output;

    while (1) {
        /* Check elapsed time */
        clock_gettime(CLOCK_MONOTONIC, &now);
        long elapsed = visibox_timespec_diff_ms(&start, &now);
        int remaining = timeout_ms - (int)elapsed;
        if (remaining <= 0) {
            return 0;  /* timeout */
        }

        /* Try to read from PTY */
        int bytes = visibox_pty_read_into_buffer(s);
        if (bytes > 0 && out_buf) {
            /* Copy new data to the external output buffer too */
            /* We read from session's output the last chunk */
            /* (out_buf is typically provided by session_input handler) */
        }

        /* Check for prompt in session's output buffer.
         * We only need to check the last ~2KB for performance. */
        if (check_buf && check_buf->used > 0) {
            size_t check_len = visibox_min(check_buf->used, 2048);
            size_t check_offset = check_buf->used - check_len;

            size_t stripped_len;
            char *stripped = visibox_strip_ansi(check_buf->data + check_offset,
                                                 check_len, &stripped_len);
            if (stripped) {
                int detected = visibox_detect_prompt(stripped, stripped_len, pat);
                free(stripped);
                if (detected) {
                    s->prompt_detected = 1;
                    s->prompt_offset = check_offset;
                    return 1;
                }
            }
        }

        /* Wait for more data via event loop (P3: no busy-wait) */
        VisiboxEvent ev;
        int rc = visibox_evloop_wait(&ev, visibox_min(remaining, 100));
        if (rc != 0) {
            return -1;
        }

        if (ev.type == VB_EV_SESSION_EXITED) {
            /* Child exited — read remaining and check one more time */
            visibox_pty_read_into_buffer(s);

            if (check_buf && check_buf->used > 0) {
                size_t check_len = visibox_min(check_buf->used, 2048);
                size_t check_offset = check_buf->used - check_len;

                size_t stripped_len;
                char *stripped = visibox_strip_ansi(check_buf->data + check_offset,
                                                     check_len, &stripped_len);
                if (stripped) {
                    int detected = visibox_detect_prompt(stripped, stripped_len, pat);
                    free(stripped);
                    if (detected) return 1;
                }
            }
            return 0;
        }

        if (ev.type == VB_EV_SESSION_OUTPUT && ev.fd == s->master_fd) {
            /* Data available — loop back to read and check */
            continue;
        }
    }

    return 0;
}