/*
 * visibox_paginator.c — Output Buffer, Pagination, and Cursor Encoding
 *
 * Manages raw output buffering, line counting, page extraction,
 * and cursor generation for the pagination system.
 */

#include "visibox.h"
#include <stdio.h>

/* ═══════════════════════════════════════════════════════════════
 * Output Buffer Lifecycle
 * ═══════════════════════════════════════════════════════════════ */

OutputBuffer *visibox_output_buffer_new(void) {
    OutputBuffer *buf = (OutputBuffer *)calloc(1, sizeof(OutputBuffer));
    if (!buf) return NULL;

    buf->data = (char *)malloc(VISIBOX_BUFFER_INITIAL_SIZE);
    if (!buf->data) {
        free(buf);
        return NULL;
    }
    buf->capacity = VISIBOX_BUFFER_INITIAL_SIZE;
    buf->used = 0;
    buf->total_lines = 0;
    buf->total_bytes = 0;
    buf->truncated = 0;
    buf->lines = NULL;
    buf->line_count = 0;
    buf->line_index_built = 0;
    buf->count_lines = 0;
    buf->count_bytes = 0;

    return buf;
}

void visibox_output_buffer_free(OutputBuffer *buf) {
    if (!buf) return;
    free(buf->data);
    free(buf->lines);
    free(buf);
}

/* ═══════════════════════════════════════════════════════════════
 * Append data to buffer, counting lines
 * ═══════════════════════════════════════════════════════════════ */

void visibox_output_buffer_append(OutputBuffer *buf, const char *data, size_t len) {
    if (!buf || !data || len == 0) return;

    /* Ensure capacity */
    while (buf->used + len > buf->capacity) {
        size_t new_cap = buf->capacity * 2;
        char *new_data = (char *)realloc(buf->data, new_cap);
        if (!new_data) return; /* silently fail on OOM — don't crash */
        buf->data = new_data;
        buf->capacity = new_cap;
    }

    /* Copy data */
    memcpy(buf->data + buf->used, data, len);
    buf->used += len;
    buf->total_bytes += len;

    /* Count newlines */
    for (size_t i = 0; i < len; i++) {
        if (data[i] == '\n')
            buf->total_lines++;
    }
}

/* Count-only mode: after truncation, count lines/bytes but don't store */
void visibox_output_buffer_count_only(OutputBuffer *buf, const char *data, size_t len) {
    if (!buf || !data || len == 0) return;
    buf->count_bytes += len;
    for (size_t i = 0; i < len; i++) {
        if (data[i] == '\n')
            buf->count_lines++;
    }
}

/* ═══════════════════════════════════════════════════════════════
 * Build Line Index (v3)
 * After all data is drained, build an array of {offset, length}
 * for O(1) line access by search_jump and fetch_page.
 * ═══════════════════════════════════════════════════════════════ */

void visibox_output_buffer_build_line_index(OutputBuffer *buf) {
    if (!buf || buf->line_index_built) return;

    /* If the last character is not \n, the final "line" still counts */
    size_t lines = buf->total_lines;
    if (buf->used > 0 && buf->data[buf->used - 1] != '\n')
        lines++;

    if (lines == 0) {
        buf->line_index_built = 1;
        return;
    }

    buf->lines = (LineEntry *)calloc(lines, sizeof(LineEntry));
    if (!buf->lines) return;

    size_t line_idx = 0;
    size_t line_start = 0;

    for (size_t i = 0; i < buf->used && line_idx < lines; i++) {
        if (buf->data[i] == '\n') {
            buf->lines[line_idx].offset = line_start;
            buf->lines[line_idx].length = i - line_start + 1; /* include \n */
            line_idx++;
            line_start = i + 1;
        }
    }

    /* Handle last line without trailing \n */
    if (line_idx < lines && line_start < buf->used) {
        buf->lines[line_idx].offset = line_start;
        buf->lines[line_idx].length = buf->used - line_start;
        line_idx++;
    }

    buf->line_count = line_idx;
    buf->line_index_built = 1;
}

/* ═══════════════════════════════════════════════════════════════
 * Get a page of lines from the buffer
 * page: 1-based page number
 * Returns a newly allocated string, caller must free.
 * ═══════════════════════════════════════════════════════════════ */

