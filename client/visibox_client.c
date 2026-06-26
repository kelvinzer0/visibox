/*
 * visibox_client.c — Standalone CLI Client for VisiBox (Fase 3)
 *
 * A separate C program (not linked into the bash binary)
 * that connects to a VisiBox daemon via Unix socket and
 * sends JSON requests, printing responses.
 *
 * Usage:
 *   visibox-cli                          # connect to default socket, interactive
 *   visibox-cli -s /path/to/sock         # connect to custom socket
 *   visibox-cli -e 'echo hello'          # shorthand: send execute command
 *   visibox-cli -e 'ls -la' -n           # with line numbers
 *   visibox-cli -f request.json          # read request from file
 *   visibox-cli --sessions               # list active sessions
 *   visibox-cli --stop                   # send SIGTERM to daemon
 *
 * When no command is given, enters interactive mode:
 * read JSON from stdin line by line, send to daemon, print response.
 *
 * Protocol: each message is "<hex-length>\\n<payload>\\n"
 * (length-prefixed framing over the Unix socket)
 *
 * BUILD: gcc -o visibox-cli client/visibox_client.c
 *   (No bash dependency — this is a standalone program)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>

#define CLIENT_DEFAULT_SOCKET  "/tmp/visibox.sock"
#define CLIENT_DEFAULT_PID     "/tmp/visibox.pid"
#define CLIENT_BUF_SIZE        (4 * 1024 * 1024)  /* 4 MB for large responses */

/* ═══════════════════════════════════════════════════════════════
 * SOCKET CONNECTION
 * ═══════════════════════════════════════════════════════════════ */

static int connect_to_daemon(const char *socket_path) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return -1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "visibox-cli: cannot connect to %s: %s\n",
                socket_path, strerror(errno));
        close(fd);
        return -1;
    }

    return fd;
}

/* ═══════════════════════════════════════════════════════════════
 * LENGTH-PREFIXED I/O
 *
 * Wire format: "<hex-length>\\n<payload>\\n"
 * ═══════════════════════════════════════════════════════════════ */

/* Send a request with length-prefixed framing */
static int send_request(int fd, const char *json) {
    size_t len = strlen(json);
    char header[64];
    int hdr_len = snprintf(header, sizeof(header), "%zx\n", len);

    /* Send header */
    ssize_t n = write(fd, header, (size_t)hdr_len);
    if (n != hdr_len) {
        perror("visibox-cli: write header");
        return -1;
    }

    /* Send payload */
    size_t sent = 0;
    while (sent < len) {
        n = write(fd, json + sent, len - sent);
        if (n < 0) {
            if (errno == EINTR) continue;
            perror("visibox-cli: write payload");
            return -1;
        }
        sent += (size_t)n;
    }

    /* Send trailing newline */
    n = write(fd, "\n", 1);
    if (n != 1) {
        perror("visibox-cli: write trailing newline");
        return -1;
    }

    return 0;
}

/* Read a length-prefixed response.
 * Returns allocated string (caller must free), or NULL on error. */
static char *read_response(int fd) {
    /* Read the length header line */
    char hdr_buf[64];
    size_t hdr_pos = 0;

    while (hdr_pos < sizeof(hdr_buf) - 1) {
        ssize_t n = read(fd, hdr_buf + hdr_pos, 1);
        if (n <= 0) {
            if (n == 0) fprintf(stderr, "visibox-cli: daemon closed connection\n");
            else perror("visibox-cli: read header");
            return NULL;
        }
        if (hdr_buf[hdr_pos] == '\n') break;
        hdr_pos++;
    }
    hdr_buf[hdr_pos] = '\0';

    /* Parse length */
    size_t payload_len = (size_t)strtoull(hdr_buf, NULL, 16);
    if (payload_len == 0) payload_len = 1;  /* at least 1 byte for "{}" */

    /* Allocate response buffer */
    char *resp = (char *)malloc(payload_len + 2);  /* +2 for \n\0 */
    if (!resp) {
        fprintf(stderr, "visibox-cli: out of memory\n");
        return NULL;
    }

    /* Read payload */
    size_t total_read = 0;
    while (total_read < payload_len) {
        ssize_t n = read(fd, resp + total_read, payload_len - total_read);
        if (n <= 0) {
            if (n == 0) fprintf(stderr, "visibox-cli: unexpected EOF\n");
            else if (errno != EINTR) perror("visibox-cli: read payload");
            free(resp);
            return NULL;
        }
        total_read += (size_t)n;
    }
    resp[total_read] = '\0';

    /* Read and discard trailing newline */
    char nl;
    read(fd, &nl, 1);

    return resp;
}

