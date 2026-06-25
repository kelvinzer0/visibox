/*
 * visibox_eventloop.c — Epoll-based Event Loop for All Session PTYs
 *
 * Fase 2: Single event loop that monitors all active session PTY fds.
 * Uses Linux epoll(7) — no busy-wait (P3).
 *
 * Design:
 *   - Each session's master_fd is registered with EPOLLIN
 *   - SIGCHLD is handled via self-pipe trick (write to pipe in signal
 *     handler, epoll watches the read-end)
 *   - visibox_evloop_wait() returns one event at a time
 *   - The caller (dispatch/session handlers) processes the event
 *
 * In pipe mode (Fase 2), the event loop is NOT the main loop.
 * Instead, session handlers call:
 *   1. Write input to PTY (visibox_pty_write)
 *   2. Poll/wait for output with timeout (visibox_evloop_wait)
 *   3. Read from PTY (visibox_pty_read_into_buffer)
 *
 * In daemon mode (Fase 3), the event loop WILL be the main loop.
 */

#include "visibox.h"

/* ═══════════════════════════════════════════════════════════════
 * GLOBAL EVENT LOOP
 * ═══════════════════════════════════════════════════════════════ */

VisiboxEventLoop visibox_evloop;

/* Map from fd → session_id for event dispatch.
 * We use a simple array indexed by fd. */
static char fd_to_session[VISIBOX_MAX_FD][VISIBOX_ID_LEN];

/* ═══════════════════════════════════════════════════════════════
 * SIGCHLD Self-Pipe
 *
 * Signal handlers can only safely call async-signal-safe functions.
 * We write a byte to a pipe in the handler, and the event loop
 * watches the read-end with epoll.
 * ═══════════════════════════════════════════════════════════════ */

static void sigchld_handler(int sig) {
    (void)sig;
    /* Write a byte to notify the event loop */
    /* write() is async-signal-safe */
    if (visibox_evloop.sigchld_pipe[1] >= 0) {
        char c = 'C';  /* C for Child */
        write(visibox_evloop.sigchld_pipe[1], &c, 1);
    }
}

/* ═══════════════════════════════════════════════════════════════
 * Event Loop Init
 * ═══════════════════════════════════════════════════════════════ */

int visibox_evloop_init(void) {
    memset(&visibox_evloop, 0, sizeof(VisiboxEventLoop));
    memset(fd_to_session, 0, sizeof(fd_to_session));

    /* Create epoll instance */
    visibox_evloop.epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (visibox_evloop.epoll_fd < 0) {
        return -1;
    }

    /* Create self-pipe for SIGCHLD */
    if (pipe(visibox_evloop.sigchld_pipe) < 0) {
        close(visibox_evloop.epoll_fd);
        visibox_evloop.epoll_fd = -1;
        return -1;
    }

    /* Set both ends non-blocking */
    fcntl(visibox_evloop.sigchld_pipe[0], F_SETFL, O_NONBLOCK);
    fcntl(visibox_evloop.sigchld_pipe[1], F_SETFL, O_NONBLOCK);

    /* Register read-end of self-pipe with epoll */
    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN;
    ev.data.fd = visibox_evloop.sigchld_pipe[0];
    if (epoll_ctl(visibox_evloop.epoll_fd, EPOLL_CTL_ADD,
                  visibox_evloop.sigchld_pipe[0], &ev) < 0) {
        close(visibox_evloop.sigchld_pipe[0]);
        close(visibox_evloop.sigchld_pipe[1]);
        close(visibox_evloop.epoll_fd);
        return -1;
    }

    /* Install SIGCHLD handler */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigchld_handler;
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGCHLD, &sa, NULL);

    visibox_evloop.running = 1;
    return 0;
}

/* ═══════════════════════════════════════════════════════════════
 * Add/Remove fd from event loop
 * ═══════════════════════════════════════════════════════════════ */

int visibox_evloop_add_fd(int fd, uint32_t events, char *session_id) {
    if (visibox_evloop.epoll_fd < 0 || fd < 0 || fd >= VISIBOX_MAX_FD)
        return -1;

    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events = events;
    ev.data.fd = fd;

    if (epoll_ctl(visibox_evloop.epoll_fd, EPOLL_CTL_ADD, fd, &ev) < 0) {
        /* May already exist — try MOD */
        if (epoll_ctl(visibox_evloop.epoll_fd, EPOLL_CTL_MOD, fd, &ev) < 0) {
            return -1;
        }
    }

    /* Store session_id mapping */
    if (session_id) {
        strncpy(fd_to_session[fd], session_id, VISIBOX_ID_LEN - 1);
        fd_to_session[fd][VISIBOX_ID_LEN - 1] = '\0';
    }

    return 0;
}

