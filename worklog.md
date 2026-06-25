---
Task ID: 2
Agent: main
Task: Fase 2 — Sessions + Event Loop (PTY, epoll, prompt detection)

Work Log:
- Read PRD v3 Fase 2 spec: PTY sessions, epoll event loop, ANSI strip, prompt detection
- Read all Fase 1 source files (visibox.h, core, dispatch, protocol, paginator, search, linenums, store, id)
- Updated visibox.h: added _GNU_SOURCE, Session/SessionRegistry/VisiboxEventLoop/VisiboxSession/EnvVar/SessionState/VisiboxEventType/VisiboxEvent structs, 30+ new function prototypes
- Created visibox_session_pty.c: openpty+fork+exec, non-blocking I/O, PTY resize, child reaping with SIGTERM→SIGKILL escalation
- Created visibox_session.c: registry init/cleanup, session create/find/destroy, activity tracking, idle sweeper thread
- Created visibox_eventloop.c: epoll-based multiplexer, SIGCHLD self-pipe trick, fd→session_id mapping
- Created visibox_prompt.c: ANSI strip (CSI/OSC/SS3/charset), POSIX regex prompt detection, wait-for-prompt via event loop
- Rewrote visibox_dispatch.c: implemented 6 session handlers (start/input/read/list/close/fetch_page)
- Updated visibox_protocol.c: parse session options, serialize 5 new response types
- Updated visibox_core.c: init eventloop+session registry, cleanup on exit
- Fixed Makefile.in: tab indentation, 4 new sources, -lutil
- Created build.sh: automated build with Makefile tab-fix
- Fixed _GNU_SOURCE, login_tty extern decl, prompt_detected field
- Build verified: 2.5MB binary, all 12 .o files linked
- Fase 1 regression tested: execute + line_numbers + search_jump
- Session start tested: PTY spawn works, initial output captured
- Committed and pushed to kelvinzer0/visibox

Stage Summary:
- Fase 2 complete: 4 new .c files (session, session_pty, eventloop, prompt)
- 14 files changed, +2345 -448 lines
- All PRD v3 Fase 2 items implemented: PTY session manager, epoll event loop, ANSI strip + prompt detection, session pagination + line_numbers, idle session sweeper
- Known limitation: pipe mode = separate process per request, so sessions don't persist across invocations. Full session lifecycle requires daemon mode (Fase 3).