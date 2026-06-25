/*
 * visibox_core.c — VisiBox Entry Point, Mode Detection, and Execute Hook
 *
 * This is the heart of VisiBox: it hooks into bash's execution path
 * to capture output via fd redirect (dup2), without forking a new shell.
 * (PRD v2 §4.2 — the critical design that makes `cd`/`export` persist.)
 *
 * Two modes:
 *   PIPE MODE:  echo '{"type":"execute","command":"ls"}' | visibox
 *               Read one JSON request from stdin, execute, write JSON response to stdout.
 *   REPL MODE:  (Fase 3) Interactive JSON REPL on unix socket.
 *   DAEMON MODE: (Fase 3) Background daemon serving multiple clients.
 */

#include "visibox.h"

/* ═══════════════════════════════════════════════════════════════
 * GLOBAL STATE
 * ═══════════════════════════════════════════════════════════════ */

VisiboxConfig visibox_config;
ResponseStore visibox_store;

int visibox_active = 0;
int visibox_capturing = 0;

/* ═══════════════════════════════════════════════════════════════
 * UTILITY
 * ═══════════════════════════════════════════════════════════════ */

size_t visibox_min(size_t a, size_t b) { return a < b ? a : b; }
size_t visibox_max(size_t a, size_t b) { return a > b ? a : b; }

long visibox_timespec_diff_ms(struct timespec *start, struct timespec *end) {
    return (end->tv_sec - start->tv_sec) * 1000L +
           (end->tv_nsec - start->tv_nsec) / 1000000L;
}

/* ═══════════════════════════════════════════════════════════════
 * CONFIG DEFAULTS
 * ═══════════════════════════════════════════════════════════════ */

void visibox_init_config(void) {
    memset(&visibox_config, 0, sizeof(VisiboxConfig));

    /* Pagination */
    visibox_config.pagination.mode = VB_PAGE_LINES;
    visibox_config.pagination.default_page_size = VISIBOX_DEFAULT_PAGE_SIZE;
    visibox_config.pagination.max_page_size = VISIBOX_MAX_PAGE_SIZE;
    visibox_config.pagination.min_page_size = VISIBOX_MIN_PAGE_SIZE;
    visibox_config.pagination.include_total = 1;
    visibox_config.pagination.cursor_expiry_ms = VISIBOX_DEFAULT_CURSOR_EXPIRY;

    /* Line numbers (v3) */
    visibox_config.line_numbers.enabled = 0;
    strncpy(visibox_config.line_numbers.separator, VISIBOX_LINENUM_SEP_DEFAULT,
            sizeof(visibox_config.line_numbers.separator) - 1);
    visibox_config.line_numbers.max_width = VISIBOX_LINENUM_MAX_WIDTH;

    /* Search (v3) */
    visibox_config.search.default_context_lines = VISIBOX_SEARCH_DEFAULT_CONTEXT;
    visibox_config.search.max_context_lines = VISIBOX_SEARCH_MAX_CONTEXT;
    visibox_config.search.max_keyword_length = VISIBOX_MAX_KEYWORD;
    visibox_config.search.case_sensitive_default = 0;

    /* Response store */
    visibox_config.store.max_entries = VISIBOX_STORE_MAX_ENTRIES;
    visibox_config.store.max_total_bytes = VISIBOX_STORE_MAX_BYTES;

    /* Sessions */
    visibox_config.sessions.max_concurrent = VISIBOX_MAX_SESSIONS;
    visibox_config.sessions.default_read_timeout_ms = 5000;
    visibox_config.sessions.default_initial_read_timeout_ms = 5000;
    visibox_config.sessions.idle_timeout_ms = 1800000;
    visibox_config.sessions.max_output_buffer_bytes_per_session = 10 * 1024 * 1024;

    /* Execute */
    visibox_config.execute.default_timeout_ms = VISIBOX_DEFAULT_TIMEOUT_MS;
    visibox_config.execute.max_timeout_ms = VISIBOX_MAX_TIMEOUT_MS;
}

/* ═══════════════════════════════════════════════════════════════
 * MODE DETECTION
 * ═══════════════════════════════════════════════════════════════ */