int visibox_evloop_remove_fd(int fd) {
    if (visibox_evloop.epoll_fd < 0 || fd < 0) return -1;

    epoll_ctl(visibox_evloop.epoll_fd, EPOLL_CTL_DEL, fd, NULL);
    memset(fd_to_session[fd], 0, VISIBOX_ID_LEN);
    return 0;
}

/* ═══════════════════════════════════════════════════════════════
 * Wait for next event
 *
 * Blocks until an event is available or timeout_ms expires.
 * timeout_ms: -1 = block forever, 0 = non-blocking, >0 = wait ms
 *
 * Returns 0 on event, -1 on error/timeout.
 * ═══════════════════════════════════════════════════════════════ */

int visibox_evloop_wait(VisiboxEvent *event, int timeout_ms) {
    if (!event || visibox_evloop.epoll_fd < 0) return -1;

    struct epoll_event ev;
    int nfds = epoll_wait(visibox_evloop.epoll_fd, &ev, 1, timeout_ms);

    if (nfds < 0) {
        if (errno == EINTR) {
            event->type = VB_EV_TIMEOUT;
            return 0;
        }
        return -1;
    }

    if (nfds == 0) {
        event->type = VB_EV_TIMEOUT;
        return 0;
    }

    int fd = ev.data.fd;

    /* Check if this is the SIGCHLD self-pipe */
    if (fd == visibox_evloop.sigchld_pipe[0] && (ev.events & EPOLLIN)) {
        /* Drain the pipe */
        char c;
        while (read(fd, &c, 1) > 0) { /* drain */ }

        event->type = VB_EV_SESSION_EXITED;
        event->fd = -1;  /* no specific fd */
        memset(event->session_id, 0, VISIBOX_ID_LEN);

        /* Notify all sessions to reap their children */
        visibox_evloop_handle_sigchld();
        return 0;
    }

    /* Check for errors */
    if (ev.events & (EPOLLERR | EPOLLHUP)) {
        /* PTY closed or error — treat as session exit */
        event->type = VB_EV_SESSION_EXITED;
        event->fd = fd;
        if (fd < VISIBOX_MAX_FD) {
            strncpy(event->session_id, fd_to_session[fd], VISIBOX_ID_LEN - 1);
        }
        return 0;
    }

    /* Data available on a session PTY */
    if (ev.events & EPOLLIN) {
        event->type = VB_EV_SESSION_OUTPUT;
        event->fd = fd;
        if (fd < VISIBOX_MAX_FD) {
            strncpy(event->session_id, fd_to_session[fd], VISIBOX_ID_LEN - 1);
        }
        return 0;
    }

    event->type = VB_EV_ERROR;
    return 0;
}

/* ═══════════════════════════════════════════════════════════════
 * Handle SIGCHLD — reap all exited children
 *
 * Called when the self-pipe signals SIGCHLD.
 * We non-blocking waitpid all children and update session state.
 * ═══════════════════════════════════════════════════════════════ */

void visibox_evloop_handle_sigchld(void) {
    /* Reap all dead children */
    int status;
    pid_t pid;
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        /* Find the session with this PID */
        pthread_mutex_lock(&visibox_sessions.lock);
        for (int i = 0; i < VISIBOX_MAX_SESSIONS; i++) {
            VisiboxSession *s = &visibox_sessions.sessions[i];
            if (s->child_pid == pid && s->state == VB_SESSION_RUNNING) {
                s->child_exited = 1;
                if (WIFEXITED(status))
                    s->child_exit_status = WEXITSTATUS(status);
                else if (WIFSIGNALED(status))
                    s->child_exit_status = 128 + WTERMSIG(status);

                /* Read any remaining output from the PTY before it closes */
                visibox_pty_read_into_buffer(s);
                break;
            }
        }
        pthread_mutex_unlock(&visibox_sessions.lock);
    }
}

/* ═══════════════════════════════════════════════════════════════
 * Event Loop Destroy
 * ═══════════════════════════════════════════════════════════════ */

void visibox_evloop_destroy(void) {
    visibox_evloop.running = 0;

    if (visibox_evloop.sigchld_pipe[0] >= 0) {
        close(visibox_evloop.sigchld_pipe[0]);
        visibox_evloop.sigchld_pipe[0] = -1;
    }
    if (visibox_evloop.sigchld_pipe[1] >= 0) {
        close(visibox_evloop.sigchld_pipe[1]);
        visibox_evloop.sigchld_pipe[1] = -1;
    }
    if (visibox_evloop.epoll_fd >= 0) {
        close(visibox_evloop.epoll_fd);
        visibox_evloop.epoll_fd = -1;
    }

    /* Restore default SIGCHLD handler */
    signal(SIGCHLD, SIG_DFL);
}