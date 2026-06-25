/*
 * visibox_session.c — Session Manager: Lifecycle, Registry, Idle Sweeper
 *
 * Fase 2: Manages the pool of interactive PTY sessions.
 * Each session has a unique session_id, a PTY pair, and an output buffer.
 *
 * Session lifecycle:
 *   session_start → ALLOCATING → RUNNING → (idle timeout or session_close) → CLOSED
 *
 * Thread safety: all registry operations are mutex-protected.
 * The idle sweeper runs in a background thread.
 */

#include "visibox.h"

/* ═══════════════════════════════════════════════════════════════
 * GLOBAL SESSION REGISTRY
 * ═══════════════════════════════════════════════════════════════ */

SessionRegistry visibox_sessions;

/* ═══════════════════════════════════════════════════════════════
 * Registry Init / Cleanup
 * ═══════════════════════════════════════════════════════════════ */

void visibox_session_registry_init(void) {
    memset(&visibox_sessions, 0, sizeof(SessionRegistry));
    pthread_mutex_init(&visibox_sessions.lock, NULL);
    visibox_sessions.sweeper_running = 0;
}

void visibox_session_cleanup_all(void) {
    /* Stop sweeper thread if running */
    if (visibox_sessions.sweeper_running) {
        visibox_sessions.sweeper_running = 0;
        /* Wait briefly for thread to exit */
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += 1;
        pthread_timedjoin_np(visibox_sessions.sweeper_thread, NULL, &ts);
    }

    pthread_mutex_lock(&visibox_sessions.lock);

    for (int i = 0; i < VISIBOX_MAX_SESSIONS; i++) {
        VisiboxSession *s = &visibox_sessions.sessions[i];
        if (s->state == VB_SESSION_RUNNING || s->state == VB_SESSION_CLOSING) {
            visibox_pty_close_fds(s);
            visibox_pty_reap_child(s);
            if (s->output) {
                visibox_output_buffer_free(s->output);
                s->output = NULL;
            }
            if (s->command) {
                free(s->command);
                s->command = NULL;
            }
            s->state = VB_SESSION_CLOSED;
        }
    }

    visibox_sessions.count = 0;
    pthread_mutex_unlock(&visibox_sessions.lock);
    pthread_mutex_destroy(&visibox_sessions.lock);
}

/* ═══════════════════════════════════════════════════════════════
 * Session Count
 * ═══════════════════════════════════════════════════════════════ */

int visibox_session_count(void) {
    pthread_mutex_lock(&visibox_sessions.lock);
    int c = (int)visibox_sessions.count;
    pthread_mutex_unlock(&visibox_sessions.lock);
    return c;
}

/* ═══════════════════════════════════════════════════════════════
 * Session Create — allocate a new session slot and spawn PTY
 *
 * Returns pointer to the new session, or NULL on failure.
 * Caller is responsible for populating env_overrides and cwd BEFORE
 * calling this.
 * ═══════════════════════════════════════════════════════════════ */