void visibox_detect_mode(int argc, char **argv, VisiboxMode *mode) {
    *mode = VB_MODE_PIPE;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--repl") == 0 || strcmp(argv[i], "-r") == 0) {
            *mode = VB_MODE_REPL;
            return;
        }
        if (strcmp(argv[i], "--daemon") == 0 || strcmp(argv[i], "-d") == 0) {
            *mode = VB_MODE_DAEMON;
            return;
        }
        if (strcmp(argv[i], "--pipe") == 0 || strcmp(argv[i], "-p") == 0) {
            *mode = VB_MODE_PIPE;
            return;
        }
    }

    /* Auto-detect: if stdin is a pipe (not a tty), use PIPE mode */
    if (!isatty(STDIN_FILENO)) {
        *mode = VB_MODE_PIPE;
    } else {
        *mode = VB_MODE_PIPE; /* default for now, REPL in Fase 3 */
    }
}

/* ═══════════════════════════════════════════════════════════════
 * CORE EXECUTE — THE CRITICAL HOOK (PRD v2 §4.2)
 *
 * This function:
 * 1. Creates a pipe
 * 2. Saves current stdout/stderr fd
 * 3. Redirects stdout/stderr to the pipe write-end
 * 4. Calls bash's parse_and_execute() — the REAL execution path
 *    - Builtin commands (cd, export, etc.) run IN THIS PROCESS → state persists
 *    - External commands: bash itself forks (native behavior) → we don't add a fork
 * 5. Restores stdout/stderr
 * 6. Drains the pipe (with limit, per P2)
 * 7. Returns the captured output in VisiboxResponse
 *
 * THIS IS WHY `cd` PERSISTS — we never fork the shell.
 * ═══════════════════════════════════════════════════════════════ */

int visibox_execute_and_capture(const char *command, VisiboxResponse *res) {
    if (!command || !res) return -1;

    visibox_generate_id(res->response_id, "res_");

    /* Create pipe for output capture */
    int pipefd[2];
    if (pipe(pipefd) < 0) {
        visibox_make_error(NULL, res, VB_ERR_INTERNAL,
                           "pipe() failed for output capture");
        return -1;
    }

    /* Set pipe read-end to non-blocking BEFORE execution.
     * This prevents deadlock when command output exceeds
     * kernel pipe buffer size (~64KB) and command blocks on write. */
    fcntl(pipefd[0], F_SETFL, O_NONBLOCK);

    /* Save original stdout and stderr */
    int saved_stdout = dup(STDOUT_FILENO);
    int saved_stderr = dup(STDERR_FILENO);
    if (saved_stdout < 0 || saved_stderr < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        visibox_make_error(NULL, res, VB_ERR_INTERNAL,
                           "dup() failed for fd save");
        return -1;
    }

    /* Redirect stdout and stderr to pipe write-end */
    dup2(pipefd[1], STDOUT_FILENO);
    dup2(pipefd[1], STDERR_FILENO);
    close(pipefd[1]);  /* close original write-end, we now write via STDOUT/STDERR */

    /* Flush any buffered C library output before redirect */
    fflush(stdout);
    fflush(stderr);

    /* ─── EXECUTE VIA BASH'S REAL PATH ─── */
    visibox_capturing = 1;

    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);

    /*
     * parse_and_execute() is bash's own function that:
     * 1. Parses the command string into a COMMAND struct
     * 2. Calls execute_command_internal() which dispatches to:
     *    - execute_builtin_or_function() for builtins (NO fork — state persists)
     *    - execute_disk_command() for externals (bash forks, as normal)
     *
     * This is declared in builtins/evalstring.c
     */
/* SEVAL flags from builtins/common.h — we can't include that header
   from visibox_core.c because it depends on bash-internal types */
