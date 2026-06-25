/*
 * visibox_daemon.c — Unix Domain Socket Daemon Mode (Fase 3)
 *
 * VisiBox as a background daemon serving multiple AI agent clients
 * over a Unix domain socket.
 *
 * Architecture:
 *   - Single event loop (epoll from Fase 2) now ALSO watches:
 *       1. listen_fd  → EPOLLIN for new connections (VB_EV_SOCKET_ACCEPT)
 *       2. client fds  → EPOLLIN for request data (VB_EV_SOCKET_READ)
 *       3. session PTY fds → EPOLLIN for PTY output (existing)
 *       4. SIGCHLD self-pipe → child process exit (existing)
 *
 *   - Each client connection gets a VisiboxClient struct with
 *     a read buffer that accumulates bytes until a complete JSON
 *     request is received (terminated by '\n').
 *
 *   - The daemon is single-threaded: only one request executes at a time.
 *     This is correct because:
 *       a) execute commands run in the bash process itself (P1: state persists)
 *       b) sessions are async via PTY — no blocking
 *     If we need concurrency in the future, we fork per-client.
 *
 * Lifecycle:
 *   1. visibox --daemon [--socket /path/to/sock] [--pid-file /path/to/pid]
 *   2. Remove stale socket if exists
 *   3. Create + bind + listen on Unix socket
 *   4. Write PID file
 *   5. Register listen_fd with epoll
 *   6. Enter main event loop
 *   7. On SIGTERM/SIGINT: cleanup and exit
 */

#include "visibox.h"

/* ═══════════════════════════════════════════════════════════════
 * CLIENT TABLE
 *
 * Fixed-size array of client slots. Indexed by searching for
 * matching fd. Could use a hash map for >32 clients but
 * VISIBOX_MAX_CLIENTS=32 is tiny — linear scan is fine.
 * ═══════════════════════════════════════════════════════════════ */

static VisiboxClient clients[VISIBOX_MAX_CLIENTS];
static int client_count = 0;

/* ═══════════════════════════════════════════════════════════════
 * SIGNAL HANDLING FOR GRACEFUL SHUTDOWN
 * ═══════════════════════════════════════════════════════════════ */

static volatile sig_atomic_t daemon_shutdown_requested = 0;
static int self_shutdown_pipe[2] = { -1, -1 };

static void daemon_sigterm_handler(int sig) {
    (void)sig;
    daemon_shutdown_requested = 1;
    if (self_shutdown_pipe[1] >= 0) {
        char c = 'S';  /* S for Shutdown */
        write(self_shutdown_pipe[1], &c, 1);
    }
}

/* ═══════════════════════════════════════════════════════════════
 * CLIENT MANAGEMENT
 * ═══════════════════════════════════════════════════════════════ */

static VisiboxClient *find_client_by_fd(int fd) {
    for (int i = 0; i < VISIBOX_MAX_CLIENTS; i++) {
        if (clients[i].active && clients[i].fd == fd)
            return &clients[i];
    }
    return NULL;
}

static VisiboxClient *alloc_client(void) {
    for (int i = 0; i < VISIBOX_MAX_CLIENTS; i++) {
        if (!clients[i].active) return &clients[i];
    }
    return NULL;
}

static void client_init(VisiboxClient *c, int fd) {
    memset(c, 0, sizeof(VisiboxClient));
    c->fd = fd;
    c->state = VB_CLIENT_READING;
    c->read_cap = 4096;
    c->read_buf = (char *)malloc(c->read_cap);
    c->read_len = 0;
    c->write_buf = NULL;
    c->write_len = 0;
    c->write_pos = 0;
    clock_gettime(CLOCK_MONOTONIC, &c->connected_at);
    c->active = 1;
    client_count++;
}

static void client_cleanup(VisiboxClient *c) {
    if (!c->active) return;

    if (c->fd >= 0) {
        visibox_evloop_remove_fd(c->fd);
        close(c->fd);
        c->fd = -1;
    }
    if (c->read_buf) { free(c->read_buf); c->read_buf = NULL; }
    if (c->write_buf) { free(c->write_buf); c->write_buf = NULL; }
    c->active = 0;
    client_count--;
}

