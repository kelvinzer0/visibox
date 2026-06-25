/*
 * visibox_dispatch.c — Request Routing and Handlers
 *
 * Routes parsed requests to the appropriate handler.
 * Fase 1: execute, fetch_page, search_jump
 * Fase 2: session_start, session_input, session_read, session_list,
 *         session_close, session_fetch_page
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
 * EXECUTE HANDLER (Fase 1 — unchanged)
 * ═══════════════════════════════════════════════════════════════ */

int visibox_handle_execute(VisiboxRequest *req, VisiboxResponse *res) {
    if (!req->command || !*req->command) {
        return visibox_make_error(req, res, VB_ERR_INVALID_REQUEST,
                                   "Missing 'command' field");
    }

    memset(res, 0, sizeof(VisiboxResponse));
    strncpy(res->request_id, req->request_id, VISIBOX_ID_LEN - 1);
    res->type = VB_RES_EXECUTE_RESULT;
    res->has_exit_code = 0;

    size_t saved_page_size = visibox_config.pagination.default_page_size;
    if (req->output_limit > 0)
        visibox_config.pagination.default_page_size = req->output_limit;

    int rc = visibox_execute_and_capture(req->command, res);

    visibox_config.pagination.default_page_size = saved_page_size;

    if (rc != 0) return rc;

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

    if (res->full_buffer) {
        visibox_store_add(res, res->full_buffer);
    }

    if (res->output_truncated) {
        visibox_paginator_build_metadata(res->full_buffer, req, res);
        res->has_next = 1;
    }

    return 0;
}

/* ═══════════════════════════════════════════════════════════════
 * FETCH PAGE HANDLER (Fase 1 — unchanged)
 * ═══════════════════════════════════════════════════════════════ */

