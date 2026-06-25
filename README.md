<p align="center">
  <strong>VisiBox</strong><br>
  <em>Fork of GNU Bash 5.3 — AI Agent Execution Engine with JSON Protocol</em>
</p>

---

## What is VisiBox?

VisiBox is a modified GNU Bash that accepts commands via **JSON** and returns structured responses — built specifically for **AI agents** that need to interact with a real shell.

Unlike shell wrappers that spawn a subprocess per command, VisiBox hooks directly into Bash's execution engine. This means shell state — `cd`, `export`, `alias` — **persists across commands**, exactly like an interactive shell.

```
echo '{"type":"execute","command":"cd /tmp"}' | ./bash
echo '{"type":"execute","command":"pwd"}' | ./bash
# → output: "/tmp"  ✓  state persisted
```

---

## Core Features

| Feature | Description |
|---|---|
| **Native State Persistence** | `cd`, `export`, `alias` persist across commands — no subprocess isolation |
| **JSON Protocol** | Structured request/response via stdin pipe, Unix socket daemon, or REPL |
| **Interactive PTY Sessions** | Spawn persistent processes (`ssh`, `python`, `mysql`), send input, read output |
| **Output Pagination** | Large output split into pages with cursors — protects AI context windows |
| **Line Numbers** | Every output line numbered like a code editor — easy to reference specific lines |
| **Keyword Search & Jump** | Search for a keyword in output, jump directly to the page containing it |
| **epoll Event Loop** | Single event loop monitors all PTY sessions — no busy-wait, scales to 16 concurrent sessions |
| **Bounded Ring Buffer** | Response history with FIFO eviction — 256 entries / 50 MB max, never unbounded |

---

## Quick Start

### Build

```bash
git clone https://github.com/kelvinzer0/visibox.git
cd visibox
./configure
make
```

The build produces a single `bash` binary with all VisiBox capabilities embedded.

### Usage Modes

**1. Pipe Mode** — single JSON request via stdin:

```bash
echo '{"type":"execute","command":"df -h","options":{"output_limit":10}}' | ./bash
```

**2. REPL Mode** — interactive JSON session:

```bash
./bash --visibox-repl
```

**3. Daemon Mode** — Unix socket server for multi-client access:

```bash
./bash --visibox-daemon --socket /tmp/visibox.sock
```

---

## JSON Protocol

### `execute` — Run a command

```json
// Request
{
  "type": "execute",
  "command": "ls -la /etc",
  "options": {
    "output_limit": 50,
    "output_unit": "lines",
    "timeout_ms": 30000
  }
}

// Response
{
  "request_id": "req_auto_1",
  "response_id": "res_a1b2c3d4e5f6g7h8",
  "type": "execute_result",
  "exit_code": 0,
  "output": "  1 │ total 1234\n  2 │ drwxr-xr-x  78 root root ...",
  "output_lines": 50,
  "output_bytes": 4096,
  "output_truncated": true,
  "line_numbers": true,
  "line_start": 1,
  "line_end": 50,
  "page": 1,
  "cursor": "cur_x1y2z3...",
  "has_next": true,
  "duration_ms": 12
}
```

### `session_start` — Spawn an interactive process

```json
{
  "type": "session_start",
  "command": "ssh user@192.168.1.100",
  "options": {
    "env": {"TERM": "xterm-256color"},
    "cwd": "/home/user",
    "initial_read_timeout_ms": 5000
  }
}
```

### `session_input` — Send input to a session

```json
{
  "type": "session_input",
  "session_id": "sess_9d8e7f6a",
  "input": "ls -la\n",
  "options": {
    "wait_for_prompt": true,
    "prompt_pattern": "\\$\\s*$",
    "timeout_ms": 10000
  }
}
```

### `search_jump` — Find keyword, jump to page (v3)

```json
{
  "type": "search_jump",
  "response_id": "res_a1b2c3d4e5f6g7h8",
  "keyword": "error",
  "options": {
    "occurrence": 1,
    "case_sensitive": false,
    "context_lines": 3,
    "line_numbers": true
  }
}
```

### `fetch_page` — Paginate large output

```json
{
  "type": "fetch_page",
  "response_id": "res_a1b2c3d4e5f6g7h8",
  "cursor": "cur_x1y2z3...",
  "options": {"output_limit": 50}
}
```

### Full Command Reference

| Command | Description |
|---|---|
| `execute` | Run a shell command, capture output |
| `session_start` | Spawn an interactive PTY session |
| `session_input` | Write input to a running session |
| `session_read` | Read new output from a session |
| `session_list` | List all active sessions with status |
| `session_close` | Close a session and get final output |
| `fetch_page` | Paginate a previous response |
| `session_fetch_page` | Paginate a session's accumulated output |
| `search_jump` | Search keyword in output, jump to page |

---

## Architecture

VisiBox modifies the GNU Bash source at the execution layer — it doesn't wrap Bash in a subprocess.

