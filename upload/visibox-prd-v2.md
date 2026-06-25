# VisiBox — PRD v2
## Shell Execution Engine untuk AI Agent
### Dokumen internal — panduan implementasi

---

## 0. RINGKASAN EKSEKUTIF

VisiBox adalah fork bash yang menambahkan 3 kapabilitas untuk dikendalikan AI agent lewat protokol JSON:

1. **ID system** — setiap operasi punya `request_id` (milik caller) dan `response_id` (milik VisiBox), untuk audit dan referensi silang.
2. **Interactive session control** — AI bisa spawn proses interaktif (ssh, python, mysql), kirim input, baca output, kapan saja, tanpa proses itu mati di antara request.
3. **Output pagination** — output besar dipotong per halaman dengan cursor, supaya AI tidak overload context window.

Dokumen ini adalah **versi rewrite** dari draft sebelumnya. Enam masalah desain di draft lama sudah diperbaiki di sini — lihat §1 untuk daftar lengkap apa yang berubah dan kenapa. Kalau Anda sudah baca draft v1, baca §1 dulu sebelum lanjut; jangan asumsikan struktur datanya sama.

---

## 1. APA YANG BERUBAH DARI v1 (DAN KENAPA)

| # | Masalah di v1 | Perbaikan di v2 |
|---|---|---|
| 1 | `execute` fork ulang shell baru per command → `cd`, `export` tidak persist antar request | Command non-session dieksekusi di **proses bash induk yang sama**, lewat jalur eksekusi asli bash. Shell state (cwd, env, alias) persist secara native, sama seperti shell interaktif biasa. |
| 2 | Truncation tetap drain seluruh output dari pipe → tidak menghemat compute, hanya menghemat token | Didokumentasikan eksplisit: `output_limit` adalah **proteksi token**, bukan proteksi compute. Ditambahkan rekomendasi command-level limiting (`head`, `LIMIT`) sebagai tanggung jawab AI agent, bukan VisiBox. |
| 3 | Response history ring buffer tanpa kebijakan retensi/limit memori | Ring buffer punya **kapasitas eksplisit** (jumlah entri + total bytes), kebijakan eviction (FIFO), dan response error standar (`ERR_RESPONSE_EXPIRED`) kalau `fetch_page` mengacu ke response yang sudah di-evict. |
| 4 | PTY read loop pakai busy-poll `usleep(10000)` per session → tidak scale ke concurrent session | Diganti `poll()`/`epoll` dari Fase 2 (bukan ditunda ke Fase 3). Satu event loop memantau semua PTY fd sekaligus. Tidak ada busy-wait. |
| 5 | Prompt detection regex rapuh terhadap ANSI escape code | Output PTY di-strip ANSI **sebelum** regex match (raw output tetap disimpan untuk ditampilkan ke AI). |
| 6 | Tidak jelas kapan AI harus pakai `execute` vs `session_start` | Ditambahkan aturan keputusan eksplisit di §3, dengan tabel kapan pakai yang mana. |

---

## 2. PRINSIP DESAIN

Ini bukan sekadar daftar fitur — ini aturan yang dipegang konsisten di seluruh implementasi, supaya saat coding solo nanti tidak bikin keputusan ad-hoc yang saling kontradiksi.

**P1 — Shell state itu sakral.**
Command non-interaktif (`execute`) HARUS berjalan di proses bash yang sama, bukan subprocess terpisah. Kalau ini dilanggar, seluruh value proposition "AI bisa `cd` lalu `ls`" hilang.

**P2 — Pagination melindungi token, bukan compute.**
VisiBox tidak menjanjikan command berhenti lebih cepat karena `output_limit` kecil. Itu tanggung jawab command itu sendiri (`head`, `LIMIT N`, dll). Dokumentasikan ini ke pengguna API supaya ekspektasi benar.

**P3 — Tidak ada busy-wait di hot path.**
Semua I/O menunggu (PTY read, socket read) lewat `poll()`/`epoll`, bukan sleep-loop. Ini bukan optimisasi prematur — ini syarat supaya Fase 2 tidak perlu rewrite di Fase 3.

**P4 — Semua resource punya batas eksplisit dan kebijakan eviction.**
Ring buffer response, jumlah session aktif, ukuran output buffer per session — semua punya angka konkret di config, bukan "in-memory, unbounded" yang baru ditemukan masalahnya saat production.

