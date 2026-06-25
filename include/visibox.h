/*
 * visibox.h — VisiBox AI Agent Execution Engine
 * Fork of GNU Bash 5.3-p15 with JSON protocol
 *
 * All struct declarations, constants, and function prototypes.
 */

#ifndef VISIBOX_H
#define VISIBOX_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <poll.h>
#include <ctype.h>
#include <limits.h>
#include <math.h>

/* ═══════════════════════════════════════════════════════════════
 * CONSTANTS
 * ═══════════════════════════════════════════════════════════════ */

#define VISIBOX_VERSION_MAJOR  0
#define VISIBOX_VERSION_MINOR  1
#define VISIBOX_VERSION_PATCH  0

#define VISIBOX_ID_LEN         16   /* length of generated IDs (including null) */
#define VISIBOX_MAX_CMD_LEN    65536
#define VISIBOX_MAX_KEYWORD    256
#define VISIBOX_MAX_ERROR_MSG  512

/* Response store defaults */
#define VISIBOX_STORE_MAX_ENTRIES      256
#define VISIBOX_STORE_MAX_BYTES        (50 * 1024 * 1024)  /* 50 MB */

/* Session defaults */
#define VISIBOX_MAX_SESSIONS           16
#define VISIBOX_MAX_FD                 1024

/* Pagination defaults */
#define VISIBOX_DEFAULT_PAGE_SIZE      100
#define VISIBOX_MAX_PAGE_SIZE          500
#define VISIBOX_MIN_PAGE_SIZE          10
#define VISIBOX_DEFAULT_CURSOR_EXPIRY  300000  /* 5 minutes */

/* Execute defaults */
#define VISIBOX_DEFAULT_TIMEOUT_MS     30000
#define VISIBOX_MAX_TIMEOUT_MS         300000  /* 5 minutes */

/* Output buffer */
#define VISIBOX_BUFFER_INITIAL_SIZE    4096
#define VISIBOX_BUFFER_CHUNK_SIZE      4096

/* Line numbers */
#define VISIBOX_LINENUM_SEP_DEFAULT    " \xe2\x94\x82 "  /* UTF-8 │ U+2502 */
#define VISIBOX_LINENUM_MAX_WIDTH      6

/* Search defaults */
#define VISIBOX_SEARCH_DEFAULT_CONTEXT 3
#define VISIBOX_SEARCH_MAX_CONTEXT     50

/* Pipe mode read buffer */
#define VISIBOX_READ_BUF_SIZE          65536

/* ═══════════════════════════════════════════════════════════════
 * ENUMS
 * ═══════════════════════════════════════════════════════════════ */

typedef enum {
    VB_REQ_EXECUTE,
    VB_REQ_SESSION_START,
    VB_REQ_SESSION_INPUT,
    VB_REQ_SESSION_READ,
    VB_REQ_SESSION_LIST,
    VB_REQ_SESSION_CLOSE,
    VB_REQ_FETCH_PAGE,
    VB_REQ_SESSION_FETCH_PAGE,
    VB_REQ_SEARCH_JUMP,          /* v3 */
    VB_REQ_UNKNOWN
} VisiboxRequestType;

typedef enum {
    VB_RES_EXECUTE_RESULT,
    VB_RES_SESSION_START_RESULT,
    VB_RES_SESSION_INPUT_RESULT,
    VB_RES_SESSION_READ_RESULT,
    VB_RES_SESSION_LIST_RESULT,
    VB_RES_SESSION_CLOSE_RESULT,
    VB_RES_PAGE_RESULT,
    VB_RES_SEARCH_JUMP_RESULT,   /* v3 */
    VB_RES_ERROR
} VisiboxResponseType;

typedef enum {
    VB_PAGE_LINES,
    VB_PAGE_BYTES
} PaginationMode;

typedef enum {
    VB_ERR_INVALID_REQUEST,
    VB_ERR_SESSION_NOT_FOUND,
    VB_ERR_SESSION_LIMIT_REACHED,
    VB_ERR_RESPONSE_EXPIRED,
    VB_ERR_CURSOR_INVALID,
    VB_ERR_TIMEOUT,
    VB_ERR_EXEC_FAILED,
    VB_ERR_SEARCH_KEYWORD_EMPTY,
    VB_ERR_INTERNAL
} VisiboxErrorCode;

typedef enum {
    VB_MODE_REPL,      /* Interactive REPL */
    VB_MODE_PIPE,      /* Single JSON request via stdin pipe */
    VB_MODE_DAEMON     /* Unix socket daemon (Fase 3) */
} VisiboxMode;

/* ═══════════════════════════════════════════════════════════════
 * DATA STRUCTURES
 * ═══════════════════════════════════════════════════════════════ */

/* --- Output Buffer with line index (v3) --- */

