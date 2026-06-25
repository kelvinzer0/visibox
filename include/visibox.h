/*
 * visibox.h — VisiBox AI Agent Execution Engine
 * Fork of GNU Bash 5.3-p15 with JSON protocol
 *
 * All struct declarations, constants, and function prototypes.
 */

#ifndef VISIBOX_H
#define VISIBOX_H

#define _GNU_SOURCE  /* for login_tty, pthread_timedjoin_np, etc. */

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
#include <sys/ioctl.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <poll.h>
#include <pty.h>
#include <termios.h>
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

/* Session / PTY defaults */
#define VISIBOX_PTY_ROWS               24
#define VISIBOX_PTY_COLS               80
#define VISIBOX_SESSION_READ_BUF_SIZE  4096
#define VISIBOX_SESSION_SWEEP_INTERVAL_MS 2000  /* idle sweep every 2s */

/* Prompt detection */
#define VISIBOX_DEFAULT_PROMPT_PATTERN  "\\$\\s*$"
#define VISIBOX_MAX_PROMPT_PATTERN_LEN 256
#define VISIBOX_MAX_ENV_VARS            32

/* Daemon / Socket (Fase 3) */
#define VISIBOX_MAX_CLIENTS             32
#define VISIBOX_DEFAULT_SOCKET_PATH     "/tmp/visibox.sock"
#define VISIBOX_DEFAULT_PID_FILE        "/tmp/visibox.pid"
#define VISIBOX_CLIENT_READ_BUF_SIZE    (1024 * 1024)  /* 1 MB */
#define VISIBOX_MAX_REQUEST_SIZE        (1024 * 1024)  /* 1 MB */

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
    int               prompt_detected;    /* Fase 2: prompt seen after session_input */

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

/* --- Session State (Fase 2) --- */

typedef enum {
    VB_SESSION_ALLOCATING,   /* PTY being created */
    VB_SESSION_RUNNING,      /* process running, PTY active */
    VB_SESSION_CLOSING,      /* close requested, draining final output */
    VB_SESSION_CLOSED        /* process exited, resources freed */
} SessionState;

/* Environment variable pair for session_start options.env */
typedef struct {
    char key[128];
    char val[512];
} EnvVar;

/* PTY Session — one per interactive process */
typedef struct {
    char        session_id[VISIBOX_ID_LEN];
    SessionState state;

    /* PTY file descriptors */
    int         master_fd;     /* our side — we read/write this */
    int         slave_fd;      /* child's side — closed after fork */
    pid_t       child_pid;     /* PID of the spawned process */

    /* Command that was run */
    char       *command;

    /* Output buffer — accumulates PTY output for pagination/search */
    OutputBuffer *output;

    /* Timing */
    struct timespec created_at;
    struct timespec last_activity;  /* updated on every read/write */
    size_t      uptime_ms;

    /* Prompt detection state */
    char        prompt_pattern[VISIBOX_MAX_PROMPT_PATTERN_LEN];
    int         prompt_detected;    /* 1 if prompt was seen in recent output */
    size_t      prompt_offset;      /* byte offset in output->data where prompt starts */

    /* Window size (from session_start or ioctl) */
    unsigned short rows;
    unsigned short cols;

    /* Environment overrides */
    EnvVar      env_overrides[VISIBOX_MAX_ENV_VARS];
    int         env_count;

    /* CWD override (set before session_start) */
    char        cwd[1024];

    /* Whether the child has exited */
    int         child_exited;
    int         child_exit_status;

    /* Registered in event loop? */
    int         in_eventloop;
} VisiboxSession;

/* --- Session Registry (Fase 2) --- */

typedef struct {
    VisiboxSession sessions[VISIBOX_MAX_SESSIONS];
    size_t        count;           /* active sessions */
    pthread_mutex_t lock;
    int           sweeper_running;
    pthread_t     sweeper_thread;
} SessionRegistry;

/* --- Event Loop (Fase 2) --- */

/* Event types that can be returned by the event loop */
typedef enum {
    VB_EV_SESSION_OUTPUT,     /* data available on session PTY fd */
    VB_EV_SESSION_EXITED,     /* child process exited (SIGCHLD) */
    VB_EV_SESSION_INPUT,      /* session_input needs to write to fd */
    VB_EV_SOCKET_READ,        /* daemon client connection has data (Fase 3) */
    VB_EV_SOCKET_ACCEPT,      /* new client connection (Fase 3) */
    VB_EV_SOCKET_HUP,         /* client disconnected (Fase 3) */
    VB_EV_TIMEOUT,            /* poll/epoll timed out */
    VB_EV_ERROR
} VisiboxEventType;