**P5 — Ambiguitas protokol = bug spec, bukan bug implementasi.**
Kalau ada pertanyaan "request macam apa yang harus dikirim AI di situasi X" yang tidak terjawab oleh dokumen ini, itu dianggap PRD belum lengkap — bukan sesuatu yang "akan jelas saat coding". Setiap request type punya kontrak lengkap di §5.

---

## 3. KAPAN PAKAI `execute` VS `session_start`

Ini keputusan yang sering diabaikan di desain serupa, padahal krusial buat AI agent supaya tidak salah pilih jalur.

| Situasi | Jalur | Alasan |
|---|---|---|
| Command selesai dengan sendirinya, exit code jelas (`ls`, `df -h`, `git status`) | `execute` | Tidak butuh state interaktif lintas-command selain shell env (yang otomatis persist via P1). |
| Command butuh input interaktif (password, konfirmasi `y/n`, REPL) | `session_start` + `session_input` | `execute` tidak punya jalur untuk mengirim input setelah command jalan. |
| Command yang sengaja tidak pernah exit (`tail -f`, server foreground, `watch`) | `session_start` | `execute` akan menunggu exit yang tidak pernah terjadi → harus timeout-kill, yang merusak semantik "command selesai, ini hasilnya". |
| Butuh banyak command berurutan dengan koneksi/proses yang sama (ssh lalu beberapa command remote) | `session_start` sekali, lalu beberapa `session_input` | Membuka ssh baru per command = re-auth tiap kali, tidak efisien, dan kalau session punya state (mysql `USE db;`) akan hilang. |
| Command lokal sederhana tapi dipanggil bertubi-tubi dengan dependency pada `cwd`/env yang diubah command sebelumnya | `execute` (berkat P1, ini AMAN — cwd persist otomatis) | Tidak perlu session untuk ini; ini justru kasus yang dulu rusak di v1. |

Aturan ringkas untuk AI agent: **kalau command itu sendiri akan exit dan tidak butuh input tambahan, pakai `execute`. Selain itu, `session_start`.**

---

## 4. ARSITEKTUR

### 4.1 Prinsip struktural

VisiBox = bash source + lapisan tambahan yang **hook ke jalur eksekusi asli**, bukan menggantikannya dengan subprocess wrapper. Ini beda penting dari "shell wrapper" generik (seperti banyak agentic-shell tool yang cuma `subprocess.run()` di Python) — di sini kita betul-betul masuk ke source bash.

```
visibox/
├── (bash source, fork dari upstream GNU bash)
│
├── shell/
│   ├── visibox_core.c           # Entry point, mode switcher (pipe/daemon/repl)
│   ├── visibox_id.c             # ID generator & validasi
│   ├── visibox_session.c        # Session manager: lifecycle, registry
│   ├── visibox_session_pty.c    # PTY spawn (openpty, login_tty, fork+exec)
│   ├── visibox_eventloop.c      # epoll/poll multiplexer — SATU loop untuk semua session fd
│   ├── visibox_prompt.c         # ANSI strip + regex prompt detection
│   ├── visibox_paginator.c      # Output buffer + pagination + cursor encoding
│   ├── visibox_store.c          # Response history ring buffer (bounded, evicting)
│   ├── visibox_protocol.c       # JSON parse/serialize, request validation
│   ├── visibox_dispatch.c       # Routing: request type → handler
│   │
│   ├── execute_cmd.c            # MODIFIED: hook output capture ke jalur asli (TIDAK fork shell baru)
│   ├── eval.c                   # MODIFIED: titip dispatch hook setelah command line di-parse
│   └── input.c                  # MODIFIED: baca dari stdin/socket sesuai mode
│
├── include/
│   └── visibox.h                # Semua struct & deklarasi (lihat §6)
│
├── config/
│   └── visibox.conf             # Lihat §7
│
├── client/
│   ├── visibox-cli              # CLI client untuk test/debug manual
│   └── libvisibox.h             # Client library header (opsional, Fase 3)
│
└── tests/
    ├── test_execute_state.c     # Verifikasi cwd/env persist antar execute (regression test untuk P1)
    ├── test_pagination.c
    ├── test_session_lifecycle.c
    └── test_eventloop_concurrent.c
```

