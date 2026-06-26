/*
 * visibox_linenums.c — Line Number Formatting & Injection (v3)
 *
 * Adds absolute line numbers to output, like a code editor.
 * Numbers are consistent across pages — page 2 starts where page 1 ends.
 * Format: {right-aligned number, width=digit_count(total_lines)} │ {content}
 *
 * Line numbers are injected AFTER output is extracted from buffer,
 * so the raw buffer remains clean for search_jump.
 */

#include "visibox.h"
#include <stdio.h>

/*
 * visibox_linenums_width — Calculate how many digits needed for line numbers.
 * total_lines: total number of lines in the entire output.
 */
int visibox_linenums_width(size_t total_lines) {
    if (total_lines <= 0) return 1;
    if (total_lines >= 1000000) return VISIBOX_LINENUM_MAX_WIDTH; /* cap at 6 */

    int w = 0;
    size_t n = total_lines;
    while (n > 0) { w++; n /= 10; }
    return w > 0 ? w : 1;
}

/*
 * visibox_linenums_inject — Add line numbers to output string.
 *
 * raw_output:  the raw output text (may contain \n)
 * line_start:  1-based number of the FIRST line in this output
 * page_lines:  how many lines are in this output
 * total_lines: total lines in the ENTIRE command output (for width calculation)
 *
 * Returns: newly allocated string with line numbers injected, caller must free.
 */
char *visibox_linenums_inject(const char *raw_output, size_t line_start,
                               size_t page_lines, size_t total_lines) {
    if (!raw_output || !*raw_output)
        return strdup("");

    if (total_lines == 0) total_lines = page_lines;
    if (page_lines == 0) page_lines = 1;

    const char *sep = visibox_config.line_numbers.separator[0]
                        ? visibox_config.line_numbers.separator
                        : VISIBOX_LINENUM_SEP_DEFAULT;
    size_t sep_len = strlen(sep);
    int width = visibox_linenums_width(total_lines);

    /* If width exceeds max, cap it */
    if (width > VISIBOX_LINENUM_MAX_WIDTH)
        width = VISIBOX_LINENUM_MAX_WIDTH;

    /*
     * Calculate output size:
     * For each line: width (digits) + sep_len + content_len
     * Add page_lines * (width + sep_len) to input length as upper bound
     */
    size_t input_len = strlen(raw_output);
    size_t max_extra = page_lines * ((size_t)width + sep_len + 2); /* +2 for \n safety */
    size_t alloc_size = input_len + max_extra + 1;

    char *out = (char *)malloc(alloc_size);
    if (!out) return strdup(raw_output); /* fallback: return raw on OOM */

    char *pos = out;
    size_t line_num = line_start;
    const char *src = raw_output;

    while (*src) {
        /* Write line number: right-aligned, width chars */
        char num_buf[16];
        snprintf(num_buf, sizeof(num_buf), "%zu", line_num);
        int num_len = (int)strlen(num_buf);
        int pad = width - num_len;
        for (int i = 0; i < pad; i++)
            *pos++ = ' ';
        for (int i = 0; i < num_len; i++)
            *pos++ = num_buf[i];

        /* Write separator */
        memcpy(pos, sep, sep_len);
        pos += sep_len;

        /* Copy content until \n or end */
        while (*src && *src != '\n') {
            *pos++ = *src++;
        }

        /* Copy the \n if present */
        if (*src == '\n') {
            *pos++ = '\n';
            src++;
        }

        line_num++;
    }

    *pos = '\0';
    return out;
}