static void client_cleanup_all(void) {
    for (int i = 0; i < VISIBOX_MAX_CLIENTS; i++) {
        client_cleanup(&clients[i]);
    }
    client_count = 0;
}

/* ═══════════════════════════════════════════════════════════════
 * CLIENT I/O
 *
 * Read: accumulate bytes into read_buf until we find '\n'.
 *       When found, we have a complete JSON request → process it.
 *
 * Write: send response bytes. Non-blocking, handles partial writes.
 * ═══════════════════════════════════════════════════════════════ */

/* Process a complete JSON request from a client's read buffer.
 * The request is null-terminated (we replace '\n' with '\0').
 * Returns 0 on success, -1 on error (client should be disconnected). */
static int client_process_request(VisiboxClient *c) {
    /* Null-terminate the request (replace the trailing '\n') */
    c->read_buf[c->read_len - 1] = '\0';  /* remove '\n' */

    VisiboxRequest req;
    memset(&req, 0, sizeof(VisiboxRequest));
    VisiboxResponse res;
    memset(&res, 0, sizeof(VisiboxResponse));

    if (visibox_parse_request(c->read_buf, &req) != 0) {
        visibox_make_error(NULL, &res, VB_ERR_INVALID_REQUEST,
                           "Failed to parse JSON request");
    } else if (req.type == VB_REQ_EXECUTE) {
        /*
         * FASE 3: Fork a child process for execute requests.
         *
         * bash's parse_and_execute() is NOT safe to call multiple times
         * outside reader_loop() — calling it twice causes SIGABRT/SEGFAULT
         * due to stale internal state. The solution: fork a fresh child
         * for each execute. Shell state (cwd, env) is scoped to that
         * child's connection lifetime (per PRD v3 open question #3).
         */
        int resp_pipe[2];
        if (pipe(resp_pipe) < 0) {
            visibox_make_error(&req, &res, VB_ERR_INTERNAL,
                               "pipe() failed for execute fork");
        } else {
            pid_t pid = fork();
            if (pid < 0) {
                close(resp_pipe[0]); close(resp_pipe[1]);
                visibox_make_error(&req, &res, VB_ERR_INTERNAL, "fork() failed");
            } else if (pid == 0) {
                /* ── CHILD: handle execute in fresh process ── */
                close(resp_pipe[0]);
                /* Set close-on-exec so pipe closes when child exits */
                fcntl(resp_pipe[1], F_SETFD, FD_CLOEXEC);

                /* Close inherited daemon fds */
                if (visibox_evloop.listen_fd >= 0)
                    close(visibox_evloop.listen_fd);
                if (visibox_evloop.sigchld_pipe[0] >= 0)
                    close(visibox_evloop.sigchld_pipe[0]);
                if (visibox_evloop.sigchld_pipe[1] >= 0)
                    close(visibox_evloop.sigchld_pipe[1]);
                if (self_shutdown_pipe[0] >= 0)
                    close(self_shutdown_pipe[0]);
                if (self_shutdown_pipe[1] >= 0)
                    close(self_shutdown_pipe[1]);

                visibox_dispatch_request(&req, &res);

                /* Serialize response and write to pipe */
                char *rj = visibox_serialize_response(&res);
                if (rj) {
                    size_t jlen = strlen(rj);
                    char hdr[32];
                    int hlen = snprintf(hdr, sizeof(hdr), "%zx\n", jlen);
                    (void)write(resp_pipe[1], hdr, (size_t)hlen);
                    (void)write(resp_pipe[1], rj, jlen);
                    (void)write(resp_pipe[1], "\n", 1);
                    free(rj);
                }
                if (req.command) free(req.command);
                if (req.keyword) free(req.keyword);
                if (req.input) free(req.input);
                if (req.cursor) free(req.cursor);
                if (req.prompt_pattern) free(req.prompt_pattern);
                if (res.output) free(res.output);
                _exit(0);
            } else {
                /* ── PARENT: read serialized response from child ── */
                close(resp_pipe[1]);

                /* Read length header */
                char hdr_buf[32];
                size_t hp = 0;
                while (hp < sizeof(hdr_buf) - 1) {
                    ssize_t n = read(resp_pipe[0], hdr_buf + hp, 1);
                    if (n <= 0) break;
                    if (hdr_buf[hp] == '\n') break;
                    hp++;
                }
                hdr_buf[hp] = '\0';

                size_t plen = (hp > 0) ? (size_t)strtoull(hdr_buf, NULL, 16) : 0;
                int got_response = 0;

                if (plen > 0 && plen < VISIBOX_MAX_REQUEST_SIZE) {
                    c->write_buf = (char *)malloc(plen + 2);
                    if (c->write_buf) {
                        size_t rd = 0;
                        while (rd < plen) {
                            ssize_t n = read(resp_pipe[0],
                                              c->write_buf + rd, plen - rd);
                            if (n <= 0) break;
                            rd += (size_t)n;
                        }
                        c->write_buf[rd] = '\0';
                        c->write_len = rd;
                        c->write_pos = 0;
                        c->state = VB_CLIENT_WRITING;
                        got_response = 1;
                    }
                }
                close(resp_pipe[0]);
                waitpid(pid, NULL, 0);

                if (got_response) {
                    visibox_evloop_remove_fd(c->fd);
                    visibox_evloop_add_fd(c->fd, EPOLLOUT, NULL);
                    c->read_len = 0;
                    return 0;
                }
                /* Fall through: generate error response */
                visibox_make_error(&req, &res, VB_ERR_INTERNAL,
                                   "execute child failed");
            }
        }
    } else {
        /* Non-execute: dispatch directly (session ops, fetch, search) */
        visibox_dispatch_request(&req, &res);
    }

    /* Free request-owned strings */
    if (req.command) free(req.command);
    if (req.keyword) free(req.keyword);
    if (req.input) free(req.input);
    if (req.cursor) free(req.cursor);
    if (req.prompt_pattern) free(req.prompt_pattern);
    if (res.output) free(res.output);

    /* Serialize response */
    char *resp_json = visibox_serialize_response(&res);
    if (!resp_json) {
        resp_json = strdup("{\"type\":\"error\",\"error_code\":\"ERR_INTERNAL\","
                           "\"error_message\":\"serialization failed\"}");
    }

    /* Prepare for writing: "<hex-length>\n<json>\n" */
    size_t json_len = strlen(resp_json);
    size_t total = 64 + json_len + 2;

    c->write_buf = (char *)malloc(total);
    if (c->write_buf) {
        c->write_len = snprintf(c->write_buf, total, "%zx\n%s\n",
                                json_len, resp_json);
        c->write_pos = 0;
        c->state = VB_CLIENT_WRITING;

        visibox_evloop_remove_fd(c->fd);
        visibox_evloop_add_fd(c->fd, EPOLLOUT, NULL);
    } else {
        free(resp_json);
        return -1;
    }

    free(resp_json);
    c->read_len = 0;
    return 0;
}