VisiboxSession *visibox_session_create(const char *command) {
    pthread_mutex_lock(&visibox_sessions.lock);

    /* Check session limit (P4: explicit limits) */
    if (visibox_sessions.count >= visibox_config.sessions.max_concurrent) {
        pthread_mutex_unlock(&visibox_sessions.lock);
        return NULL;
    }

    /* Find a free slot */
    int slot = -1;
    for (int i = 0; i < VISIBOX_MAX_SESSIONS; i++) {
        if (visibox_sessions.sessions[i].state == VB_SESSION_CLOSED ||
            visibox_sessions.sessions[i].state == 0) {
            slot = i;
            break;
        }
    }

    if (slot < 0) {
        pthread_mutex_unlock(&visibox_sessions.lock);
        return NULL;
    }

    VisiboxSession *s = &visibox_sessions.sessions[slot];
    memset(s, 0, sizeof(VisiboxSession));

    /* Generate session ID */
    visibox_generate_id(s->session_id, "sess");

    /* Set state */
    s->state = VB_SESSION_ALLOCATING;
    s->master_fd = -1;
    s->slave_fd = -1;
    s->child_pid = -1;
    s->child_exited = 0;
    s->child_exit_status = 0;
    s->in_eventloop = 0;

    /* Set defaults */
    s->rows = VISIBOX_PTY_ROWS;
    s->cols = VISIBOX_PTY_COLS;
    strncpy(s->prompt_pattern, VISIBOX_DEFAULT_PROMPT_PATTERN,
            VISIBOX_MAX_PROMPT_PATTERN_LEN - 1);

    /* Allocate output buffer */
    s->output = visibox_output_buffer_new();
    if (!s->output) {
        s->state = VB_SESSION_CLOSED;
        pthread_mutex_unlock(&visibox_sessions.lock);
        return NULL;
    }

    /* Copy command */
    s->command = strdup(command);
    if (!s->command) {
        visibox_output_buffer_free(s->output);
        s->output = NULL;
        s->state = VB_SESSION_CLOSED;
        pthread_mutex_unlock(&visibox_sessions.lock);
        return NULL;
    }

    /* Record creation time */
    clock_gettime(CLOCK_MONOTONIC, &s->created_at);
    clock_gettime(CLOCK_MONOTONIC, &s->last_activity);

    visibox_sessions.count++;

    /* Spawn the PTY (still under lock for safety) */
    int rc = visibox_pty_spawn(s, command);
    if (rc != 0) {
        /* PTY spawn failed — clean up */
        free(s->command);
        s->command = NULL;
        visibox_output_buffer_free(s->output);
        s->output = NULL;
        s->state = VB_SESSION_CLOSED;
        visibox_sessions.count--;
        pthread_mutex_unlock(&visibox_sessions.lock);
        return NULL;
    }

    /* Register master_fd with event loop for EPOLLIN */
    visibox_evloop_add_fd(s->master_fd, EPOLLIN, s->session_id);
    s->in_eventloop = 1;

    pthread_mutex_unlock(&visibox_sessions.lock);
    return s;
}

/* ═══════════════════════════════════════════════════════════════
 * Session Find — lookup by session_id
 * ═══════════════════════════════════════════════════════════════ */

VisiboxSession *visibox_session_find(const char *session_id) {
    if (!session_id) return NULL;

    pthread_mutex_lock(&visibox_sessions.lock);

    for (int i = 0; i < VISIBOX_MAX_SESSIONS; i++) {
        VisiboxSession *s = &visibox_sessions.sessions[i];
        if ((s->state == VB_SESSION_RUNNING || s->state == VB_SESSION_CLOSING) &&
            strcmp(s->session_id, session_id) == 0) {
            pthread_mutex_unlock(&visibox_sessions.lock);
            return s;
        }
    }

    pthread_mutex_unlock(&visibox_sessions.lock);
    return NULL;
}

/* ═══════════════════════════════════════════════════════════════
 * Session Destroy — close a session by session_id
 *
 * Steps:
 * 1. Find session
 * 2. If already closed, return already_closed
 * 3. Send SIGHUP to child (polite close)
 * 4. Wait briefly for exit
 * 5. If still alive, SIGKILL
 * 6. Reap child, close fds, free buffer
 * 7. Remove from event loop
 * ═══════════════════════════════════════════════════════════════ */

int visibox_session_destroy(const char *session_id) {
    if (!session_id) return -1;

    pthread_mutex_lock(&visibox_sessions.lock);

    VisiboxSession *s = NULL;
    int slot = -1;
    for (int i = 0; i < VISIBOX_MAX_SESSIONS; i++) {
        if (strcmp(visibox_sessions.sessions[i].session_id, session_id) == 0) {
            s = &visibox_sessions.sessions[i];
            slot = i;
            break;
        }
    }

    if (!s || (s->state != VB_SESSION_RUNNING && s->state != VB_SESSION_CLOSING)) {
        /* Session doesn't exist or already closed */
        pthread_mutex_unlock(&visibox_sessions.lock);
        return 1;  /* already_closed signal */
    }

    /* Mark as closing */
    s->state = VB_SESSION_CLOSING;

    /* Try to read any remaining output from the PTY first */
    if (s->master_fd >= 0) {
        visibox_pty_read_into_buffer(s);
    }

    /* Remove from event loop */
    if (s->in_eventloop && s->master_fd >= 0) {
        visibox_evloop_remove_fd(s->master_fd);
        s->in_eventloop = 0;
    }

    /* Close PTY fds — this sends SIGHUP to the child */
    visibox_pty_close_fds(s);

    /* Reap child with escalating signals */
    if (s->child_pid > 0 && !s->child_exited) {
        /* Give child 100ms to exit after SIGHUP (from PTY close) */
        for (int attempt = 0; attempt < 5; attempt++) {
            int status;
            pid_t result = waitpid(s->child_pid, &status, WNOHANG);
            if (result == s->child_pid) {
                s->child_exited = 1;
                if (WIFEXITED(status))
                    s->child_exit_status = WEXITSTATUS(status);
                else if (WIFSIGNALED(status))
                    s->child_exit_status = 128 + WTERMSIG(status);
                break;
            }
            if (attempt == 2) {
                /* After 3 attempts (~30ms), send SIGTERM */
                kill(s->child_pid, SIGTERM);
            }
            if (attempt == 3) {
                /* After 4 attempts, send SIGKILL */
                kill(s->child_pid, SIGKILL);
            }
            usleep(10000);  /* 10ms between attempts */
        }

        if (!s->child_exited) {
            /* Final reap — don't block forever */
            int status;
            waitpid(s->child_pid, &status, 0);
            s->child_exited = 1;
        }
    }

    /* Free resources */
    if (s->output) {
        visibox_output_buffer_free(s->output);
        s->output = NULL;
    }
    if (s->command) {
        free(s->command);
        s->command = NULL;
    }

    s->state = VB_SESSION_CLOSED;
    s->master_fd = -1;
    s->slave_fd = -1;
    visibox_sessions.count--;

    pthread_mutex_unlock(&visibox_sessions.lock);
    return 0;
}