typedef struct {
    size_t offset;    /* byte offset in main buffer */
    size_t length;    /* length of line (including \n if present) */
} LineEntry;

typedef struct {
    char  *data;              /* raw output data */
    size_t capacity;          /* allocated capacity */
    size_t used;              /* bytes used */
    size_t total_lines;       /* total line count */
    size_t total_bytes;       /* total bytes (may exceed buffer if truncated) */
    int    truncated;         /* 1 if output was truncated due to limit */

    /* Line index — lazily built after drain completes */
    LineEntry *lines;         /* array of line entries */
    size_t    line_count;     /* same as total_lines */
    int       line_index_built;

    /* For counting-only mode (after truncation) */
    size_t count_lines;       /* lines counted but not stored */
    size_t count_bytes;       /* bytes counted but not stored */
} OutputBuffer;

/* --- Line Number Config (v3) --- */

typedef struct {
    int   enabled;                /* default from config */
    char  separator[16];          /* default: " │ " */
    int   max_width;              /* max digit width for line numbers */
} LineNumberConfig;

/* --- Search Config (v3) --- */

typedef struct {
    int   default_context_lines;
    int   max_context_lines;
    int   max_keyword_length;
    int   case_sensitive_default;
} SearchConfig;

/* --- Pagination Config --- */

typedef struct {
    PaginationMode mode;
    size_t default_page_size;
    size_t max_page_size;
    size_t min_page_size;
    int    include_total;
    size_t cursor_expiry_ms;
} PaginationConfig;

/* --- Response Store Config --- */

typedef struct {
    size_t max_entries;
    size_t max_total_bytes;
    /* eviction_policy: always FIFO for v3 */
} ResponseStoreConfig;

/* --- Session Config --- */

typedef struct {
    size_t max_concurrent;
    size_t default_read_timeout_ms;
    size_t default_initial_read_timeout_ms;
    size_t idle_timeout_ms;
    size_t max_output_buffer_bytes_per_session;
} SessionConfig;

/* --- Execute Config --- */

typedef struct {
    size_t default_timeout_ms;
    size_t max_timeout_ms;
} ExecuteConfig;

/* --- Master Config --- */

typedef struct {
    PaginationConfig   pagination;
    LineNumberConfig   line_numbers;
    SearchConfig       search;
    ResponseStoreConfig store;
    SessionConfig      sessions;
    ExecuteConfig      execute;
} VisiboxConfig;

/* --- VisiBox Request --- */

typedef struct {
    char             request_id[VISIBOX_ID_LEN];
    VisiboxRequestType type;
    char            *command;         /* for execute, session_start */
    char             session_id[VISIBOX_ID_LEN]; /* for session_* */
    char             response_id[VISIBOX_ID_LEN]; /* for fetch_page, search_jump */
    char            *input;           /* for session_input */
    char            *keyword;         /* for search_jump (v3) */

    /* Options */
    size_t    output_limit;
    PaginationMode output_unit;   /* lines or bytes */
    size_t    timeout_ms;
    int       line_numbers;      /* v3: inject line numbers in output */
    int       case_sensitive;    /* v3: for search_jump */
    int       occurrence;        /* v3: for search_jump, 1-based, -1 = last */
    int       context_lines;     /* v3: for search_jump */
    char     *prompt_pattern;    /* for session_input */
    int       wait_for_prompt;   /* for session_input */
    char     *cursor;            /* for fetch_page */
} VisiboxRequest;

/* --- VisiBox Response --- */

typedef struct {
    char              request_id[VISIBOX_ID_LEN];
    char              response_id[VISIBOX_ID_LEN];
    VisiboxResponseType type;

    /* Execute result */
    int               exit_code;
    int               has_exit_code;   /* false if timed out */
    size_t            duration_ms;
    int               timed_out;

    /* Output */
    char             *output;          /* formatted output string */
    size_t            output_lines;
    size_t            output_bytes;
    int               output_truncated;

    /* v3: Line numbers */
    int               line_numbers;    /* whether output has line numbers */
    size_t            line_start;      /* first line number in this output */
    size_t            line_end;        /* last line number in this output */

    /* Pagination */
    int               page;
    char              cursor[VISIBOX_ID_LEN];
    int               has_next;

    /* v3: Search jump */
    int               found;
    size_t            found_line;
    size_t            total_occurrences;
    int               search_occurrence;  /* occurrence value from request */
    char              keyword[VISIBOX_MAX_KEYWORD];

    /* Session */
    char              session_id[VISIBOX_ID_LEN];
    int               already_closed;

    /* Error */
    VisiboxErrorCode  error_code;
    char              error_message[VISIBOX_MAX_ERROR_MSG];

    /* Internal: pointer to full output buffer (for fetch_page/search_jump) */
    OutputBuffer     *full_buffer;
} VisiboxResponse;