### 4.2 Bagaimana `execute` TIDAK fork shell baru (perbaikan masalah #1)

Ini bagian paling penting yang berubah dari v1. Penjelasan mekanismenya:

**Yang SALAH (v1):** parent fork() → child jalankan command via `visibox_execute_original()` di proses terpisah → exit. Command seperti `cd` cuma mengubah cwd di child, hilang begitu child exit. Parent (proses VisiBox utama) cwd-nya tidak pernah berubah.

**Yang BENAR (v2):** Hook ditempatkan di `eval.c`, **setelah** bash parse command line jadi command struct, tapi **sebelum/sesudah** bash menjalankannya lewat jalur normal (`execute_command()` bash asli). Untuk command yang sudah `fork` secara native oleh bash (kebanyakan command eksternal memang fork di bash), kita tidak menambah fork lagi — kita hanya:

1. Redirect fd 1/2 proses VisiBox **sementara** (lewat `dup2` + simpan fd asli) sebelum memanggil jalur eksekusi bash asli.
2. Panggil `execute_command()` bash asli — bash sendiri yang menentukan apakah perlu fork (command eksternal) atau tidak (builtin seperti `cd`, `export`).
3. Setelah command selesai, restore fd 1/2 ke semula.
4. Buffer yang menampung output adalah pipe yang dibaca di proses VisiBox utama sendiri (bukan child terpisah) — karena fd 1/2 sudah diarahkan ke pipe itu selama command jalan.

Konsekuensi: builtin (`cd`, `export`, `alias`, `unset`) memodifikasi state proses VisiBox utama secara langsung, persis seperti shell interaktif normal. Command eksternal tetap fork (seperti biasanya di bash), tapi itu fork yang sama yang sudah dilakukan bash secara native — bukan fork tambahan dari VisiBox.

```c
// execute_cmd.c — MODIFIED (v2, bukan v1)
int visibox_execute_and_capture(VisiboxRequest *req, VisiboxResponse *res) {
    visibox_generate_id(res->response_id, "res_");

    int pipefd[2];
    pipe(pipefd);

    int saved_stdout = dup(STDOUT_FILENO);
    int saved_stderr = dup(STDERR_FILENO);
    dup2(pipefd[1], STDOUT_FILENO);
    dup2(pipefd[1], STDERR_FILENO);
    close(pipefd[1]);

    // PENTING: set pipe read-end non-blocking SEBELUM eksekusi,
    // supaya kita bisa membaca progresif sambil command masih jalan
    // (perlu untuk early-truncation tanpa deadlock pada command yang
    // outputnya lebih besar dari pipe buffer kernel, biasanya 64KB)
    fcntl(pipefd[0], F_SETFL, O_NONBLOCK);

    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);

    // INI jalur eksekusi BASH ASLI — bukan fork+exec buatan VisiBox.
    // Builtin (cd, export, dll) jalan langsung di proses ini.
    // Command eksternal: bash sendiri yang fork, seperti perilaku normal.
    int exit_code = execute_command(req->parsed_command);  // fungsi bash native

    clock_gettime(CLOCK_MONOTONIC, &end);

    // Restore fd SEGERA setelah command selesai — sebelum baca pipe,
    // supaya VisiBox bisa nulis log/error ke stdout asli kalau perlu
    dup2(saved_stdout, STDOUT_FILENO);
    dup2(saved_stderr, STDERR_FILENO);
    close(saved_stdout);
    close(saved_stderr);

    OutputBuffer *buf = visibox_output_buffer_new();
    visibox_drain_pipe_with_limit(pipefd[0], buf, req->output_limit, req->page_mode);
    close(pipefd[0]);

    res->exit_code = exit_code;
    res->duration_ms = timespec_diff_ms(&start, &end);
    res->output = visibox_output_buffer_get_page(buf, 1, req->output_limit, req->page_mode);
    res->output_lines = buf->total_lines;
    res->output_bytes = buf->total_bytes;
    res->output_truncated = buf->truncated;

    if (buf->truncated) {
        visibox_paginator_build_metadata(buf, req, res);
    }

    visibox_response_store_add(res, buf);  // simpan buf untuk fetch_page nanti
    return 0;
}
```