```
                    ┌──────────────────────────────────┐
                    │          VisiBox (bash)           │
                    │                                  │
  JSON stdin ────► │  visibox_protocol.c  (parse)     │
                    │       │                          │
                    │  visibox_dispatch.c  (route)     │
                    │       │                          │
                    │  ┌────┴────┐                     │
                    │  │ execute │  session_*          │
                    │  └────┬────┘     │               │
                    │       │    visibox_session_pty.c  │
                    │  eval.c hook    │                │
                    │  (fd redirect)  │                │
                    │       │    visibox_eventloop.c   │
                    │  bash native    │  (epoll)       │
                    │  execute_command│                │
                    │       │    ┌────┴────┐          │
                    │  visibox_       │ PTY    │       │
                    │  paginator.c    │ fds    │       │
                    │  (line nums,    │        │       │
                    │   search,       └────────┘       │
                    │   pagination)                    │
                    │       │                          │
  JSON stdout ◄── │  visibox_protocol.c (serialize)  │
                    └──────────────────────────────────┘
```

**Key design decisions:**

- **No extra fork for `execute`** — hooks into `eval.c`, redirects fd 1/2 via `dup2`, calls Bash's native `execute_command()`. Builtins like `cd` and `export` modify the parent process directly.
- **epoll event loop** — one loop monitors all PTY session fds. Zero busy-wait.
- **Bounded ring buffer** — response history capped at 256 entries / 50 MB. FIFO eviction with `ERR_RESPONSE_EXPIRED` on miss.

---

## Configuration

Edit `config/visibox.conf` to tune behavior:

```json
{
  "pagination": {
    "default_page_size": 100,
    "max_page_size": 500
  },
  "line_numbers": {
    "default_enabled": false,
    "separator": " │ "
  },
  "sessions": {
    "max_concurrent": 16,
    "idle_timeout_ms": 1800000
  },
  "response_store": {
    "max_entries": 256,
    "max_total_bytes": 52428800
  }
}
```

---

## CLI Client

A lightweight CLI client for testing and debugging:

```bash
# Execute a command
./client/visibox-cli execute "df -h"

# Start an interactive session
./client/visibox-cli session-start "python3"
./client/visibox-cli session-input sess_abc123 "print('hello')\n"
./client/visibox-cli session-read sess_abc123
./client/visibox-cli session-close sess_abc123
```

---

## Error Codes

| Code | Meaning |
|---|---|
| `ERR_INVALID_REQUEST` | Malformed JSON or missing required fields |
| `ERR_SESSION_NOT_FOUND` | Session ID doesn't exist |
| `ERR_SESSION_LIMIT_REACHED` | Max concurrent sessions reached (default: 16) |
| `ERR_RESPONSE_EXPIRED` | Response evicted from ring buffer |
| `ERR_CURSOR_INVALID` | Pagination cursor expired |
| `ERR_TIMEOUT` | Command exceeded timeout |
| `ERR_EXEC_FAILED` | Failed to spawn process |
| `ERR_SEARCH_KEYWORD_EMPTY` | Empty keyword in search_jump |
| `ERR_INTERNAL` | Internal VisiBox error |

---

## When to Use `execute` vs `session_start`

| Scenario | Use | Why |
|---|---|---|
| Simple commands that exit (`ls`, `df`, `git status`) | `execute` | No interactivity needed, state persists via P1 |
| Commands needing input (passwords, REPL, confirmations) | `session_start` | `execute` can't send input after launch |
| Long-running processes (`tail -f`, servers) | `session_start` | `execute` would block forever |
| Sequential remote commands over SSH | `session_start` once, then `session_input` | Avoids re-auth per command |

**Rule of thumb:** if the command exits on its own and doesn't need input → `execute`. Otherwise → `session_start`.

---

## Project Structure

```
shell/
├── visibox_core.c            # Entry point, mode switching
├── visibox_protocol.c        # JSON parse/serialize
├── visibox_dispatch.c        # Request routing
├── visibox_paginator.c       # Output buffer + pagination
├── visibox_linenums.c        # Line number injection
├── visibox_search.c          # Keyword search & jump
├── visibox_store.c           # Bounded ring buffer
├── visibox_session.c         # Session lifecycle
├── visibox_session_pty.c     # PTY spawn (openpty, fork+exec)
├── visibox_eventloop.c       # epoll multiplexer
├── visibox_prompt.c          # ANSI strip + prompt detection
├── visibox_repl.c            # Interactive REPL mode
├── visibox_daemon.c          # Unix socket daemon
├── visibox_config_loader.c   # Config file parser
└── visibox_id.c              # ID generator

include/
└── visibox.h                 # All structs & prototypes

config/
└── visibox.conf              # Runtime configuration

client/
├── visibox_cli               # Compiled CLI client
└── visibox_client.c          # CLI client source
```

---

## Versioning

| Phase | Status | Features |
|---|---|---|
| **Fase 1** | ✅ Done | `execute` with fd-redirect hook, output pagination, ring buffer, pipe mode, ID system |
| **Fase 2** | ✅ Done | PTY sessions, epoll event loop, prompt detection, session lifecycle, ANSI stripping |
| **Fase 3** | 🚧 In Progress | REPL mode, Unix socket daemon, CLI client, multi-client handling |
| **Fase 4** | Planned | Full error handling, memory limit enforcement, end-to-end test suite |

---

## License

GNU General Public License v3 — inherits from GNU Bash.

VisiBox is a fork of [GNU Bash 5.3](https://www.gnu.org/software/bash/) by Chet Ramey.