/* --- Store Entry for Response History --- */

typedef struct {
    char          response_id[VISIBOX_ID_LEN];
    OutputBuffer *buffer;
    time_t        created_at;
    size_t        approx_bytes;
} StoreEntry;

/* --- Response Store (bounded ring buffer) --- */

typedef struct {
    StoreEntry      entries[VISIBOX_STORE_MAX_ENTRIES];
    size_t          head;            /* next write position (FIFO) */
    size_t          count;
    size_t          total_bytes_used;
    pthread_mutex_t lock;
} ResponseStore;

/* --- Search Result (v3) --- */

typedef struct {
    int    found;
    size_t found_line;           /* 1-based, absolute */
    size_t total_occurrences;
} VisiboxSearchResult;

/* ═══════════════════════════════════════════════════════════════
 * GLOBAL STATE
 * ═══════════════════════════════════════════════════════════════ */

/* Global config — loaded from config/visibox.conf or defaults */
extern VisiboxConfig visibox_config;

/* Global response store */
extern ResponseStore visibox_store;

/* Flag: are we in visibox mode (vs normal bash)? */
extern int visibox_active;

/* Flag: should we capture output for the current command? */
extern int visibox_capturing;

/* ═══════════════════════════════════════════════════════════════
 * FUNCTION PROTOTYPES
 * ═══════════════════════════════════════════════════════════════ */

/* --- visibox_core.c --- */
int  visibox_main (int argc, char **argv);
void visibox_detect_mode (int argc, char **argv, VisiboxMode *mode);
int  visibox_pipe_mode (void);
int  visibox_dispatch_request (VisiboxRequest *req, VisiboxResponse *res);
void visibox_init_config (void);

/* --- visibox_id.c --- */
void visibox_generate_id (char *out, const char *prefix);
int  visibox_id_valid (const char *id);

/* --- visibox_paginator.c --- */
OutputBuffer *visibox_output_buffer_new (void);
void          visibox_output_buffer_free (OutputBuffer *buf);
void          visibox_output_buffer_append (OutputBuffer *buf, const char *data, size_t len);
void          visibox_output_buffer_count_only (OutputBuffer *buf, const char *data, size_t len);
void          visibox_output_buffer_build_line_index (OutputBuffer *buf);
char         *visibox_output_buffer_get_page (OutputBuffer *buf, int page,
                                               size_t page_size, PaginationMode mode);
char         *visibox_output_buffer_get_line_range (OutputBuffer *buf,
                                                     size_t start, size_t end);
void          visibox_drain_pipe_with_limit (int fd, OutputBuffer *buf,
                                              size_t limit, PaginationMode mode);
void          visibox_paginator_build_metadata (OutputBuffer *buf,
                                                 VisiboxRequest *req, VisiboxResponse *res);

/* --- visibox_linenums.c (v3) --- */
char *visibox_linenums_inject (const char *raw_output, size_t line_start,
                                size_t page_lines, size_t total_lines);
int   visibox_linenums_width (size_t total_lines);

/* --- visibox_search.c (v3) --- */
VisiboxSearchResult visibox_search_keyword (const OutputBuffer *buf,
                                             const char *keyword,
                                             int case_sensitive,
                                             int occurrence);
char *visibox_search_get_context (const OutputBuffer *buf,
                                   size_t found_line,
                                   int context_lines,
                                   int line_numbers,
                                   size_t output_limit,
                                   size_t *out_line_start,
                                   size_t *out_line_end);

/* --- visibox_store.c --- */
void    visibox_store_init (void);
int     visibox_store_add (VisiboxResponse *res, OutputBuffer *buf);
OutputBuffer *visibox_store_get (const char *response_id);
int     visibox_store_has (const char *response_id);
void    visibox_store_cleanup (void);

/* --- visibox_protocol.c --- */
int  visibox_parse_request (const char *json_str, VisiboxRequest *req);
char *visibox_serialize_response (const VisiboxResponse *res);

/* --- visibox_dispatch.c --- */
int visibox_handle_execute (VisiboxRequest *req, VisiboxResponse *res);
int visibox_handle_fetch_page (VisiboxRequest *req, VisiboxResponse *res);
int visibox_handle_search_jump (VisiboxRequest *req, VisiboxResponse *res);
int visibox_make_error (VisiboxRequest *req, VisiboxResponse *res,
                         VisiboxErrorCode code, const char *msg);

/* --- Hook into bash (called from modified eval.c / execute_cmd.c) --- */
int  visibox_execute_and_capture (const char *command, VisiboxResponse *res);

/* --- Utility --- */
size_t visibox_min (size_t a, size_t b);
size_t visibox_max (size_t a, size_t b);
long   visibox_timespec_diff_ms (struct timespec *start, struct timespec *end);

#endif /* VISIBOX_H */