static int client_handle_read(VisiboxClient *c) {
    if (!c->active) return -1;

    char tmp[4096];
    ssize_t n = read(c->fd, tmp, sizeof(tmp));

    if (n <= 0) {
        /* Connection closed or error */
        return -1;
    }

    /* Append to read buffer */
    if (c->read_len + (size_t)n >= c->read_cap) {
        /* Grow buffer */
        size_t new_cap = c->read_cap * 2;
        if (new_cap > VISIBOX_MAX_REQUEST_SIZE) new_cap = VISIBOX_MAX_REQUEST_SIZE;
        if (c->read_len + (size_t)n >= new_cap) {
            /* Request too large — disconnect */
            return -1;
        }
        char *new_buf = (char *)realloc(c->read_buf, new_cap);
        if (!new_buf) return -1;
        c->read_buf = new_buf;
        c->read_cap = new_cap;
    }

    memcpy(c->read_buf + c->read_len, tmp, (size_t)n);
    c->read_len += (size_t)n;

    /* Check if we have a complete request (ended with '\n') */
    if (c->read_len > 0 && c->read_buf[c->read_len - 1] == '\n') {
        c->state = VB_CLIENT_PROCESSING;
        return client_process_request(c);
    }

    return 0;
}

static int client_handle_write(VisiboxClient *c) {
    if (!c->active || !c->write_buf) return -1;

    ssize_t n = write(c->fd, c->write_buf + c->write_pos,
                      c->write_len - c->write_pos);

    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
        return -1;
    }

    c->write_pos += (size_t)n;

    if (c->write_pos >= c->write_len) {
        /* Response fully sent — go back to reading */
        free(c->write_buf);
        c->write_buf = NULL;
        c->write_len = 0;
        c->write_pos = 0;
        c->state = VB_CLIENT_READING;

        /* Switch epoll back to read */
        visibox_evloop_remove_fd(c->fd);
        visibox_evloop_add_fd(c->fd, EPOLLIN, NULL);
    }

    return 0;
}