**Catatan implementasi penting:** `execute_command()` adalah fungsi internal bash (nama sebenarnya bisa berbeda tergantung versi source — cek `execute_cmd.c` upstream bash, biasanya `execute_command()` atau `execute_command_internal()`). Tugas pertama di Fase 1 adalah memetakan fungsi mana persisnya di source yang dipakai, dan memverifikasi lewat eksperimen kecil bahwa `cd` di dalam pemanggilan ini benar-benar mengubah `getcwd()` proses, bukan cuma child.

### 4.3 Drain dengan limit — kebijakan diperjelas (perbaikan masalah #2)

```c
// visibox_paginator.c
void visibox_drain_pipe_with_limit(int fd, OutputBuffer *buf,
                                     size_t limit, PaginationMode mode) {
    char chunk[4096];
    ssize_t n;
    bool limit_hit = false;

    while (1) {
        n = read(fd, chunk, sizeof(chunk));
        if (n > 0) {
            if (!limit_hit) {
                visibox_output_buffer_append(buf, chunk, n);
                if (limit > 0 &&
                    ((mode == VISIBOX_PAGE_LINES && buf->total_lines >= limit) ||
                     (mode == VISIBOX_PAGE_BYTES && buf->total_bytes >= limit))) {
                    buf->truncated = true;
                    limit_hit = true;
                    // CATATAN: kita TERUS membaca fd sampai EOF untuk mencegah
                    // proses penulis (command) blok menulis ke pipe penuh,
                    // tapi BERHENTI menyimpan ke buffer (buang chunk).
                    // Ini TIDAK menghemat waktu eksekusi command — command
                    // tetap jalan sampai selesai. Ini HANYA mencegah AI
                    // menerima output raksasa. Lihat P2 di §2.
                }
            }
            // kalau limit_hit, chunk dibuang — tapi total_lines/total_bytes
            // TETAP dihitung di luar buffer kalau include_total=true di config,
            // supaya pagination metadata (total_lines) akurat
            if (limit_hit) {
                visibox_output_buffer_count_only(buf, chunk, n);
            }
        } else if (n == 0) {
            break;  // EOF, command selesai menulis
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                struct pollfd pfd = { .fd = fd, .events = POLLIN };
                poll(&pfd, 1, -1);  // tunggu data tanpa busy-wait
                continue;
            }
            break;  // error lain, stop
        }
    }
}
```

---

## 5. PROTOKOL — KONTRAK LENGKAP

Setiap request type di bawah punya: field wajib, field opsional dengan default, dan daftar error yang mungkin. Ini supaya tidak ada celah "AI kirim X, VisiBox tidak tahu harus jawab apa".

### 5.0 Error format standar (dipakai semua request type)

```json
{
  "request_id": "req_xxx",
  "response_id": "res_xxxxxxxx",
  "type": "error",
  "error_code": "ERR_SESSION_NOT_FOUND",
  "error_message": "Session sess_xxx tidak ditemukan atau sudah ditutup",
  "details": {}
}
```

Daftar kode error baku (dipakai konsisten, bukan string bebas per handler):

| Kode | Arti |
|---|---|
| `ERR_INVALID_REQUEST` | JSON tidak valid atau field wajib hilang |
| `ERR_SESSION_NOT_FOUND` | `session_id` tidak ada di registry (sudah ditutup/expired/salah) |
| `ERR_SESSION_LIMIT_REACHED` | Sudah mencapai `max_sessions`, harus tutup salah satu dulu |
| `ERR_RESPONSE_EXPIRED` | `reference_response_id` sudah di-evict dari ring buffer |
| `ERR_CURSOR_INVALID` | Cursor tidak dikenal atau sudah lewat `cursor_expiry_ms` |
| `ERR_TIMEOUT` | Operasi melewati `timeout_ms` tanpa hasil (bukan error eksekusi, hanya timeout) |
| `ERR_EXEC_FAILED` | Gagal spawn proses (PTY alloc gagal, command tidak ditemukan, dll) |
| `ERR_INTERNAL` | Bug internal VisiBox, sertakan detail untuk debug |

### 5.1 `execute`