int visibox_handle_fetch_page(VisiboxRequest *req, VisiboxResponse *res) {
    memset(res, 0, sizeof(VisiboxResponse));
    strncpy(res->request_id, req->request_id, VISIBOX_ID_LEN - 1);
    res->type = VB_RES_PAGE_RESULT;

    OutputBuffer *buf = visibox_store_get(req->response_id);
    if (!buf) {
        return visibox_make_error(req, res, VB_ERR_RESPONSE_EXPIRED,
                                   "Response data expired or not found");
    }

    int page = 1;
    if (req->cursor) {
        page = 2;
    }

    size_t ps = req->output_limit > 0 ? req->output_limit
                                        : visibox_config.pagination.default_page_size;

    res->output = visibox_output_buffer_get_page(buf, page, ps, VB_PAGE_LINES);
    res->output_lines = buf->total_lines;
    res->page = page;

    res->line_start = (page - 1) * ps + 1;
    res->line_end = visibox_min((size_t)page * ps, buf->line_count);

    visibox_output_buffer_build_line_index(buf);
    res->has_next = ((size_t)page * ps < buf->line_count) ? 1 : 0;

    if (res->has_next)
        visibox_generate_id(res->cursor, "cur");

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
 * SEARCH JUMP HANDLER (Fase 1 — unchanged)
 * ═══════════════════════════════════════════════════════════════ */

int visibox_handle_search_jump(VisiboxRequest *req, VisiboxResponse *res) {
    memset(res, 0, sizeof(VisiboxResponse));
    strncpy(res->request_id, req->request_id, VISIBOX_ID_LEN - 1);
    res->type = VB_RES_SEARCH_JUMP_RESULT;

    if (!req->keyword || !*req->keyword) {
        return visibox_make_error(req, res, VB_ERR_SEARCH_KEYWORD_EMPTY,
                                   "keyword must not be empty");
    }
    if (req->occurrence == 0) {
        return visibox_make_error(req, res, VB_ERR_INVALID_REQUEST,
                                   "occurrence must be >= 1 or -1 (last)");
    }

    OutputBuffer *buf = visibox_store_get(req->response_id);
    if (!buf) {
        return visibox_make_error(req, res, VB_ERR_RESPONSE_EXPIRED,
                                   "Response data expired or not found");
    }

    visibox_output_buffer_build_line_index(buf);

    VisiboxSearchResult sr = visibox_search_keyword(
        buf, req->keyword, req->case_sensitive, req->occurrence);

    if (req->keyword)
        strncpy(res->keyword, req->keyword, VISIBOX_MAX_KEYWORD - 1);
    res->search_occurrence = req->occurrence;
    res->total_occurrences = sr.total_occurrences;
    res->found = sr.found;
    res->found_line = sr.found_line;

    if (sr.found) {
        size_t out_limit = req->output_limit > 0 ? req->output_limit
                                                   : visibox_config.pagination.default_page_size;
        res->output = visibox_search_get_context(
            buf, sr.found_line, req->context_lines, req->line_numbers,
            out_limit, &res->line_start, &res->line_end);
        res->line_numbers = req->line_numbers;
        visibox_generate_id(res->cursor, "cur");
    } else {
        res->output = strdup("");
    }

    return 0;
}

/* ═══════════════════════════════════════════════════════════════
 * SESSION START HANDLER (Fase 2)
 *
 * Creates a new PTY session with the given command.
 * - Validates command
 * - Checks session limit
 * - Creates session (allocates PTY, forks child)
 * - Waits briefly for initial output (banner, first prompt)
 * - Returns session_id
 * ═══════════════════════════════════════════════════════════════ */

int visibox_handle_session_start(VisiboxRequest *req, VisiboxResponse *res) {
    memset(res, 0, sizeof(VisiboxResponse));
    strncpy(res->request_id, req->request_id, VISIBOX_ID_LEN - 1);
    res->type = VB_RES_SESSION_START_RESULT;

    if (!req->command || !*req->command) {
        return visibox_make_error(req, res, VB_ERR_INVALID_REQUEST,
                                   "Missing 'command' field for session_start");
    }

    /* Check session limit (P4) */
    if ((size_t)visibox_session_count() >= visibox_config.sessions.max_concurrent) {
        return visibox_make_error(req, res, VB_ERR_SESSION_LIMIT_REACHED,
                                   "Maximum concurrent sessions reached");
    }

    /* Create the session */
    VisiboxSession *s = visibox_session_create(req->command);
    if (!s) {
        return visibox_make_error(req, res, VB_ERR_EXEC_FAILED,
                                   "Failed to allocate PTY or spawn process");
    }

    /* Start idle sweeper on first session */
    if (!visibox_sessions.sweeper_running) {
        visibox_sessions.sweeper_running = 1;
        pthread_create(&visibox_sessions.sweeper_thread, NULL,
                       visibox_session_sweeper_thread, NULL);
        pthread_detach(visibox_sessions.sweeper_thread);
    }

    /* Copy session_id to response */
    strncpy(res->session_id, s->session_id, VISIBOX_ID_LEN - 1);
    visibox_generate_id(res->response_id, "res");

    /* Wait for initial output using event loop (P3) */
    int initial_timeout = (int)visibox_config.sessions.default_initial_read_timeout_ms;
    struct timespec start, now;
    clock_gettime(CLOCK_MONOTONIC, &start);

    while (1) {
        clock_gettime(CLOCK_MONOTONIC, &now);
        long elapsed = visibox_timespec_diff_ms(&start, &now);
        int remaining = initial_timeout - (int)elapsed;
        if (remaining <= 0) break;

        /* Try to read from PTY */
        visibox_pty_read_into_buffer(s);

        /* Check if we have enough output (or child already exited) */
        if (s->output->used > 0 || s->child_exited) break;

        /* Wait for data via event loop */
        VisiboxEvent ev;
        int rc = visibox_evloop_wait(&ev, visibox_min((size_t)remaining, 100));
        if (rc != 0) break;
        if (ev.type == VB_EV_SESSION_EXITED) break;
        if (ev.type == VB_EV_SESSION_OUTPUT && ev.fd == s->master_fd) continue;
        break;
    }

    /* Build initial output for response */
    size_t ps = req->output_limit > 0 ? req->output_limit
                                        : visibox_config.pagination.default_page_size;

    /* Build line index for the output we've collected so far */
    visibox_output_buffer_build_line_index(s->output);

    /* Get first page of session output */
    res->output = visibox_output_buffer_get_page(s->output, 1, ps, VB_PAGE_LINES);
    res->output_lines = s->output->total_lines;
    res->output_bytes = s->output->total_bytes;
    res->output_truncated = s->output->truncated;

    /* Calculate uptime */
    clock_gettime(CLOCK_MONOTONIC, &now);
    s->uptime_ms = (size_t)visibox_timespec_diff_ms(&s->created_at, &now);

    /* Apply line numbers if requested */
    if (req->line_numbers && res->output && s->output->line_count > 0) {
        size_t page_lines = visibox_min(ps, s->output->line_count);
        char *numbered = visibox_linenums_inject(res->output, 1, page_lines,
                                                   s->output->line_count);
        free(res->output);
        res->output = numbered;
        res->line_numbers = 1;
        res->line_start = 1;
        res->line_end = page_lines;
    }

    /* Store session output in response store for fetch_page/search_jump */
    /* We store a snapshot — session output continues to grow */
    /* For session_fetch_page, we use the session's live buffer */

    if (res->output_truncated) {
        visibox_paginator_build_metadata(s->output, req, res);
        res->has_next = 1;
    }

    return 0;
}

/* ═══════════════════════════════════════════════════════════════
 * SESSION INPUT HANDLER (Fase 2)
 *
 * Sends input to a running session, optionally waits for prompt.
 * - Finds session by session_id
 * - Writes input to PTY master_fd
 * - If wait_for_prompt: uses event loop to wait for output + prompt
 * - Returns output since last read
 * ═══════════════════════════════════════════════════════════════ */

int visibox_handle_session_input(VisiboxRequest *req, VisiboxResponse *res) {
    memset(res, 0, sizeof(VisiboxResponse));
    strncpy(res->request_id, req->request_id, VISIBOX_ID_LEN - 1);
    res->type = VB_RES_SESSION_INPUT_RESULT;

    if (!req->session_id[0]) {
        return visibox_make_error(req, res, VB_ERR_INVALID_REQUEST,
                                   "Missing 'session_id' for session_input");
    }

    VisiboxSession *s = visibox_session_find(req->session_id);
    if (!s) {
        return visibox_make_error(req, res, VB_ERR_SESSION_NOT_FOUND,
                                   "Session not found or already closed");
    }

    if (!req->input || !*req->input) {
        return visibox_make_error(req, res, VB_ERR_INVALID_REQUEST,
                                   "Missing 'input' field for session_input");
    }

    /* Record output position BEFORE writing input */
    size_t output_before = s->output->used;
    size_t lines_before = s->output->total_lines;

    /* Write input to PTY */
    size_t input_len = strlen(req->input);
    if (visibox_pty_write(s, req->input, input_len) < 0) {
        return visibox_make_error(req, res, VB_ERR_EXEC_FAILED,
                                   "Failed to write to session PTY");
    }

    /* Wait for output and optional prompt */
    int timeout = (int)visibox_config.sessions.default_read_timeout_ms;
    if (req->timeout_ms > 0) timeout = (int)req->timeout_ms;

    if (req->wait_for_prompt || req->prompt_pattern) {
        /* Wait for prompt using event loop */
        const char *pattern = req->prompt_pattern ? req->prompt_pattern
                                                   : s->prompt_pattern;
        int found = visibox_session_wait_for_prompt(s, pattern, timeout, NULL);
        (void)found;
    } else {
        /* Just wait for some output (not prompt-specific) */
        struct timespec start, now;
        clock_gettime(CLOCK_MONOTONIC, &start);

        /* Read output with timeout using event loop (P3) */
        while (1) {
            clock_gettime(CLOCK_MONOTONIC, &now);
            long elapsed = visibox_timespec_diff_ms(&start, &now);
            int remaining = timeout - (int)elapsed;
            if (remaining <= 0) break;

            int bytes = visibox_pty_read_into_buffer(s);
            if (bytes > 0) {
                /* Got some output — read a bit more with short timeout */
                /* to capture trailing output */
                VisiboxEvent ev;
                visibox_evloop_wait(&ev, 50);  /* 50ms grace period */
                visibox_pty_read_into_buffer(s);
                break;
            }

            if (s->child_exited) break;

            VisiboxEvent ev;
            int rc = visibox_evloop_wait(&ev, visibox_min((size_t)remaining, 100));
            if (rc != 0) break;
            if (ev.type == VB_EV_SESSION_EXITED) {
                visibox_pty_read_into_buffer(s);
                break;
            }
            if (ev.type == VB_EV_SESSION_OUTPUT && ev.fd == s->master_fd) continue;
            break;
        }
    }

    /* Extract NEW output (since before we wrote input) */
    size_t new_bytes = s->output->used - output_before;
    char *new_output = NULL;
    if (new_bytes > 0) {
        new_output = (char *)malloc(new_bytes + 1);
        if (new_output) {
            memcpy(new_output, s->output->data + output_before, new_bytes);
            new_output[new_bytes] = '\0';
        }
    }

    /* Build response */
    visibox_generate_id(res->response_id, "res");
    strncpy(res->session_id, s->session_id, VISIBOX_ID_LEN - 1);

    /* Calculate output for response (paginated) */
    size_t ps = req->output_limit > 0 ? req->output_limit
                                        : visibox_config.pagination.default_page_size;

    /* For session_input, we return the NEW output since the input was sent */
    if (new_output && new_bytes > 0) {
        /* Build a temporary buffer for the new output */
        OutputBuffer *new_buf = visibox_output_buffer_new();
        visibox_output_buffer_append(new_buf, new_output, new_bytes);
        visibox_output_buffer_build_line_index(new_buf);

        res->output = visibox_output_buffer_get_page(new_buf, 1, ps, VB_PAGE_LINES);
        res->output_lines = new_buf->total_lines;
        res->output_bytes = new_buf->total_bytes;
        res->output_truncated = new_buf->truncated;

        if (req->line_numbers && res->output && new_buf->line_count > 0) {
            size_t page_lines = visibox_min(ps, new_buf->line_count);
            /* Line numbers are relative to session output, not this chunk */
            char *numbered = visibox_linenums_inject(res->output, lines_before + 1,
                                                       page_lines, new_buf->line_count + lines_before);
            free(res->output);
            res->output = numbered;
            res->line_numbers = 1;
            res->line_start = lines_before + 1;
            res->line_end = lines_before + page_lines;
        }

        visibox_output_buffer_free(new_buf);
    } else {
        res->output = strdup("");
        res->output_lines = 0;
        res->output_bytes = 0;
    }

    free(new_output);

    /* Prompt detection result */
    res->prompt_detected = s->prompt_detected;
    /* Reset prompt flag for next input */
    s->prompt_detected = 0;

    /* Check if child exited */
    if (s->child_exited) {
        res->exit_code = s->child_exit_status;
        res->has_exit_code = 1;
    }

    return 0;
}

/* ═══════════════════════════════════════════════════════════════
 * SESSION READ HANDLER (Fase 2)
 *
 * Reads new output from a session without sending input.
 * Useful for long-running processes (tail -f, server output).
 * ═══════════════════════════════════════════════════════════════ */

int visibox_handle_session_read(VisiboxRequest *req, VisiboxResponse *res) {
    memset(res, 0, sizeof(VisiboxResponse));
    strncpy(res->request_id, req->request_id, VISIBOX_ID_LEN - 1);
    res->type = VB_RES_SESSION_READ_RESULT;

    if (!req->session_id[0]) {
        return visibox_make_error(req, res, VB_ERR_INVALID_REQUEST,
                                   "Missing 'session_id' for session_read");
    }

    VisiboxSession *s = visibox_session_find(req->session_id);
    if (!s) {
        return visibox_make_error(req, res, VB_ERR_SESSION_NOT_FOUND,
                                   "Session not found or already closed");
    }

    /* Record position before reading */
    size_t output_before = s->output->used;
    size_t lines_before = s->output->total_lines;

    /* Read available output */
    visibox_pty_read_into_buffer(s);

    /* If no new data, wait briefly using event loop */
    if (s->output->used == output_before && !s->child_exited) {
        int timeout = (int)visibox_config.sessions.default_read_timeout_ms;
        if (req->timeout_ms > 0) timeout = (int)req->timeout_ms;

        VisiboxEvent ev;
        int rc = visibox_evloop_wait(&ev, timeout);
        if (rc == 0 && ev.type == VB_EV_SESSION_OUTPUT && ev.fd == s->master_fd) {
            visibox_pty_read_into_buffer(s);
        } else if (rc == 0 && ev.type == VB_EV_SESSION_EXITED) {
            visibox_pty_read_into_buffer(s);
        }
    }

    /* Extract new output */
    size_t new_bytes = s->output->used - output_before;
    visibox_generate_id(res->response_id, "res");
    strncpy(res->session_id, s->session_id, VISIBOX_ID_LEN - 1);

    size_t ps = req->output_limit > 0 ? req->output_limit
                                        : visibox_config.pagination.default_page_size;

    if (new_bytes > 0) {
        char *new_output = (char *)malloc(new_bytes + 1);
        if (new_output) {
            memcpy(new_output, s->output->data + output_before, new_bytes);
            new_output[new_bytes] = '\0';

            OutputBuffer *new_buf = visibox_output_buffer_new();
            visibox_output_buffer_append(new_buf, new_output, new_bytes);
            visibox_output_buffer_build_line_index(new_buf);

            res->output = visibox_output_buffer_get_page(new_buf, 1, ps, VB_PAGE_LINES);
            res->output_lines = new_buf->total_lines;
            res->output_bytes = new_bytes;
            res->output_truncated = new_buf->truncated;

            if (req->line_numbers && res->output && new_buf->line_count > 0) {
                size_t page_lines = visibox_min(ps, new_buf->line_count);
                char *numbered = visibox_linenums_inject(res->output, lines_before + 1,
                                                           page_lines, new_buf->line_count + lines_before);
                free(res->output);
                res->output = numbered;
                res->line_numbers = 1;
                res->line_start = lines_before + 1;
                res->line_end = lines_before + page_lines;
            }

            visibox_output_buffer_free(new_buf);
            free(new_output);
        }
    } else {
        res->output = strdup("");
        res->output_lines = 0;
        res->output_bytes = 0;
    }

    if (s->child_exited) {
        res->exit_code = s->child_exit_status;
        res->has_exit_code = 1;
    }

    return 0;
}

/* ═══════════════════════════════════════════════════════════════
 * SESSION LIST HANDLER (Fase 2)
 *
 * Returns info about all active sessions.
 * AI uses this to see what sessions exist and decide which to close.
 * ═══════════════════════════════════════════════════════════════ */

int visibox_handle_session_list(VisiboxRequest *req, VisiboxResponse *res) {
    memset(res, 0, sizeof(VisiboxResponse));
    strncpy(res->request_id, req->request_id, VISIBOX_ID_LEN - 1);
    res->type = VB_RES_SESSION_LIST_RESULT;
    visibox_generate_id(res->response_id, "res");

    /* Build a JSON array of sessions */
    pthread_mutex_lock(&visibox_sessions.lock);

    /* Calculate total output size */
    size_t cap = 4096;
    char *out = (char *)malloc(cap);
    if (!out) {
        pthread_mutex_unlock(&visibox_sessions.lock);
        return visibox_make_error(req, res, VB_ERR_INTERNAL, "Out of memory");
    }

    size_t pos = 0;
    out[pos++] = '[';

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    int first = 1;

    for (int i = 0; i < VISIBOX_MAX_SESSIONS; i++) {
        VisiboxSession *s = &visibox_sessions.sessions[i];
        if (s->state != VB_SESSION_RUNNING && s->state != VB_SESSION_CLOSING)
            continue;

        /* Ensure capacity */
        if (pos + 512 > cap) {
            cap *= 2;
            char *tmp = (char *)realloc(out, cap);
            if (!tmp) { free(out); pthread_mutex_unlock(&visibox_sessions.lock);
                         return visibox_make_error(req, res, VB_ERR_INTERNAL, "OOM"); }
            out = tmp;
        }

        if (!first) out[pos++] = ',';
        first = 0;

        size_t uptime = (size_t)visibox_timespec_diff_ms(&s->created_at, &now);
        size_t idle = (size_t)visibox_timespec_diff_ms(&s->last_activity, &now);

        pos += snprintf(out + pos, cap - pos,
            "{\"session_id\":\"%s\","
            "\"command\":\"%s\","
            "\"state\":\"%s\","
            "\"child_pid\":%d,"
            "\"child_exited\":%s,"
            "\"uptime_ms\":%zu,"
            "\"idle_ms\":%zu,"
            "\"output_lines\":%zu,"
            "\"output_bytes\":%zu}",
            s->session_id,
            s->command ? s->command : "",
            s->state == VB_SESSION_RUNNING ? "running" : "closing",
            (int)s->child_pid,
            s->child_exited ? "true" : "false",
            uptime,
            idle,
            s->output ? s->output->total_lines : 0,
            s->output ? s->output->used : 0
        );
    }

    out[pos++] = ']';
    out[pos] = '\0';

    pthread_mutex_unlock(&visibox_sessions.lock);

    res->output = out;
    res->output_lines = visibox_session_count();
    return 0;
}

/* ═══════════════════════════════════════════════════════════════
 * SESSION CLOSE HANDLER (Fase 2)
 *
 * Closes a session. Per PRD v2 §6.6:
 * If session already closed, return already_closed: true (not error).
 * ═══════════════════════════════════════════════════════════════ */

int visibox_handle_session_close(VisiboxRequest *req, VisiboxResponse *res) {
    memset(res, 0, sizeof(VisiboxResponse));
    strncpy(res->request_id, req->request_id, VISIBOX_ID_LEN - 1);
    visibox_generate_id(res->response_id, "res");

    if (!req->session_id[0]) {
        res->type = VB_RES_SESSION_CLOSE_RESULT;
        return visibox_make_error(req, res, VB_ERR_INVALID_REQUEST,
                                   "Missing 'session_id' for session_close");
    }

    /* Read any final output before closing */
    VisiboxSession *s = visibox_session_find(req->session_id);

    if (!s) {
        /* Already closed or never existed */
        res->type = VB_RES_SESSION_CLOSE_RESULT;
        res->already_closed = 1;
        res->output = strdup("");
        strncpy(res->session_id, req->session_id, VISIBOX_ID_LEN - 1);
        return 0;
    }

    /* Drain any remaining output */
    visibox_pty_read_into_buffer(s);

    /* Get final output snapshot */
    size_t final_lines = s->output ? s->output->total_lines : 0;
    size_t final_bytes = s->output ? s->output->used : 0;

    char *final_output = NULL;
    if (s->output && s->output->used > 0) {
        size_t ps = req->output_limit > 0 ? req->output_limit
                                            : visibox_config.pagination.default_page_size;
        final_output = visibox_output_buffer_get_page(s->output, 1, ps, VB_PAGE_LINES);
    } else {
        final_output = strdup("");
    }

    /* Destroy the session */
    int rc = visibox_session_destroy(req->session_id);

    res->type = VB_RES_SESSION_CLOSE_RESULT;
    strncpy(res->session_id, req->session_id, VISIBOX_ID_LEN - 1);
    res->output = final_output;
    res->output_lines = final_lines;
    res->output_bytes = final_bytes;

    if (rc == 1) {
        res->already_closed = 1;
    }

    if (rc == 0) {
        res->exit_code = s ? s->child_exit_status : 0;
        res->has_exit_code = 1;
    }

    return 0;
}

/* ═══════════════════════════════════════════════════════════════
 * SESSION FETCH PAGE HANDLER (Fase 2)
 *
 * Paginates a session's accumulated output.
 * Similar to fetch_page but reads from the live session buffer
 * instead of the response store.
 * ═══════════════════════════════════════════════════════════════ */

int visibox_handle_session_fetch_page(VisiboxRequest *req, VisiboxResponse *res) {
    memset(res, 0, sizeof(VisiboxResponse));
    strncpy(res->request_id, req->request_id, VISIBOX_ID_LEN - 1);
    res->type = VB_RES_PAGE_RESULT;

    if (!req->session_id[0]) {
        return visibox_make_error(req, res, VB_ERR_INVALID_REQUEST,
                                   "Missing 'session_id'");
    }

    VisiboxSession *s = visibox_session_find(req->session_id);
    if (!s) {
        return visibox_make_error(req, res, VB_ERR_SESSION_NOT_FOUND,
                                   "Session not found or already closed");
    }

    /* Read any pending output first */
    visibox_pty_read_into_buffer(s);
    visibox_output_buffer_build_line_index(s->output);

    /* Parse page number */
    int page = 1;
    /* Use cursor to encode page: cursor "cur_page_N" */
    if (req->cursor && strncmp(req->cursor, "cur_page_", 9) == 0) {
        page = atoi(req->cursor + 9);
        if (page < 1) page = 1;
    }

    size_t ps = req->output_limit > 0 ? req->output_limit
                                        : visibox_config.pagination.default_page_size;

    res->output = visibox_output_buffer_get_page(s->output, page, ps, VB_PAGE_LINES);
    res->output_lines = s->output->total_lines;
    res->page = page;

    res->line_start = (page - 1) * ps + 1;
    res->line_end = visibox_min((size_t)page * ps, s->output->line_count);

    res->has_next = ((size_t)page * ps < s->output->line_count) ? 1 : 0;

    if (res->has_next) {
        /* Encode page number in cursor */
        snprintf(res->cursor, VISIBOX_ID_LEN, "cur_page_%d", page + 1);
    }

    /* Apply line numbers if requested */
    if (req->line_numbers && res->output && s->output->line_count > 0) {
        size_t page_lines = res->line_end - res->line_start + 1;
        char *numbered = visibox_linenums_inject(res->output, res->line_start,
                                                   page_lines, s->output->line_count);
        free(res->output);
        res->output = numbered;
        res->line_numbers = 1;
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

        /* Fase 2: session handlers */
        case VB_REQ_SESSION_START:
            return visibox_handle_session_start(req, res);

        case VB_REQ_SESSION_INPUT:
            return visibox_handle_session_input(req, res);

        case VB_REQ_SESSION_READ:
            return visibox_handle_session_read(req, res);

        case VB_REQ_SESSION_LIST:
            return visibox_handle_session_list(req, res);

        case VB_REQ_SESSION_CLOSE:
            return visibox_handle_session_close(req, res);

        case VB_REQ_SESSION_FETCH_PAGE:
            return visibox_handle_session_fetch_page(req, res);

        case VB_REQ_UNKNOWN:
            return visibox_make_error(req, res, VB_ERR_INVALID_REQUEST,
                                       "Unknown request type");
    }

    return visibox_make_error(req, res, VB_ERR_INTERNAL, "Dispatch fell through");
}