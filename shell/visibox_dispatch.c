/*
 * visibox_dispatch.c — Request Routing and Handlers
 *
 * Routes parsed requests to the appropriate handler.
 * Implements execute, fetch_page, and search_jump (Fase 1).
 * Session handlers are stubs for Fase 2.
 */

#include "visibox.h"

/* Forward declaration — bash's parse_and_execute */
extern int parse_and_execute(char *, const char *, int);

/* ═══════════════════════════════════════════════════════════════
 * ERROR HELPER
 * ═══════════════════════════════════════════════════════════════ */

int visibox_make_error(VisiboxRequest *req, VisiboxResponse *res,
                        VisiboxErrorCode code, const char *msg) {
    memset(res, 0, sizeof(VisiboxResponse));
    if (req) {
        strncpy(res->request_id, req->request_id, VISIBOX_ID_LEN - 1);
    }
    visibox_generate_id(res->response_id, "res");
    res->type = VB_RES_ERROR;
    res->error_code = code;
    if (msg)
        strncpy(res->error_message, msg, VISIBOX_MAX_ERROR_MSG - 1);
    return -1;
}

/* ═══════════════════════════════════════════════════════════════
 * EXECUTE HANDLER
 * ═══════════════════════════════════════════════════════════════ */

int visibox_handle_execute(VisiboxRequest *req, VisiboxResponse *res) {
    if (!req->command || !*req->command) {
        return visibox_make_error(req, res, VB_ERR_INVALID_REQUEST,
                                   "Missing 'command' field");
    }

    /* Set response defaults */
    memset(res, 0, sizeof(VisiboxResponse));
    strncpy(res->request_id, req->request_id, VISIBOX_ID_LEN - 1);
    res->type = VB_RES_EXECUTE_RESULT;
    res->has_exit_code = 0;

    /* Override config defaults with request options */
    /* (visibox_execute_and_capture uses global config,
       so we temporarily modify it if needed) */
    size_t saved_page_size = visibox_config.pagination.default_page_size;
    if (req->output_limit > 0)
        visibox_config.pagination.default_page_size = req->output_limit;

    /* Execute with capture — the critical hook (PRD v2 §4.2) */
    int rc = visibox_execute_and_capture(req->command, res);

    /* Restore config */
    visibox_config.pagination.default_page_size = saved_page_size;

    if (rc != 0) {
        /* Error was already set by visibox_execute_and_capture */
        return rc;
    }

    /* Apply line numbers if requested (v3) */
    if (req->line_numbers && res->output && res->output_lines > 0) {
        size_t ps = req->output_limit > 0 ? req->output_limit
                                             : visibox_config.pagination.default_page_size;
        char *numbered = visibox_linenums_inject(res->output, 1, ps, res->output_lines);
        free(res->output);
        res->output = numbered;
        res->line_numbers = 1;
        res->line_start = 1;
        res->line_end = visibox_min(ps, res->output_lines);
    }

    /* Store buffer for fetch_page / search_jump */
    if (res->full_buffer) {
        visibox_store_add(res, res->full_buffer);
        /* Buffer is now owned by the store — don't free it twice */
    }

    /* Build pagination metadata if truncated */
    if (res->output_truncated) {
        visibox_paginator_build_metadata(res->full_buffer, req, res);
        res->has_next = 1;
    }

    return 0;
}

/* ═══════════════════════════════════════════════════════════════
 * FETCH PAGE HANDLER
 * ═══════════════════════════════════════════════════════════════ */

int visibox_handle_fetch_page(VisiboxRequest *req, VisiboxResponse *res) {
    memset(res, 0, sizeof(VisiboxResponse));
    strncpy(res->request_id, req->request_id, VISIBOX_ID_LEN - 1);
    res->type = VB_RES_PAGE_RESULT;

    /* Validate response_id */
    OutputBuffer *buf = visibox_store_get(req->response_id);
    if (!buf) {
        return visibox_make_error(req, res, VB_ERR_RESPONSE_EXPIRED,
                                   "Response data expired or not found");
    }

    /* For now, page is passed via a simple mechanism.
     * TODO: proper cursor-based page tracking */
    int page = 1;  /* default to page 2 (first page was in execute response) */

    /* Parse page from cursor or options */
    if (req->cursor) {
        /* Simple cursor: extract page number from cursor string
         * Cursor format: cur_{hex} where hex encodes page number
         * For now, we just increment. Proper cursor tracking comes later. */
        page = 2;  /* most common case: fetching next page */
    }

    size_t ps = req->output_limit > 0 ? req->output_limit
                                        : visibox_config.pagination.default_page_size;

    res->output = visibox_output_buffer_get_page(buf, page, ps, VB_PAGE_LINES);
    res->output_lines = buf->total_lines;
    res->page = page;

    /* Calculate line range */
    res->line_start = (page - 1) * ps + 1;
    res->line_end = visibox_min((size_t)page * ps, buf->line_count);

    /* Check if there's a next page */
    visibox_output_buffer_build_line_index(buf);
    res->has_next = ((size_t)page * ps < buf->line_count) ? 1 : 0;

    /* Generate cursor for next page if available */
    if (res->has_next)
        visibox_generate_id(res->cursor, "cur");

    /* Apply line numbers if requested (v3) */
    if (req->line_numbers && res->output && buf->line_count > 0) {
        size_t page_lines = res->line_end - res->line_start + 1;
        char *numbered = visibox_linenums_inject(res->output, res->line_start,
                                                   page_lines, buf->line_count);
        free(res->output);
        res->output = numbered;
        res->line_numbers = 1;
    }

    return 0;
}