**Request:**
```json
{
  "request_id": "req_001",
  "type": "execute",
  "command": "df -h",
  "options": {
    "output_limit": 50,
    "output_unit": "lines",
    "timeout_ms": 30000
  }
}
```

| Field | Wajib | Default | Catatan |
|---|---|---|---|
| `request_id` | tidak | auto-generate `req_auto_{n}` | |
| `command` | ya | — | dieksekusi via jalur bash asli (§4.2) |
| `options.output_limit` | tidak | dari `config.pagination.default_page_size` | `null` = tanpa limit |
| `options.output_unit` | tidak | `"lines"` | `"lines"` \| `"bytes"` |
| `options.timeout_ms` | tidak | `30000` | command yang melewati ini di-SIGKILL, response `ERR_TIMEOUT` dengan output partial yang sudah terkumpul |

**Response sukses:** (sama seperti v1, struktur tidak berubah karena sudah baik)

**Kasus timeout:** response tetap dikirim, bukan dianggap gagal total:
```json
{
  "request_id": "req_001",
  "response_id": "res_xxx",
  "type": "execute_result",
  "exit_code": null,
  "error_code": "ERR_TIMEOUT",
  "output": "...(output yang terkumpul sampai timeout)...",
  "output_truncated": true,
  "timed_out": true
}
```

### 5.2 `session_start`

**Request:**
```json
{
  "request_id": "req_010",
  "type": "session_start",
  "command": "ssh user@192.168.1.100",
  "options": {
    "env": {"TERM": "xterm-256color"},
    "cwd": "/home/user",
    "initial_read_timeout_ms": 5000,
    "output_limit": 100
  }
}
```

Field baru dibanding v1: `initial_read_timeout_ms` (terpisah dari `read_timeout_ms` default session, karena initial banner/prompt biasanya butuh waktu beda dengan read berikutnya). Kalau gagal alloc PTY atau `MAX_SESSIONS` tercapai → `ERR_SESSION_LIMIT_REACHED` atau `ERR_EXEC_FAILED`, tidak ada session yang teregister.

### 5.3 `session_input`

```json
{
  "request_id": "req_011",
  "type": "session_input",
  "session_id": "sess_9d8e7f6a",
  "input": "mypassword\n",
  "options": {
    "wait_for_prompt": true,
    "prompt_pattern": "\\$\\s*$",
    "timeout_ms": 10000,
    "output_limit": 100
  }
}
```

**Perbaikan masalah #5 (ANSI):** `prompt_pattern` dicocokkan terhadap output yang sudah di-strip ANSI escape sequence (lewat `visibox_prompt.c`). Field `output` di response tetap berisi raw output (termasuk ANSI) — supaya AI yang ingin lihat warna/formatting asli tetap bisa, tapi prompt detection tidak rapuh terhadap itu.

Kalau `session_id` tidak ditemukan → `ERR_SESSION_NOT_FOUND`, tidak ada side effect.

### 5.4 `session_read`

Sama seperti v1 — baca tanpa kirim input, untuk polling output baru (misal proses background di session yang sedang jalan lama).

### 5.5 `session_list`

Sama seperti v1, tambah field `uptime_ms` per session untuk kemudahan AI menilai session mana yang sudah lama idle dan layak ditutup.

### 5.6 `session_close`

Sama seperti v1. Tambahan: kalau `session_id` sudah closed sebelumnya (dipanggil dua kali), response bukan error — kembalikan status `already_closed: true` dengan `final_output` kosong, supaya AI yang retry karena network flaky tidak salah anggap gagal.

### 5.7 `fetch_page` / `session_fetch_page`

```json
{
  "request_id": "req_051",
  "type": "fetch_page",
  "response_id": "res_page01",
  "cursor": "cur_8f7e6d5c4b3a",
  "options": {"output_limit": 50}
}
```

**Perbaikan masalah #3 (retensi):** kalau `response_id` sudah di-evict dari ring buffer → `ERR_RESPONSE_EXPIRED`. Kalau cursor valid formatnya tapi sudah lewat `cursor_expiry_ms` → `ERR_CURSOR_INVALID`. Dua error ini dibedakan supaya AI tahu apakah masalahnya "datanya hilang" (harus jalankan command lagi) atau "cursor basi" (mungkin masih ada data, tapi harus minta page 1 lagi).

