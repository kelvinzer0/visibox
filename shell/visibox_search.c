/*
 * visibox_search.c — Keyword Search Engine on Output Buffer (v3)
 *
 * Performs substring search across the entire output buffer,
 * returns the line number of the Nth occurrence, and provides
 * context extraction for search_jump responses.
 *
 * P7: Search is server-side, AI only receives the relevant page.
 */

#include "visibox.h"
#include <strings.h>  /* strcasestr() */

/* ═══════════════════════════════════════════════════════════════
 * visibox_search_keyword — Search for keyword in output buffer.
 *
 * buf:            the output buffer (must have line_index_built)
 * keyword:        the search string (plain substring, not regex)
 * case_sensitive: 0 = case-insensitive, 1 = case-sensitive
 * occurrence:     1-based. Which match to return? -1 = last match.
 *
 * Returns: SearchResult with found_line (1-based) and total_occurrences.
 */
VisiboxSearchResult visibox_search_keyword(const OutputBuffer *buf,
                                             const char *keyword,
                                             int case_sensitive,
                                             int occurrence) {
    VisiboxSearchResult result = {0};

    if (!buf || !keyword || !*keyword || !buf->lines || buf->line_count == 0)
        return result;

    size_t kw_len = strlen(keyword);
    size_t total = 0;
    size_t found_at = 0;  /* 0 = not found */
    int looking_for_last = (occurrence < 0);

    if (looking_for_last)
        occurrence = INT_MAX;

    for (size_t i = 0; i < buf->line_count; i++) {
        LineEntry *le = &buf->lines[i];
        if (le->length == 0) continue;

        /* Get the line content (strip trailing \n for matching) */
        size_t line_len = le->length;
        int has_newline = 0;
        if (line_len > 0 && buf->data[le->offset + line_len - 1] == '\n') {
            line_len--;
            has_newline = 1;
        }

        /* Substring match */
        int match = 0;
        if (case_sensitive) {
            match = (memmem(buf->data + le->offset, line_len, keyword, kw_len) != NULL);
        } else {
            /* Manual case-insensitive search (strcasestr works on strings,
               we need to work on a potentially non-null-terminated buffer) */
            if (kw_len <= line_len) {
                for (size_t j = 0; j <= line_len - kw_len; j++) {
                    int ok = 1;
                    for (size_t k = 0; k < kw_len && ok; k++) {
                        unsigned char a = (unsigned char)buf->data[le->offset + j + k];
                        unsigned char b = (unsigned char)keyword[k];
                        if (tolower(a) != tolower(b))
                            ok = 0;
                    }
                    if (ok) { match = 1; break; }
                }
            }
        }

        if (match) {
            total++;
            if (total == (size_t)occurrence) {
                found_at = i + 1;  /* 1-based */
                if (looking_for_last) {
                    /* Keep going to find the last one */
                }
            }
            if (looking_for_last) {
                found_at = i + 1;  /* update to latest */
            }
        }
    }

    result.total_occurrences = total;
    result.found = (found_at > 0) ? 1 : 0;
    result.found_line = found_at;
    return result;
}

/* ═══════════════════════════════════════════════════════════════
 * visibox_search_get_context — Extract context lines around a found line.
 *
 * buf:          output buffer
 * found_line:   1-based absolute line number where keyword was found
 * context_lines: how many lines before/after to include
 * line_numbers: whether to inject line numbers
 * output_limit: max lines to return
 * out_line_start, out_line_end: set to the actual range returned (1-based)
 *
 * Returns: newly allocated string with the context, caller must free.
 */
char *visibox_search_get_context(const OutputBuffer *buf,
                                   size_t found_line,
                                   int context_lines,
                                   int line_numbers,
                                   size_t output_limit,
                                   size_t *out_line_start,
                                   size_t *out_line_end) {
    if (!buf || !buf->lines || found_line == 0) {
        *out_line_start = 0;
        *out_line_end = 0;
        return strdup("");
    }

    size_t total = buf->line_count;

    /* Calculate range (1-based) */
    size_t start = (found_line > (size_t)context_lines)
                       ? found_line - (size_t)context_lines
                       : 1;
    size_t end = found_line + (size_t)context_lines;
    if (end > total) end = total;

    /* Apply output_limit */
    if (output_limit > 0 && (end - start + 1) > output_limit) {
        /* Center on found_line as much as possible */
        size_t half = output_limit / 2;
        size_t s = (found_line > half) ? found_line - half : 1;
        size_t e = s + output_limit - 1;
        if (e > total) e = total;
        if (e - s + 1 < output_limit)
            s = (e >= output_limit) ? e - output_limit + 1 : 1;
        start = s;
        end = e;
    }

    *out_line_start = start;
    *out_line_end = end;

    /* Extract raw lines */
    char *raw = visibox_output_buffer_get_line_range((OutputBuffer *)buf, start, end);

    if (line_numbers && raw) {
        /* Inject line numbers */
        size_t page_lines = end - start + 1;
        char *numbered = visibox_linenums_inject(raw, start, page_lines, total);
        free(raw);
        return numbered;
    }

    return raw ? raw : strdup("");
}