/* ═══════════════════════════════════════════════════════════════
 * SEARCH JUMP HANDLER (v3)
 * ═══════════════════════════════════════════════════════════════ */

int visibox_handle_search_jump(VisiboxRequest *req, VisiboxResponse *res) {
    memset(res, 0, sizeof(VisiboxResponse));
    strncpy(res->request_id, req->request_id, VISIBOX_ID_LEN - 1);
    res->type = VB_RES_SEARCH_JUMP_RESULT;

    /* Validate keyword */
    if (!req->keyword || !*req->keyword) {
        return visibox_make_error(req, res, VB_ERR_SEARCH_KEYWORD_EMPTY,
                                   "keyword must not be empty");
    }

    /* Validate occurrence */
    if (req->occurrence == 0) {
        return visibox_make_error(req, res, VB_ERR_INVALID_REQUEST,
                                   "occurrence must be >= 1 or -1 (last)");
    }

    /* Get buffer from store */
    OutputBuffer *buf = visibox_store_get(req->response_id);
    if (!buf) {
        return visibox_make_error(req, res, VB_ERR_RESPONSE_EXPIRED,
                                   "Response data expired or not found");
    }

    /* Build line index if needed */
    visibox_output_buffer_build_line_index(buf);

    /* Perform search */
    VisiboxSearchResult sr = visibox_search_keyword(
        buf, req->keyword,
        req->case_sensitive,
        req->occurrence
    );

    /* Populate response */
    if (req->keyword)
        strncpy(res->keyword, req->keyword, VISIBOX_MAX_KEYWORD - 1);
    res->search_occurrence = req->occurrence;
    res->total_occurrences = sr.total_occurrences;
    res->found = sr.found;
    res->found_line = sr.found_line;

    if (sr.found) {
        /* Extract context around the found line */
        size_t out_limit = req->output_limit > 0 ? req->output_limit
                                                   : visibox_config.pagination.default_page_size;
        res->output = visibox_search_get_context(
            buf, sr.found_line,
            req->context_lines,
            req->line_numbers,
            out_limit,
            &res->line_start,
            &res->line_end
        );
        res->line_numbers = req->line_numbers;

        /* Calculate page_hint */
        size_t ps = visibox_config.pagination.default_page_size;
        size_t page_hint = ((sr.found_line - 1) / ps) + 1;
        (void)page_hint;  /* included in serialization */

        /* Generate cursor to that page */
        visibox_generate_id(res->cursor, "cur");
    } else {
        res->output = strdup("");
    }

    return 0;
}

/* ═══════════════════════════════════════════════════════════════
 * MAIN DISPATCH
 * ═══════════════════════════════════════════════════════════════ */

int visibox_dispatch_request(VisiboxRequest *req, VisiboxResponse *res) {
    switch (req->type) {
        case VB_REQ_EXECUTE:
            return visibox_handle_execute(req, res);

        case VB_REQ_FETCH_PAGE:
            return visibox_handle_fetch_page(req, res);

        case VB_REQ_SEARCH_JUMP:
            return visibox_handle_search_jump(req, res);

        /* Fase 2 stubs */
        case VB_REQ_SESSION_START:
            return visibox_make_error(req, res, VB_ERR_EXEC_FAILED,
                                       "session_start not yet implemented (Fase 2)");

        case VB_REQ_SESSION_INPUT:
            return visibox_make_error(req, res, VB_ERR_SESSION_NOT_FOUND,
                                       "session_input not yet implemented (Fase 2)");

        case VB_REQ_SESSION_READ:
            return visibox_make_error(req, res, VB_ERR_SESSION_NOT_FOUND,
                                       "session_read not yet implemented (Fase 2)");

        case VB_REQ_SESSION_LIST:
            return visibox_make_error(req, res, VB_ERR_EXEC_FAILED,
                                       "session_list not yet implemented (Fase 2)");

        case VB_REQ_SESSION_CLOSE:
            return visibox_make_error(req, res, VB_ERR_SESSION_NOT_FOUND,
                                       "session_close not yet implemented (Fase 2)");

        case VB_REQ_SESSION_FETCH_PAGE:
            return visibox_make_error(req, res, VB_ERR_EXEC_FAILED,
                                       "session_fetch_page not yet implemented (Fase 2)");

        case VB_REQ_UNKNOWN:
            return visibox_make_error(req, res, VB_ERR_INVALID_REQUEST,
                                       "Unknown request type");
    }

    return visibox_make_error(req, res, VB_ERR_INTERNAL, "Dispatch fell through");
}