#ifndef SEVAL_NONINT
#define SEVAL_NONINT    0x001
#endif
#ifndef SEVAL_NOHIST
#define SEVAL_NOHIST    0x004
#endif

    /* parse_and_execute is declared in externs.h, defined in builtins/evalstring.c */
    extern int parse_and_execute (char *, const char *, int);
    int exit_code = parse_and_execute((char *)command, "visibox",
                                       SEVAL_NOHIST | SEVAL_NONINT);

    clock_gettime(CLOCK_MONOTONIC, &end);

    visibox_capturing = 0;

    /* Flush stdout again — builtin output may be buffered */
    fflush(stdout);
    fflush(stderr);

    /* ─── RESTORE ORIGINAL FDs ─── */
    dup2(saved_stdout, STDOUT_FILENO);
    dup2(saved_stderr, STDERR_FILENO);
    close(saved_stdout);
    close(saved_stderr);

    /* ─── DRAIN PIPE ─── */
    OutputBuffer *buf = visibox_output_buffer_new();

    /* Determine page size and mode from defaults for now
     * (request-specific options are applied by visibox_handle_execute) */
    size_t page_size = visibox_config.pagination.default_page_size;
    PaginationMode mode = visibox_config.pagination.mode;

    visibox_drain_pipe_with_limit(pipefd[0], buf, page_size, mode);
    close(pipefd[0]);

    /* Build line index for search_jump support (v3) */
    visibox_output_buffer_build_line_index(buf);

    /* ─── POPULATE RESPONSE ─── */
    res->exit_code = exit_code;
    res->has_exit_code = 1;
    res->duration_ms = (size_t)visibox_timespec_diff_ms(&start, &end);
    res->output_lines = buf->total_lines;
    res->output_bytes = buf->total_bytes;
    res->output_truncated = buf->truncated;

    /* Get first page of output */
    res->output = visibox_output_buffer_get_page(buf, 1, page_size, mode);

    /* Store buffer for fetch_page / search_jump */
    res->full_buffer = buf;
    /* Note: visibox_store_add will be called by the dispatcher */

    return 0;
}

/* ═══════════════════════════════════════════════════════════════
 * PIPE MODE — Single request from stdin, response to stdout
 * ═══════════════════════════════════════════════════════════════ */

int visibox_pipe_mode(void) {
    /* Read all of stdin into a buffer */
    size_t capacity = VISIBOX_READ_BUF_SIZE;
    size_t len = 0;
    char *json_buf = (char *)malloc(capacity);
    if (!json_buf) {
        fprintf(stderr, "visibox: out of memory\n");
        return 1;
    }

    ssize_t n;
    while ((n = read(STDIN_FILENO, json_buf + len, capacity - len - 1)) > 0) {
        len += (size_t)n;
        if (len + 1 >= capacity) {
            capacity *= 2;
            char *tmp = (char *)realloc(json_buf, capacity);
            if (!tmp) {
                fprintf(stderr, "visibox: out of memory\n");
                free(json_buf);
                return 1;
            }
            json_buf = tmp;
        }
    }
    json_buf[len] = '\0';

    if (len == 0) {
        /* Empty input — exit silently (could be a test) */
        free(json_buf);
        return 0;
    }

    /* Parse request */
    VisiboxRequest req;
    memset(&req, 0, sizeof(VisiboxRequest));
    VisiboxResponse res;
    memset(&res, 0, sizeof(VisiboxResponse));

    if (visibox_parse_request(json_buf, &req) != 0) {
        free(json_buf);
        visibox_make_error(&req, &res, VB_ERR_INVALID_REQUEST,
                           "Failed to parse JSON request");
        char *resp_json = visibox_serialize_response(&res);
        if (resp_json) {
            printf("%s\n", resp_json);
            free(resp_json);
        }
        return 1;
    }

    free(json_buf);

    /* Dispatch */
    int rc = visibox_dispatch_request(&req, &res);

    /* Serialize and print response */
    char *resp_json = visibox_serialize_response(&res);
    if (resp_json) {
        printf("%s\n", resp_json);
        free(resp_json);
    }

    /* Cleanup */
    if (res.output) free(res.output);
    /* Note: res.full_buffer is owned by the store after dispatch */

    return (rc == 0) ? 0 : 1;
}

/* ═══════════════════════════════════════════════════════════════
 * MAIN ENTRY — Called from modified shell.c
 * ═══════════════════════════════════════════════════════════════ */

int visibox_main(int argc, char **argv) {
    VisiboxMode mode;

    /* Initialize */
    visibox_init_config();
    visibox_store_init();
    visibox_active = 1;

    /* Detect mode */
    visibox_detect_mode(argc, argv, &mode);

    switch (mode) {
        case VB_MODE_PIPE:
            return visibox_pipe_mode();

        case VB_MODE_REPL:
            /* Fase 3 */
            fprintf(stderr, "visibox: REPL mode not yet implemented (Fase 3)\n");
            return 1;

        case VB_MODE_DAEMON:
            /* Fase 3 */
            fprintf(stderr, "visibox: Daemon mode not yet implemented (Fase 3)\n");
            return 1;
    }

    return 1;
}