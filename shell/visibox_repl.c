/*
 * visibox_repl.c — Interactive REPL Mode (Fase 3)
 *
 * Reads JSON requests from stdin (one per line, like a REPL),
 * dispatches them, and prints the JSON response to stdout.
 *
 * Unlike pipe mode (which reads ONE request and exits),
 * REPL mode loops forever, processing one request per line.
 *
 * This is useful for:
 *   - Interactive testing/debugging
 *   - Piping multiple requests:  seq of JSON lines | visibox --repl
 *   - Connecting via:  visibox --repl  (then type JSON manually)
 *
 * The event loop is NOT the main loop here (same as pipe mode).
 * Session PTY output is polled on-demand by session handlers.
 */

#include "visibox.h"

/* ═══════════════════════════════════════════════════════════════
 * REPL MODE
 *
 * Read one JSON line from stdin → dispatch → print response → repeat.
 * Exits on EOF (stdin closed) or empty line.
 * ═══════════════════════════════════════════════════════════════ */

int visibox_repl_mode(void) {
    fprintf(stderr, "visibox: REPL mode — enter JSON requests, one per line.\n");
    fprintf(stderr, "visibox: Press Ctrl-D or send EOF to exit.\n");

    char *line = NULL;
    size_t line_cap = 0;
    ssize_t line_len;
    int request_count = 0;

    /* We use a FILE* based line reader for simplicity.
     * For very large requests, this buffers in stdio. */
    while ((line_len = getline(&line, &line_cap, stdin)) > 0) {
        /* Strip trailing newline/carriage return */
        while (line_len > 0 && (line[line_len - 1] == '\n' ||
                                line[line_len - 1] == '\r')) {
            line[--line_len] = '\0';
        }

        /* Skip empty lines */
        if (line_len == 0) continue;

        request_count++;

        /* Parse request */
        VisiboxRequest req;
        memset(&req, 0, sizeof(VisiboxRequest));
        VisiboxResponse res;
        memset(&res, 0, sizeof(VisiboxResponse));

        if (visibox_parse_request(line, &req) != 0) {
            visibox_make_error(NULL, &res, VB_ERR_INVALID_REQUEST,
                               "Failed to parse JSON request");
            char *resp_json = visibox_serialize_response(&res);
            if (resp_json) {
                printf("%s\n", resp_json);
                fflush(stdout);
                free(resp_json);
            }
            continue;
        }

        /* Dispatch */
        visibox_dispatch_request(&req, &res);

        /* Free request-owned strings */
        if (req.command) free(req.command);
        if (req.keyword) free(req.keyword);
        if (req.input) free(req.input);
        if (req.cursor) free(req.cursor);
        if (req.prompt_pattern) free(req.prompt_pattern);

        /* Serialize and print response */
        char *resp_json = visibox_serialize_response(&res);
        if (resp_json) {
            printf("%s\n", resp_json);
            fflush(stdout);
            free(resp_json);
        }

        /* Free response output */
        if (res.output) free(res.output);
    }

    if (line) free(line);

    fprintf(stderr, "visix: REPL exiting after %d request(s)\n", request_count);

    /* Cleanup */
    visibox_session_cleanup_all();
    visibox_evloop_destroy();
    visibox_store_cleanup();

    return 0;
}