/* ═══════════════════════════════════════════════════════════════
 * SHORTHAND COMMANDS
 * ═══════════════════════════════════════════════════════════════ */

static int send_execute(int fd, const char *command, int line_numbers,
                        int pretty) {
    /* Build JSON request */
    char json[65536];
    int pos = 0;

    pos += snprintf(json + pos, sizeof(json) - pos,
                    "{\"type\":\"execute\",\"command\":");

    /* Escape the command for JSON */
    pos += snprintf(json + pos, sizeof(json) - pos, "\"");
    for (const char *p = command; *p && pos < (int)sizeof(json) - 4; p++) {
        switch (*p) {
            case '"':  pos += snprintf(json+pos, sizeof(json)-pos, "\\\""); break;
            case '\\': pos += snprintf(json+pos, sizeof(json)-pos, "\\\\"); break;
            case '\n': pos += snprintf(json+pos, sizeof(json)-pos, "\\n"); break;
            case '\t': pos += snprintf(json+pos, sizeof(json)-pos, "\\t"); break;
            default:   json[pos++] = *p; break;
        }
    }
    pos += snprintf(json + pos, sizeof(json) - pos, "\"");

    if (line_numbers) {
        pos += snprintf(json + pos, sizeof(json) - pos,
                        ",\"options\":{\"line_numbers\":true}");
    }

    pos += snprintf(json + pos, sizeof(json) - pos, "}");

    if (send_request(fd, json) != 0) return -1;

    char *resp = read_response(fd);
    if (!resp) return -1;

    printf("%s\n", resp);
    free(resp);
    return 0;
}

static int send_session_list(int fd) {
    const char *json = "{\"type\":\"session_list\"}";
    if (send_request(fd, json) != 0) return -1;

    char *resp = read_response(fd);
    if (!resp) return -1;

    printf("%s\n", resp);
    free(resp);
    return 0;
}

/* ═══════════════════════════════════════════════════════════════
 * STOP DAEMON
 * ═══════════════════════════════════════════════════════════════ */

static int stop_daemon(void) {
    /* Read PID from PID file */
    FILE *f = fopen(CLIENT_DEFAULT_PID, "r");
    if (!f) {
        fprintf(stderr, "visibox-cli: cannot read PID file %s: %s\n",
                CLIENT_DEFAULT_PID, strerror(errno));
        return 1;
    }

    pid_t pid = 0;
    if (fscanf(f, "%d", &pid) != 1 || pid <= 0) {
        fprintf(stderr, "visibox-cli: invalid PID in %s\n", CLIENT_DEFAULT_PID);
        fclose(f);
        return 1;
    }
    fclose(f);

    printf("Sending SIGTERM to daemon (pid %d)\n", pid);
    if (kill(pid, SIGTERM) < 0) {
        fprintf(stderr, "visibox-cli: kill failed: %s\n", strerror(errno));
        return 1;
    }

    /* Wait briefly for daemon to exit */
    for (int i = 0; i < 30; i++) {
        usleep(100000);  /* 100ms */
        if (kill(pid, 0) < 0 && errno == ESRCH) {
            printf("Daemon stopped.\n");
            return 0;
        }
    }

    fprintf(stderr, "visibox-cli: daemon did not stop in 3s, sending SIGKILL\n");
    kill(pid, SIGKILL);
    return 0;
}

/* ═══════════════════════════════════════════════════════════════
 * INTERACTIVE MODE
 * ═══════════════════════════════════════════════════════════════ */