/* ═══════════════════════════════════════════════════════════════
 * NEW CONNECTION
 * ═══════════════════════════════════════════════════════════════ */

static int accept_new_client(int listen_fd) {
    struct sockaddr_un addr;
    socklen_t addrlen = sizeof(addr);
    int client_fd = accept(listen_fd, (struct sockaddr *)&addr, &addrlen);

    if (client_fd < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK)
            perror("visibox daemon: accept");
        return -1;
    }

    /* Check client limit */
    if (client_count >= VISIBOX_MAX_CLIENTS) {
        fprintf(stderr, "visibox daemon: max clients (%d) reached, rejecting\n",
                VISIBOX_MAX_CLIENTS);
        /* Send a polite error and close */
        const char *err = "{\"type\":\"error\",\"error_code\":\"ERR_INTERNAL\","
                          "\"error_message\":\"max clients reached\"}\n";
        write(client_fd, err, strlen(err));
        close(client_fd);
        return -1;
    }

    /* Set non-blocking */
    fcntl(client_fd, F_SETFL, O_NONBLOCK);

    /* Allocate client slot */
    VisiboxClient *c = alloc_client();
    if (!c) {
        close(client_fd);
        return -1;
    }

    client_init(c, client_fd);

    /* Register with epoll for reading */
    visibox_evloop_add_fd(client_fd, EPOLLIN, NULL);

    return 0;
}

/* ═══════════════════════════════════════════════════════════════
 * PID FILE
 * ═══════════════════════════════════════════════════════════════ */

static int write_pid_file(const char *path) {
    FILE *f = fopen(path, "w");
    if (!f) {
        perror("visibox daemon: cannot write PID file");
        return -1;
    }
    fprintf(f, "%d\n", getpid());
    fclose(f);
    return 0;
}

static void remove_pid_file(const char *path) {
    if (path) unlink(path);
}

/* ═══════════════════════════════════════════════════════════════
 * DAEMON MAIN LOOP
 * ═══════════════════════════════════════════════════════════════ */