---

## 6. DATA STRUCTURES (KUNCI — DIRINGKAS DARI v1, HANYA YANG BERUBAH)

```c
// ═══════════════════════════════════════
// RESPONSE STORE — bounded ring buffer (perbaikan #3)
// ═══════════════════════════════════════
typedef struct {
    char response_id[16];
    OutputBuffer *buffer;      // pointer, BUKAN copy — owned oleh store
    time_t created_at;
    size_t approx_bytes;       // untuk hitung total_bytes_used di bawah
} StoreEntry;

typedef struct {
    StoreEntry entries[VISIBOX_STORE_MAX_ENTRIES];  // dari config, default 256
    size_t head;                 // posisi FIFO write berikutnya
    size_t count;
    size_t total_bytes_used;     // dijaga <= store_max_bytes dari config
    pthread_mutex_t lock;
} ResponseStore;

// Eviction: FIFO ketat. Saat entry baru ditambah dan
// (count == MAX_ENTRIES) ATAU (total_bytes_used + new_entry_bytes > store_max_bytes),
// entry tertua (head) di-evict dulu sebelum insert.
// fetch_page terhadap response_id yang sudah di-evict → ERR_RESPONSE_EXPIRED.

// ═══════════════════════════════════════
// EVENT LOOP — satu loop untuk semua session (perbaikan #4)
// ═══════════════════════════════════════
typedef struct {
    int epoll_fd;
    struct epoll_event events[MAX_SESSIONS];
    // map dari fd -> Session*, supaya saat epoll_wait return kita
    // tahu session mana yang punya data tanpa linear scan
    Session *fd_to_session[VISIBOX_MAX_FD];
} EventLoop;

// visibox_eventloop.c menyediakan:
//   visibox_eventloop_register(EventLoop*, Session*)    // saat session_start
//   visibox_eventloop_unregister(EventLoop*, Session*)  // saat session_close
//   visibox_eventloop_wait(EventLoop*, int timeout_ms, Session **out_ready[], int *out_count)
// Dipanggil oleh DAEMON MODE untuk melayani banyak client bersamaan tanpa
// satu thread per session. PIPE MODE (single request-response) tetap bisa
// pakai poll() sederhana satu-fd karena cuma satu session yang relevan per call.

// ═══════════════════════════════════════
// PROMPT DETECTION dengan ANSI strip (perbaikan #5)
// ═══════════════════════════════════════
// visibox_prompt.c
char *visibox_strip_ansi(const char *raw, size_t len);  // alokasi baru, caller free
bool visibox_detect_prompt(const OutputBuffer *buf, const char *pattern);
// Implementasi detect_prompt: strip ANSI dari buf->buffer dulu (ke buffer
// sementara), baru regex match. buf->buffer ASLI tidak diubah — itu yang
// dikirim ke AI di field "output".
```

Struct lain (`VisiboxIds`, `OutputBuffer`, `Session`, `VisiboxRequest`, `VisiboxResponse`) tetap seperti v1 — sudah cukup baik, tidak diulang di sini supaya dokumen ini fokus ke yang berubah.

---

## 7. CONFIG (TAMBAHAN DARI v1)

```json
{
  "pagination": {
    "mode": "lines",
    "default_page_size": 100,
    "max_page_size": 500,
    "min_page_size": 10,
    "include_total": true,
    "cursor_expiry_ms": 300000
  },
  "response_store": {
    "max_entries": 256,
    "max_total_bytes": 52428800,
    "eviction_policy": "fifo"
  },
  "sessions": {
    "max_concurrent": 16,
    "default_read_timeout_ms": 5000,
    "default_initial_read_timeout_ms": 5000,
    "idle_timeout_ms": 1800000,
    "max_output_buffer_bytes_per_session": 10485760
  },
  "execute": {
    "default_timeout_ms": 30000,
    "max_timeout_ms": 300000
  }
}
```

`sessions.idle_timeout_ms` — field baru: session yang tidak ada `session_input`/`session_read` selama ini lama akan auto-close oleh background sweeper (lihat Fase 2 di roadmap), supaya tidak ada PTY zombie menumpuk kalau AI lupa `session_close`.

---

## 8. ROADMAP (DISESUAIKAN — EVENT LOOP DIMAJUKAN KE FASE 2)