static int interactive_mode(int fd) {
    fprintf(stderr, "visibox-cli: connected. Enter JSON requests (Ctrl-D to exit).\n");

    char *line = NULL;
    size_t line_cap = 0;
    ssize_t line_len;

    while ((line_len = getline(&line, &line_cap, stdin)) > 0) {
        /* Strip trailing newline */
        while (line_len > 0 && (line[line_len - 1] == '\n' ||
                                line[line_len - 1] == '\r')) {
            line[--line_len] = '\0';
        }

        if (line_len == 0) continue;

        /* Special commands */
        if (strcmp(line, ":quit") == 0 || strcmp(line, ":q") == 0) break;
        if (strcmp(line, ":sessions") == 0 || strcmp(line, ":ls") == 0) {
            send_session_list(fd);
            continue;
        }
        if (strncmp(line, ":e ", 3) == 0) {
            send_execute(fd, line + 3, 0, 0);
            continue;
        }
        if (strncmp(line, ":en ", 4) == 0) {
            send_execute(fd, line + 4, 1, 0);
            continue;
        }

        /* Send raw JSON */
        if (send_request(fd, line) != 0) break;

        char *resp = read_response(fd);
        if (!resp) break;

        printf("%s\n", resp);
        free(resp);
    }

    if (line) free(line);
    return 0;
}

/* ═══════════════════════════════════════════════════════════════
 * MAIN
 * ═══════════════════════════════════════════════════════════════ */

int visibox_client_main(int argc, char **argv) {
    const char *socket_path = CLIENT_DEFAULT_SOCKET;
    const char *exec_cmd = NULL;
    const char *file_path = NULL;
    int line_numbers = 0;
    int stop = 0;
    int sessions = 0;

    /* Parse arguments */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-s") == 0 || strcmp(argv[i], "--socket") == 0) {
            if (i + 1 < argc) socket_path = argv[++i];
        } else if (strcmp(argv[i], "-e") == 0 || strcmp(argv[i], "--execute") == 0) {
            if (i + 1 < argc) exec_cmd = argv[++i];
        } else if (strcmp(argv[i], "-n") == 0 || strcmp(argv[i], "--line-numbers") == 0) {
            line_numbers = 1;
        } else if (strcmp(argv[i], "-f") == 0 || strcmp(argv[i], "--file") == 0) {
            if (i + 1 < argc) file_path = argv[++i];
        } else if (strcmp(argv[i], "--stop") == 0) {
            stop = 1;
        } else if (strcmp(argv[i], "--sessions") == 0) {
            sessions = 1;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            printf("visibox-cli — VisiBox Client\n\n");
            printf("Usage: visibox-cli [options]\n\n");
            printf("Options:\n");
            printf("  -s, --socket PATH    Connect to custom socket (default: %s)\n", CLIENT_DEFAULT_SOCKET);
            printf("  -e, --execute CMD    Execute a command (shorthand)\n");
            printf("  -n, --line-numbers   Enable line numbers in output\n");
            printf("  -f, --file PATH      Read request JSON from file\n");
            printf("  --sessions            List active sessions\n");
            printf("  --stop                Stop the daemon\n");
            printf("  -h, --help            Show this help\n");
            return 0;
        }
    }

    /* Handle --stop without connecting */
    if (stop) return stop_daemon();

    /* Connect to daemon */
    int fd = connect_to_daemon(socket_path);
    if (fd < 0) return 1;

    int rc = 0;

    if (exec_cmd) {
        /* Shorthand execute */
        rc = send_execute(fd, exec_cmd, line_numbers, 0);
    } else if (sessions) {
        /* List sessions */
        rc = send_session_list(fd);
    } else if (file_path) {
        /* Read request from file and send */
        FILE *f = fopen(file_path, "r");
        if (!f) {
            fprintf(stderr, "visibox-cli: cannot open %s: %s\n",
                    file_path, strerror(errno));
            close(fd);
            return 1;
        }

        char *line = NULL;
        size_t cap = 0;
        ssize_t len;
        while ((len = getline(&line, &cap, f)) > 0) {
            while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r'))
                line[--len] = '\0';
            if (len == 0) continue;

            if (send_request(fd, line) != 0) { rc = 1; break; }
            char *resp = read_response(fd);
            if (!resp) { rc = 1; break; }
            printf("%s\n", resp);
            free(resp);
        }
        if (line) free(line);
        fclose(f);
    } else {
        /* Interactive mode */
        rc = interactive_mode(fd);
    }

    close(fd);
    return (rc == 0) ? 0 : 1;
}

/* Standalone main for building visibox-cli separately */
#ifndef VISIBOX_MAIN
int main(int argc, char **argv) {
    return visibox_client_main(argc, argv);
}
#endif