/* ═══════════════════════════════════════════════════════════════
 * Session Alive Check
 * ═══════════════════════════════════════════════════════════════ */

int visibox_session_is_alive(VisiboxSession *s) {
    if (!s) return 0;

    /* Check if child has exited (via waitpid) */
    visibox_pty_reap_child(s);

    return (s->state == VB_SESSION_RUNNING && !s->child_exited);
}

/* ═══════════════════════════════════════════════════════════════
 * Activity Tracking
 * ═══════════════════════════════════════════════════════════════ */

void visibox_session_update_activity(VisiboxSession *s) {
    if (!s) return;
    clock_gettime(CLOCK_MONOTONIC, &s->last_activity);
}

/* ═══════════════════════════════════════════════════════════════
 * Idle Sweeper — close sessions that have been idle too long
 *
 * Called periodically (every VISIBOX_SESSION_SWEEP_INTERVAL_MS)
 * by the sweeper background thread.
 * Also called inline when checking session state.
 * ═══════════════════════════════════════════════════════════════ */

void visibox_session_sweep_idle(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);

    pthread_mutex_lock(&visibox_sessions.lock);

    for (int i = 0; i < VISIBOX_MAX_SESSIONS; i++) {
        VisiboxSession *s = &visibox_sessions.sessions[i];
        if (s->state != VB_SESSION_RUNNING) continue;

        /* Check if child has already exited */
        visibox_pty_reap_child(s);
        if (s->child_exited) {
            /* Child died — auto-close */
            pthread_mutex_unlock(&visibox_sessions.lock);
            visibox_session_destroy(s->session_id);
            pthread_mutex_lock(&visibox_sessions.lock);
            continue;
        }

        /* Check idle timeout */
        long idle_ms = visibox_timespec_diff_ms(&s->last_activity, &now);
        if ((size_t)idle_ms >= visibox_config.sessions.idle_timeout_ms) {
            char sid[VISIBOX_ID_LEN];
            strncpy(sid, s->session_id, VISIBOX_ID_LEN - 1);
            sid[VISIBOX_ID_LEN - 1] = '\0';
            pthread_mutex_unlock(&visibox_sessions.lock);
            visibox_session_destroy(sid);
            pthread_mutex_lock(&visibox_sessions.lock);
        }
    }

    pthread_mutex_unlock(&visibox_sessions.lock);
}

/* ═══════════════════════════════════════════════════════════════
 * Sweeper Background Thread
 *
 * Runs in a loop, sleeping between sweeps.
 * Started when the first session is created.
 * Stopped during cleanup.
 * ═══════════════════════════════════════════════════════════════ */

void *visibox_session_sweeper_thread(void *arg) {
    (void)arg;

    while (visibox_sessions.sweeper_running) {
        usleep(VISIBOX_SESSION_SWEEP_INTERVAL_MS * 1000);
        if (!visibox_sessions.sweeper_running) break;
        visibox_session_sweep_idle();
    }

    return NULL;
}