```
FASE 1: CORE EXECUTE (Minggu 1-3)
├── Fork bash + build system, identifikasi execute_command() di source
├── Eksperimen: verifikasi cd/export persist via hook di eval.c (test_execute_state.c)
├── Command ID system (request_id + response_id)
├── Output capture via fd redirect (BUKAN fork tambahan) — §4.2
├── Output pagination (lines + bytes mode) + cursor encoding
├── Response store dengan eviction policy (§6) — bukan unbounded
├── Pipe mode (echo JSON | visibox)
└── Test suite Fase 1: state persistence + pagination edge case

FASE 2: SESSIONS + EVENT LOOP (Minggu 4-7)
├── PTY session manager (openpty, login_tty)
├── Event loop (epoll) dari AWAL — bukan busy-poll yang direfactor nanti
├── session_start / session_input / session_read / session_close
├── ANSI strip + prompt detection (visibox_prompt.c)
├── Session pagination (output_limit per read)
├── Idle session sweeper (background thread, cek idle_timeout_ms)
├── Session list + status
└── Test: concurrent session (minimal 4 session paralel, verifikasi tidak ada cross-talk)

FASE 3: API & INTERFACE (Minggu 8-10)
├── REPL mode
├── Unix socket daemon mode (pakai event loop yang sudah ada dari Fase 2)
├── CLI client (visibox-cli)
└── Multi-client handling lewat daemon (event loop sudah siap, ini cuma wiring)

FASE 4: POLISH (Minggu 11-12)
├── Error handling lengkap untuk semua kode di §5.0
├── Memory limit enforcement (response_store.max_total_bytes, session buffer limit)
├── Dokumentasi protokol final (generate dari §5 di PRD ini)
└── Test suite end-to-end (skenario §9 di bawah, dijalankan sebagai integration test)
```

Catatan: Fase 3 di v1 menyebut "HTTP API mode" — dihapus dari scope v2 kecuali Anda butuh itu secara spesifik. Untuk dokumen internal solo-coding, unix socket daemon + pipe mode sudah cukup untuk integrasi dengan AI agent lokal; HTTP nambah kompleksitas (auth, TLS) yang tidak perlu di tahap ini.

---

## 9. SKENARIO END-TO-END (untuk integration test, sama kasusnya dengan v1 tapi sekarang jadi acceptance criteria)

Gunakan flow di v1 §8 (investigasi server: df, journalctl dengan pagination, ssh session, restart service, close session) sebagai **test case Fase 4**, bukan cuma ilustrasi. Tambahkan satu skenario baru yang v1 tidak punya — regression test untuk perbaikan #1:

```bash
# Skenario tambahan: verifikasi shell state persist antar execute call
{"request_id":"t1","type":"execute","command":"cd /tmp"}
→ exit_code: 0

{"request_id":"t2","type":"execute","command":"pwd"}
→ output harus "/tmp", BUKAN direktori awal proses VisiBox.
  Kalau output bukan /tmp, berarti perbaikan #1 di §4.2 gagal — STOP, jangan lanjut ke fase berikutnya.
```

---

## 10. PERTANYAAN TERBUKA (perlu diputuskan sebelum/selama Fase 1, bukan diasumsikan)

1. Versi bash source mana yang jadi basis fork? (Pengaruh ke nama fungsi internal seperti `execute_command()` — perlu dicek persis di versi yang dipilih.)
2. Apakah builtin yang mengubah file descriptor (`exec 3<>file`) perlu penanganan khusus saat fd 1/2 di-redirect sementara untuk capture? (Kemungkinan edge case di §4.2 yang belum diuji.)
3. Untuk daemon mode multi-client: apakah satu proses VisiBox melayani banyak AI agent dengan shell state terpisah per client, atau shared state? (Ini keputusan besar — kalau terpisah, butuh banyak instance proses, bukan satu proses dengan banyak koneksi soket ke satu shell yang sama.)

Pertanyaan #3 ini sebaiknya dijawab sebelum mulai Fase 3, karena akan menentukan apakah event loop di Fase 2 itu "satu shell, banyak session anak" (yang sudah didesain di sini) atau "banyak shell terpisah, masing-masing dengan event loop sendiri" (arsitektur berbeda).