int visibox_daemon_mode(const char *socket_path, const char *pid_file) {
    const char *sock_path = socket_path ? socket_path : VISIBOX_DEFAULT_SOCKET_PATH;
    const char *pid_path  = pid_file    ? pid_file    : VISIBOX_DEFAULT_PID_FILE;

    fprintf(stderr, "visibox daemon: starting on %s\n", sock_path);

    /* Remove stale socket */
    unlink(sock_path);

    /* Create Unix domain socket */
    int listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        perror("visibox daemon: socket");
        return 1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, sock_path, sizeof(addr.sun_path) - 1);

    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("visibox daemon: bind");
        close(listen_fd);
        return 1;
    }

    /* Set non-blocking for accept */
    fcntl(listen_fd, F_SETFL, O_NONBLOCK);

    if (listen(listen_fd, 16) < 0) {
        perror("visibox daemon: listen");
        close(listen_fd);
        unlink(sock_path);
        return 1;
    }

    /* Store listen_fd in event loop */
    visibox_evloop.listen_fd = listen_fd;

    /* Register listen_fd with epoll */
    if (visibox_evloop_add_fd(listen_fd, EPOLLIN, NULL) < 0) {
        perror("visibox daemon: epoll add listen_fd");
        close(listen_fd);
        unlink(sock_path);
        return 1;
    }

    /* Create self-pipe for shutdown signaling */
    if (pipe(self_shutdown_pipe) < 0) {
        perror("visibox daemon: shutdown pipe");
        /* Continue anyway — we can still check the volatile flag */
    } else {
        fcntl(self_shutdown_pipe[0], F_SETFL, O_NONBLOCK);
        /* Register shutdown pipe with epoll */
        visibox_evloop_add_fd(self_shutdown_pipe[0], EPOLLIN, NULL);
    }

    /* Install SIGTERM/SIGINT handlers for graceful shutdown */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = daemon_sigterm_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);

    /* Write PID file */
    write_pid_file(pid_path);

    fprintf(stderr, "visibox daemon: ready (pid %d)\n", getpid());

    /* ─── MAIN EVENT LOOP ─── */
    while (!daemon_shutdown_requested && visibox_evloop.running) {
        VisiboxEvent event;
        int rc = visibox_evloop_wait(&event, 1000);  /* 1s timeout for shutdown check */

        if (rc != 0) continue;

        /* Check shutdown pipe */
        if (event.fd == self_shutdown_pipe[0] && (event.type == VB_EV_SESSION_OUTPUT ||
                                                    event.type == VB_EV_TIMEOUT)) {
            char c;
            while (read(self_shutdown_pipe[0], &c, 1) > 0) { /* drain */ }
            if (daemon_shutdown_requested) break;
        }

        switch (event.type) {
            case VB_EV_SOCKET_ACCEPT:
                /* New client connection */
                if (event.fd == listen_fd) {
                    accept_new_client(listen_fd);
                }
                break;

            case VB_EV_SOCKET_READ:
                /* Data from a client — or write-ready for a client in WRITING state */
                {
                    VisiboxClient *c = find_client_by_fd(event.fd);
                    if (c) {
                        if (c->state == VB_CLIENT_WRITING) {
                            if (client_handle_write(c) != 0) {
                                client_cleanup(c);
                            }
                        } else {
                            if (client_handle_read(c) != 0) {
                                client_cleanup(c);
                            }
                        }
                    }
                }
                break;

            case VB_EV_SOCKET_HUP:
                /* Client disconnected */
                {
                    VisiboxClient *c = find_client_by_fd(event.fd);
                    if (c) client_cleanup(c);
                }
                break;

            case VB_EV_SESSION_OUTPUT:
                /* Session PTY has data — read it into the session buffer */
                {
                    VisiboxSession *s = visibox_session_find(event.session_id);
                    if (s) visibox_pty_read_into_buffer(s);
                }
                break;

            case VB_EV_SESSION_EXITED:
                /* Child process exited — already handled by evloop_handle_sigchld */
                break;

            case VB_EV_TIMEOUT:
                /* No events — check for idle sessions, etc. */
                break;

            case VB_EV_ERROR:
                /* Check if it's a client fd that errored */
                if (event.fd >= 0 && event.fd != listen_fd &&
                    event.fd != visibox_evloop.sigchld_pipe[0]) {
                    VisiboxClient *c = find_client_by_fd(event.fd);
                    if (c) client_cleanup(c);
                }
                break;
        }
    }

    /* ─── SHUTDOWN ─── */
    fprintf(stderr, "visibox daemon: shutting down...\n");

    /* Cleanup all clients */
    client_cleanup_all();

    /* Cleanup sessions */
    visibox_session_cleanup_all();

    /* Close listen socket */
    if (listen_fd >= 0) {
        visibox_evloop_remove_fd(listen_fd);
        close(listen_fd);
    }

    /* Close shutdown pipe */
    if (self_shutdown_pipe[0] >= 0) {
        visibox_evloop_remove_fd(self_shutdown_pipe[0]);
        close(self_shutdown_pipe[0]);
    }
    if (self_shutdown_pipe[1] >= 0) {
        close(self_shutdown_pipe[1]);
    }

    /* Remove socket and PID file */
    unlink(sock_path);
    remove_pid_file(pid_path);

    /* Cleanup event loop */
    visibox_evloop_destroy();
    visibox_store_cleanup();

    fprintf(stderr, "visibox daemon: stopped\n");
    return 0;
}

void visibox_daemon_shutdown(void) {
    daemon_shutdown_requested = 1;
    if (self_shutdown_pipe[1] >= 0) {
        char c = 'S';
        write(self_shutdown_pipe[1], &c, 1);
    }
}