/*
 * visibox_session_pty.c — PTY Spawn, Read, Write for Interactive Sessions
 *
 * Fase 2: Manages pseudo-terminal lifecycle for each session.
 * Uses openpty() + fork() + execvp() to spawn the session command
 * in a real PTY, giving full interactive terminal semantics
 * (line editing, signals, window size).
 *
 * The master_fd is registered with the event loop (visibox_eventloop.c)
 * so we never busy-poll (P3).
 */

#include "visibox.h"
#include <paths.h>

/* login_tty is in <pty.h> on some systems, but may need explicit decl */
extern int login_tty(int fd);

/* ═══════════════════════════════════════════════════════════════
 * PTY Spawn — openpty + fork + exec
 *
 * This is the core of session_start.
 * 1. openpty() to get master/slave fd pair
 * 2. Set terminal size (rows, cols)
 * 3. fork()
 * 4. Child: setsid, login_tty(slave), apply env, execvp(command)
 * 5. Parent: close slave, register master_fd with event loop
 * ═══════════════════════════════════════════════════════════════ */

int visibox_pty_spawn(VisiboxSession *s, const char *command) {
    if (!s || !command) return -1;

    int master_fd, slave_fd;
    struct winsize ws;

    memset(&ws, 0, sizeof(ws));
    ws.ws_row = s->rows > 0 ? s->rows : VISIBOX_PTY_ROWS;
    ws.ws_col = s->cols > 0 ? s->cols : VISIBOX_PTY_COLS;

    /* 1. Allocate PTY */
    if (openpty(&master_fd, &slave_fd, NULL, NULL, &ws) < 0) {
        return -1;
    }

    /* Set master_fd non-blocking — we'll use epoll to wait for data (P3) */
    fcntl(master_fd, F_SETFL, O_NONBLOCK);

    /* 2. Fork */
    pid_t pid = fork();
    if (pid < 0) {
        close(master_fd);
        close(slave_fd);
        return -1;
    }

    if (pid == 0) {
        /* ─── CHILD PROCESS ─── */

        /* Create new session — child becomes session leader,
         * detaches from controlling terminal of the parent VisiBox process */
        setsid();

        /* Make the slave PTY our controlling terminal */
        if (login_tty(slave_fd) < 0) {
            _exit(127);
        }

        /* Close master_fd — child doesn't need it */
        close(master_fd);

        /* Apply CWD override if set */
        if (s->cwd[0] != '\0') {
            chdir(s->cwd);
        }

        /* Apply environment overrides */
        for (int i = 0; i < s->env_count; i++) {
            setenv(s->env_overrides[i].key, s->env_overrides[i].val, 1);
        }

        /* Ensure TERM is set (default: xterm-256color) */
        if (getenv("TERM") == NULL) {
            setenv("TERM", "xterm-256color", 1);
        }

        /* Parse command into argv for execvp.
         * Support both simple commands ("ls -la") and full shell
         * invocation. We use /bin/sh -c for complex commands. */
        execl("/bin/sh", "sh", "-c", command, (char *)NULL);

        /* If exec fails */
        _exit(127);
    }

    /* ─── PARENT PROCESS ─── */

    close(slave_fd);

    s->master_fd = master_fd;
    s->slave_fd = -1;  /* closed in parent */
    s->child_pid = pid;
    s->state = VB_SESSION_RUNNING;
    s->child_exited = 0;
    s->child_exit_status = 0;

    return 0;
}

/* ═══════════════════════════════════════════════════════════════
 * PTY Write — send input to the child process
 *
 * Writes data to the master fd. This is how session_input works.
 * The child sees this as if typed on its terminal.
 * ═══════════════════════════════════════════════════════════════ */

int visibox_pty_write(VisiboxSession *s, const char *data, size_t len) {
    if (!s || s->master_fd < 0 || !data || len == 0) return -1;

    ssize_t total_written = 0;
    while (total_written < (ssize_t)len) {
        ssize_t n = write(s->master_fd, data + total_written,
                          len - (size_t)total_written);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                /* Master fd is non-blocking, wait a bit */
                struct pollfd pfd = { .fd = s->master_fd, .events = POLLOUT };
                poll(&pfd, 1, 100);
                continue;
            }
            return -1;  /* real error */
        }
        total_written += n;
    }

    visibox_session_update_activity(s);
    return 0;
}

/* ═══════════════════════════════════════════════════════════════
 * PTY Read — read available data from the PTY into session buffer
 *
 * Non-blocking read. Reads whatever is available right now.
 * Returns bytes read, or -1 on error, or 0 if no data available.
 * Called by the event loop when EPOLLIN fires.
 * ═══════════════════════════════════════════════════════════════ */

int visibox_pty_read_into_buffer(VisiboxSession *s) {
    if (!s || s->master_fd < 0 || !s->output) return -1;

    char buf[VISIBOX_SESSION_READ_BUF_SIZE];
    ssize_t n;

    /* Read in a loop until EAGAIN (no more data) */
    int total = 0;
    for (;;) {
        n = read(s->master_fd, buf, sizeof(buf));
        if (n > 0) {
            /* Append to session output buffer.
             * Apply session buffer size limit from config. */
            size_t max_buf = visibox_config.sessions.max_output_buffer_bytes_per_session;
            if (max_buf > 0 && s->output->used + (size_t)n > max_buf) {
                /* Buffer is full — count only (like truncated execute) */
                visibox_output_buffer_count_only(s->output, buf, (size_t)n);
                s->output->truncated = 1;
            } else {
                visibox_output_buffer_append(s->output, buf, (size_t)n);
            }
            total += (int)n;
        } else if (n == 0) {
            /* EOF — PTY closed (child exited) */
            s->child_exited = 1;
            break;
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;  /* no more data right now */
            }
            return -1;  /* error */
        }
    }

    if (total > 0) {
        visibox_session_update_activity(s);
    }
    return total;
}

/* ═══════════════════════════════════════════════════════════════
 * PTY Resize — change terminal window size
 * ═══════════════════════════════════════════════════════════════ */

int visibox_pty_resize(VisiboxSession *s, unsigned short rows, unsigned short cols) {
    if (!s || s->master_fd < 0) return -1;

    struct winsize ws;
    memset(&ws, 0, sizeof(ws));
    ws.ws_row = rows;
    ws.ws_col = cols;

    if (ioctl(s->master_fd, TIOCSWINSZ, &ws) < 0) {
        return -1;
    }

    s->rows = rows;
    s->cols = cols;
    return 0;
}

/* ═══════════════════════════════════════════════════════════════
 * PTY Close — close file descriptors and reap child
 * ═══════════════════════════════════════════════════════════════ */

void visibox_pty_close_fds(VisiboxSession *s) {
    if (!s) return;

    if (s->master_fd >= 0) {
        /* Remove from event loop first */
        visibox_evloop_remove_fd(s->master_fd);
        close(s->master_fd);
        s->master_fd = -1;
    }
    if (s->slave_fd >= 0) {
        close(s->slave_fd);
        s->slave_fd = -1;
    }
}

void visibox_pty_reap_child(VisiboxSession *s) {
    if (!s) return;

    if (s->child_pid > 0 && !s->child_exited) {
        int status;
        pid_t result = waitpid(s->child_pid, &status, WNOHANG);
        if (result == s->child_pid) {
            s->child_exited = 1;
            if (WIFEXITED(status)) {
                s->child_exit_status = WEXITSTATUS(status);
            } else if (WIFSIGNALED(status)) {
                s->child_exit_status = 128 + WTERMSIG(status);
            }
        }
    }
}