typedef struct {
    VisiboxEventType type;
    int              fd;           /* which fd triggered */
    char             session_id[VISIBOX_ID_LEN]; /* if session-related */
    /* For EV_SESSION_OUTPUT: data is read from the session's output buffer */
    /* For EV_SESSION_EXITED: session state is updated */
} VisiboxEvent;

/* Event loop using epoll (P3: no busy-wait) */
typedef struct {
    int         epoll_fd;           /* epoll file descriptor */
    int         sigchld_pipe[2];    /* self-pipe for SIGCHLD notification */
    int         listen_fd;          /* daemon: unix socket listen fd (Fase 3) */
    int         running;            /* 1 = loop is active */
} VisiboxEventLoop;

/* --- Client Connection (Fase 3) --- */

typedef enum {
    VB_CLIENT_READING,      /* reading request bytes */
    VB_CLIENT_PROCESSING,   /* dispatching/executing */
    VB_CLIENT_WRITING       /* sending response */
} ClientState;

typedef struct {
    int         fd;             /* client socket fd */
    ClientState state;
    char       *read_buf;       /* accumulating request bytes */
    size_t      read_len;       /* bytes in read_buf */
    size_t      read_cap;       /* allocated capacity of read_buf */
    char       *write_buf;      /* response to send */
    size_t      write_len;      /* bytes remaining to write */
    size_t      write_pos;      /* current write offset */
    struct timespec connected_at;
    int         active;         /* 1 = slot in use */
} VisiboxClient;

/* ═══════════════════════════════════════════════════════════════
 * GLOBAL STATE
 * ═══════════════════════════════════════════════════════════════ */

/* Global config — loaded from config/visibox.conf or defaults */
extern VisiboxConfig visibox_config;

/* Global response store */
extern ResponseStore visibox_store;

/* Global session registry (Fase 2) */
extern SessionRegistry visibox_sessions;

/* Global event loop (Fase 2) */
extern VisiboxEventLoop visibox_evloop;

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

/* --- visibox_session.c (Fase 2) --- */
void    visibox_session_registry_init (void);
int     visibox_session_count (void);
VisiboxSession *visibox_session_create (const char *command);
VisiboxSession *visibox_session_find (const char *session_id);
int     visibox_session_destroy (const char *session_id);
int     visibox_session_is_alive (VisiboxSession *s);
void    visibox_session_update_activity (VisiboxSession *s);
void    visibox_session_sweep_idle (void);
void   *visibox_session_sweeper_thread (void *arg);
void    visibox_session_cleanup_all (void);

/* --- visibox_session_pty.c (Fase 2) --- */
int     visibox_pty_spawn (VisiboxSession *s, const char *command);
int     visibox_pty_write (VisiboxSession *s, const char *data, size_t len);
int     visibox_pty_read_into_buffer (VisiboxSession *s);
int     visibox_pty_resize (VisiboxSession *s, unsigned short rows, unsigned short cols);
void    visibox_pty_close_fds (VisiboxSession *s);
void    visibox_pty_reap_child (VisiboxSession *s);

/* --- visibox_eventloop.c (Fase 2) --- */
int     visibox_evloop_init (void);
int     visibox_evloop_add_fd (int fd, uint32_t events, char *session_id);
int     visibox_evloop_remove_fd (int fd);
int     visibox_evloop_wait (VisiboxEvent *event, int timeout_ms);
void    visibox_evloop_destroy (void);
void    visibox_evloop_handle_sigchld (void);

/* --- visibox_prompt.c (Fase 2) --- */
char   *visibox_strip_ansi (const char *raw, size_t len, size_t *out_len);
int     visibox_detect_prompt (const char *stripped, size_t len, const char *pattern);
int     visibox_session_wait_for_prompt (VisiboxSession *s, const char *pattern,
                                          int timeout_ms, OutputBuffer *out_buf);

/* --- visibox_dispatch.c (Fase 2 additions) --- */
int visibox_handle_session_start (VisiboxRequest *req, VisiboxResponse *res);
int visibox_handle_session_input (VisiboxRequest *req, VisiboxResponse *res);
int visibox_handle_session_read (VisiboxRequest *req, VisiboxResponse *res);
int visibox_handle_session_list (VisiboxRequest *req, VisiboxResponse *res);
int visibox_handle_session_close (VisiboxRequest *req, VisiboxResponse *res);
int visibox_handle_session_fetch_page (VisiboxRequest *req, VisiboxResponse *res);

/* --- visibox_daemon.c (Fase 3) --- */
int  visibox_daemon_mode (const char *socket_path, const char *pid_file);
void visibox_daemon_shutdown (void);

/* --- visibox_repl.c (Fase 3) --- */
int  visibox_repl_mode (void);

/* --- visibox_config_loader.c (Fase 3) --- */
int  visibox_load_config (const char *path);

/* --- visibox_client.c (Fase 3) --- */
int  visibox_client_main (int argc, char **argv);

#endif /* VISIBOX_H */