char *visibox_output_buffer_get_page(OutputBuffer *buf, int page,
                                       size_t page_size, PaginationMode mode) {
    if (!buf || !buf->data || buf->used == 0)
        return strdup("");

    if (mode == VB_PAGE_BYTES) {
        /* Byte-based pagination */
        size_t start = (page - 1) * page_size;
        size_t end = visibox_min(start + page_size, buf->used);
        if (start >= buf->used) return strdup("");
        size_t len = end - start;
        char *result = (char *)malloc(len + 1);
        if (!result) return strdup("");
        memcpy(result, buf->data + start, len);
        result[len] = '\0';
        return result;
    }

    /* Line-based pagination (default) */
    visibox_output_buffer_build_line_index(buf);
    if (!buf->lines || buf->line_count == 0)
        return strdup("");

    size_t total = buf->line_count;
    size_t start_line = (page - 1) * page_size;
    size_t end_line = visibox_min(start_line + page_size, total);

    if (start_line >= total) return strdup("");

    return visibox_output_buffer_get_line_range(buf, start_line + 1, end_line);
}

/* Get a range of lines (1-based, inclusive) */
char *visibox_output_buffer_get_line_range(OutputBuffer *buf,
                                             size_t start, size_t end) {
    visibox_output_buffer_build_line_index(buf);
    if (!buf->lines || buf->line_count == 0)
        return strdup("");

    /* Convert to 0-based */
    size_t s = visibox_max(start, 1) - 1;
    size_t e = visibox_min(end, buf->line_count);

    if (s >= buf->line_count) return strdup("");
    if (s >= e) return strdup("");

    /* Calculate total length */
    size_t total_len = 0;
    for (size_t i = s; i < e; i++) {
        total_len += buf->lines[i].length;
    }

    char *result = (char *)malloc(total_len + 1);
    if (!result) return strdup("");

    size_t pos = 0;
    for (size_t i = s; i < e; i++) {
        memcpy(result + pos, buf->data + buf->lines[i].offset, buf->lines[i].length);
        pos += buf->lines[i].length;
    }
    result[pos] = '\0';

    return result;
}

/* ═══════════════════════════════════════════════════════════════
 * Drain pipe with limit (P2: protects tokens, not compute)
 * ═══════════════════════════════════════════════════════════════ */

void visibox_drain_pipe_with_limit(int fd, OutputBuffer *buf,
                                    size_t limit, PaginationMode mode) {
    char chunk[VISIBOX_BUFFER_CHUNK_SIZE];
    ssize_t n;
    int limit_hit = 0;

    while (1) {
        n = read(fd, chunk, sizeof(chunk));
        if (n > 0) {
            if (!limit_hit) {
                visibox_output_buffer_append(buf, chunk, (size_t)n);
                if (limit > 0 &&
                    ((mode == VB_PAGE_LINES && buf->total_lines >= limit) ||
                     (mode == VB_PAGE_BYTES && buf->used >= limit))) {
                    buf->truncated = 1;
                    limit_hit = 1;
                }
            }
            if (limit_hit) {
                /* Keep counting for accurate metadata */
                visibox_output_buffer_count_only(buf, chunk, (size_t)n);
            }
        } else if (n == 0) {
            break;  /* EOF */
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                struct pollfd pfd = { .fd = fd, .events = POLLIN };
                poll(&pfd, 1, -1);
                continue;
            }
            break;  /* real error */
        }
    }

    /* Finalize total_lines with count-only data */
    buf->total_lines += buf->count_lines;
    buf->total_bytes += buf->count_bytes;
}

/* ═══════════════════════════════════════════════════════════════
 * Build pagination metadata
 * ═══════════════════════════════════════════════════════════════ */

void visibox_paginator_build_metadata(OutputBuffer *buf,
                                       VisiboxRequest *req, VisiboxResponse *res) {
    if (!buf || !req || !res) return;

    size_t page_size = req->output_limit > 0 ? req->output_limit
                                               : visibox_config.pagination.default_page_size;
    PaginationMode mode = req->output_unit;

    /* Build line index for accurate total */
    visibox_output_buffer_build_line_index(buf);

    /* Generate cursor for page 2 */
    visibox_generate_id(res->cursor, "cur");
}