# VisiBox — PRD v3
## Shell Execution Engine untuk AI Agent
### Dokumen internal — panduan implementasi

---

## 0. RINGKASAN EKSEKUTIF

VisiBox adalah fork bash yang menambahkan 5 kapabilitas untuk dikendalikan AI agent lewat protokol JSON:

1. **ID system** — setiap operasi punya `request_id` (milik caller) dan `response_id` (milik VisiBox), untuk audit dan referensi silang.
2. **Interactive session control** — AI bisa spawn proses interaktif (ssh, python, mysql), kirim input, baca output, kapan saja, tanpa proses itu mati di antara request.
3. **Output pagination** — output besar dipotong per halaman dengan cursor, supaya AI tidak overload context window.
4. **Line numbers** — setiap baris output shell dilengkapi nomor baris (seperti code editor), memudahkan AI merujuk baris spesifik saat analisis output.
5. **Keyword-based pagination jump** — AI bisa mencari keyword di seluruh output dan langsung loncat ke halaman pagination yang mengandung keyword tersebut, tanpa harus scroll manual halaman per halaman.

Dokumen ini adalah **versi v3** yang membangun di atas v2 (rewrite). Enam masalah desain di draft v1 sudah diperbaiki di v2 — lihat §1 untuk daftar lengkap apa yang berubah dari v1. Fitur baru v3 (#4 dan #5) dijelaskan di §1.3.

---

## 1. APA YANG BERUBAH DARI VERSI SEBELUMNYA

### 1.1 Perubahan v1 → v2

| # | Masalah di v1 | Perbaikan di v2 |
|---|---|---|
| 1 | `execute` fork ulang shell baru per command → `cd`, `export` tidak persist antar request | Command non-session dieksekusi di **proses bash induk yang sama**, lewat jalur eksekusi asli bash. Shell state (cwd, env, alias) persist secara native, sama seperti shell interaktif biasa. |
| 2 | Truncation tetap drain seluruh output dari pipe → tidak menghemat compute, hanya menghemat token | Didokumentasikan eksplisit: `output_limit` adalah **proteksi token**, bukan proteksi compute. Ditambahkan rekomendasi command-level limiting (`head`, `LIMIT`) sebagai tanggung jawab AI agent, bukan VisiBox. |
| 3 | Response history ring buffer tanpa kebijakan retensi/limit memori | Ring buffer punya **kapasitas eksplisit** (jumlah entri + total bytes), kebijakan eviction (FIFO), dan response error standar (`ERR_RESPONSE_EXPIRED`) kalau `fetch_page` mengacu ke response yang sudah di-evict. |
| 4 | PTY read loop pakai busy-poll `usleep(10000)` per session → tidak scale ke concurrent session | Diganti `poll()`/`epoll` dari Fase 2 (bukan ditunda ke Fase 3). Satu event loop memantau semua PTY fd sekaligus. Tidak ada busy-wait. |
| 5 | Prompt detection regex rapuh terhadap ANSI escape code | Output PTY di-strip ANSI **sebelum** regex match (raw output tetap disimpan untuk ditampilkan ke AI). |
| 6 | Tidak jelas kapan AI harus pakai `execute` vs `session_start` | Ditambahkan aturan keputusan eksplisit di §3, dengan tabel kapan pakai yang mana. |

### 1.2 Perubahan v2 → v3 (BARU)

| # | Fitur Baru v3 | Deskripsi |
|---|---|---|
| 7 | **Line numbers di output** | Setiap baris output shell diberi nomor baris absolut (berdasarkan posisi di keseluruhan output, bukan per-halaman). AI bisa merujuk "baris 42" dan langsung tahu itu baris ke-42 dari total output. Nomor baris konsisten di semua halaman — halaman 2 tidak mulai dari 1 lagi, melainkan dari `page_size + 1`. |
| 8 | **Keyword-based pagination jump** (`search_jump`) | Request type baru yang memungkinkan AI mencari keyword di seluruh output (termasuk halaman yang belum di-fetch), dan langsung menerima halaman yang mengandung keyword beserta konteks sekitarnya. Menghilangkan kebutuhan untuk scroll halaman per halaman saat mencari sesuatu di output besar. |

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

**P6 — Nomor baris itu absolut dan konsisten.** *(BARU v3)*
Line numbers merepresentasikan posisi baris di keseluruhan output, bukan posisi relatif per halaman. Halaman 1 = baris 1–100, halaman 2 = baris 101–200, dst. Ini memungkinkan AI merujuk baris spesifik secara unik tanpa ambigu "halaman berapa, baris ke berapa".

**P7 — Pencarian harus cepat tanpa fetch seluruh output.** *(BARU v3)*
`search_jump` harus bisa mencari keyword tanpa mengirim seluruh output ke AI. Implementasi pencarian dilakukan server-side pada buffered output, bukan client-side. AI hanya menerima halaman yang relevan.

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
│   ├── visibox_linenums.c       # [BARU v3] Line number formatting & injection
│   ├── visibox_search.c         # [BARU v3] Keyword search engine pada output buffer
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
    ├── test_linenums.c          # [BARU v3] Verifikasi line numbers konsisten antar halaman
    ├── test_search_jump.c       # [BARU v3] Verifikasi keyword search & page jump
    ├── test_session_lifecycle.c
    └── test_eventloop_concurrent.c
```

### 4.2 Bagaimana `execute` TIDAK fork shell baru (perbaikan masalah #1)

Ini bagian paling penting yang berubah dari v1. Penjelasan mekanismenya:

**Yang SALAH (v1):** parent fork() → child jalankan command via `visibox_execute_original()` di proses terpisah → exit. Command seperti `cd` cuma mengubah cwd di child, hilang begitu child exit. Parent (proses VisiBox utama) cwd-nya tidak pernah berubah.

**Yang BENAR (v2+):** Hook ditempatkan di `eval.c`, **setelah** bash parse command line jadi command struct, tapi **sebelum/sesudah** bash menjalankannya lewat jalur normal (`execute_command()` bash asli). Untuk command yang sudah `fork` secara native oleh bash (kebanyakan command eksternal memang fork di bash), kita tidak menambah fork lagi — kita hanya:

1. Redirect fd 1/2 proses VisiBox **sementara** (lewat `dup2` + simpan fd asli) sebelum memanggil jalur eksekusi bash asli.
2. Panggil `execute_command()` bash asli — bash sendiri yang menentukan apakah perlu fork (command eksternal) atau tidak (builtin seperti `cd`, `export`).
3. Setelah command selesai, restore fd 1/2 ke semula.
4. Buffer yang menampung output adalah pipe yang dibaca di proses VisiBox utama sendiri (bukan child terpisah) — karena fd 1/2 sudah diarahkan ke pipe itu selama command jalan.

Konsekuensi: builtin (`cd`, `export`, `alias`, `unset`) memodifikasi state proses VisiBox utama secara langsung, persis seperti shell interaktif normal. Command eksternal tetap fork (seperti biasanya di bash), tapi itu fork yang sama yang sudah dilakukan bash secara native — bukan fork tambahan dari VisiBox.

```c
// execute_cmd.c — MODIFIED (v2+, bukan v1)
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

    // [BARU v3] Format line numbers jika diminta
    if (req->options.line_numbers) {
        visibox_linenums_inject(res->output, 1, req->output_limit, buf->total_lines);
        res->line_numbers = true;
        res->line_start = 1;
        res->line_end = visibox_min(req->output_limit, buf->total_lines);
    }

    if (buf->truncated) {
        visibox_paginator_build_metadata(buf, req, res);
    }

    visibox_response_store_add(res, buf);  // simpan buf untuk fetch_page/search_jump nanti
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

## 5. FITUR BARU v3 — DETAIL TEKNIS

### 5A. LINE NUMBERS DI OUTPUT

#### 5A.1 Konsep

Setiap baris output shell yang dikembalikan ke AI bisa dilengkapi nomor barit absolut. Nomor baris ini merepresentasikan posisi baris di **keseluruhan output** command, bukan posisi relatif di halaman saat ini.

**Contoh output `ls -la` dengan 100 file, page_size=10, line_numbers=true:**

Halaman 1:
```
  1 │ total 504
  2 │ drwxr-xr-x  2 root root  4096 Jun 25 10:00 .
  3 │ drwxr-xr-x  3 root root  4096 Jun 25 09:55 ..
  4 │ -rw-r--r--  1 root root  8192 Jun 25 10:00 file1.txt
  5 │ -rw-r--r--  1 root root  4096 Jun 25 10:00 file2.txt
  6 │ -rw-r--r--  1 root root  4096 Jun 25 10:00 file3.txt
  7 │ -rw-r--r--  1 root root  4096 Jun 25 10:00 file4.txt
  8 │ -rw-r--r--  1 root root  4096 Jun 25 10:00 file5.txt
  9 │ -rw-r--r--  1 root root  4096 Jun 25 10:00 file6.txt
 10 │ -rw-r--r--  1 root root  4096 Jun 25 10:00 file7.txt
```

Halaman 2 (`fetch_page` page=2):
```
 11 │ -rw-r--r--  1 root root  4096 Jun 25 10:00 file8.txt
 12 │ -rw-r--r--  1 root root  4096 Jun 25 10:00 file9.txt
 13 │ -rw-r--r--  1 root root  4096 Jun 25 10:00 file10.txt
 ...
 20 │ -rw-r--r--  1 root root  4096 Jun 25 10:00 file17.txt
```

**Key insight:** Baris 1-10 di halaman 1, baris 11-20 di halaman 2. AI bisa merujuk "baris 15" dan itu unik di seluruh output — tidak ambigu "halaman 2 baris 5".

#### 5A.2 Format Line Number

```
{right-aligned number, width = digit_count(total_lines)} │ {content}
```

- Pemisah default: ` │ ` (space, box-drawing vertical line U+2502, space)
- Lebar kolom nomor barit otomatis menyesuaikan jumlah digit total_lines
  - 1-9 baris → width 1
  - 10-99 baris → width 2
  - 100-999 baris → width 3
  - dst.
- Right-aligned, padded dengan spasi
- Baris kosong juga dapat nomor baris (kontennya string kosong setelah separator)

#### 5A.3 Penggunaan di Protokol

Line numbers dikontrol via field `options.line_numbers` (boolean, default: `false`).

**`execute` dengan line numbers:**
```json
{
  "request_id": "req_001",
  "type": "execute",
  "command": "find /var/log -name '*.log' -type f",
  "options": {
    "output_limit": 50,
    "output_unit": "lines",
    "line_numbers": true
  }
}
```

**Response:**
```json
{
  "request_id": "req_001",
  "response_id": "res_xxxxxxxx",
  "type": "execute_result",
  "exit_code": 0,
  "output": "  1 │ /var/log/syslog\n  2 │ /var/log/auth.log\n  3 │ /var/log/kern.log\n...",
  "output_lines": 127,
  "output_bytes": 4096,
  "output_truncated": true,
  "line_numbers": true,
  "line_start": 1,
  "line_end": 50,
  "duration_ms": 245
}
```

**`fetch_page` dengan line numbers:**
```json
{
  "request_id": "req_051",
  "type": "fetch_page",
  "response_id": "res_page01",
  "cursor": "cur_8f7e6d5c4b3a",
  "options": {
    "output_limit": 50,
    "line_numbers": true
  }
}
```

**Response `fetch_page`:**
```json
{
  "request_id": "req_051",
  "response_id": "res_page01",
  "type": "page_result",
  "page": 2,
  "cursor": "cur_next_cursor",
  "output": " 51 │ ...\n 52 │ ...\n...",
  "line_numbers": true,
  "line_start": 51,
  "line_end": 100,
  "output_lines": 127,
  "has_next": true
}
```

#### 5A.4 Implementasi: `visibox_linenums.c`

```c
// visibox_linenums.c

// Inject line numbers ke string output yang sudah jadi.
// `line_start` = nomor baris pertama di string ini (absolut, bukan 0-based)
// `total_lines` = total baris di keseluruhan output (untuk hitung width)
// Mengembalikan string baru yang dialokasikan, caller harus free().
char *visibox_linenums_inject(const char *raw_output, size_t line_start,
                               size_t page_lines, size_t total_lines);

// Helper: hitung jumlah digit yang diperlukan
int visibox_linenums_width(size_t total_lines) {
    if (total_lines == 0) return 1;
    int w = 0;
    size_t n = total_lines;
    while (n > 0) { w++; n /= 10; }
    return w;
}

// Helper: format satu baris dengan line number
// Format: {num, right-aligned, width=w} │ {content}
// Memakai separator dari config (default: " │ ")
void visibox_linenums_format_line(char *out, size_t out_size,
                                   size_t line_num, int width,
                                   const char *separator,
                                   const char *content, size_t content_len);
```

**Performa:** Line number injection dilakukan **setelah** output diambil dari buffer, bukan saat buffering. Ini berarti:
- Buffer tetap menyimpan raw output (tanpa line numbers) — hemat memori
- Line numbers dihitung on-the-fly saat response dibuat
- `search_jump` bisa mencari di raw buffer tanpa skip line numbers
- Format line numbers bisa berbeda antar request (satu client minta line numbers, client lain tidak — output yang sama dari buffer)

#### 5A.5 Edge Cases

| Kasus | Perilaku |
|---|---|
| Output kosong (0 baris) | `line_numbers: true` tapi output string kosong. `line_start: 0`, `line_end: 0`. |
| Baris terakhir tidak memiliki newline | Tetap diberi nomor baris. Line number mengikuti jumlah baris, bukan jumlah newline. |
| Output hanya 1 baris | Width = 1, format: `1 │ content` |
| Output 1000+ baris | Width = 4, format: `   1 │ content` ... `1000 │ content` |
| ANSI escape codes di output | Line numbers diletakkan SEBELUM ANSI codes per baris. Format: `  1 │ \e[31mred text\e[0m` — supaya AI tetap bisa parse ANSI. |
| `search_jump` + `line_numbers` | Output dari search_jump juga mendukung line_numbers=true, dan nomor barisnya tetap absolut. |

---

### 5B. KEYWORD-BASED PAGINATION JUMP (`search_jump`)

#### 5B.1 Konsep

Saat output command sangat besar (misalnya 10.000 baris log), AI sering perlu mencari sesuatu spesifik (error message, IP address, nama file). Tanpa `search_jump`, AI harus:
1. Fetch halaman 1, scan manual
2. Fetch halaman 2, scan manual
3. ... berulang sampai ketemu

`search_jump` menghilangkan ini dengan satu request: AI kirim keyword, VisiBox cari di seluruh buffered output, dan kembalikan halaman yang mengandung keyword beserta konteks.

#### 5B.2 Request: `search_jump`

```json
{
  "request_id": "req_060",
  "type": "search_jump",
  "response_id": "res_xxxxxxxx",
  "keyword": "ERROR",
  "options": {
    "case_sensitive": false,
    "occurrence": 1,
    "context_lines": 3,
    "output_limit": 50,
    "line_numbers": true
  }
}
```

| Field | Wajib | Default | Catatan |
|---|---|---|---|
| `response_id` | ya | — | Response ID output yang ingin dicari (harus masih ada di ring buffer) |
| `keyword` | ya | — | String yang dicari. Bisa mengandung spasi. Tidak support regex di v3 (plaintext substring match). |
| `options.case_sensitive` | tidak | `false` | Pencarian case-insensitive secara default |
| `options.occurrence` | tidak | `1` | Kejadian ke-berapa yang diinginkan. `1` = kejadian pertama, `2` = kedua, dst. `-1` = kejadian terakhir. |
| `options.context_lines` | tidak | `3` | Jumlah baris konteks sebelum dan sesudah baris yang mengandung keyword. Total baris di output = 1 (baris keyword) + 2 × context_lines. |
| `options.output_limit` | tidak | dari config `pagination.default_page_size` | Batas maksimum baris yang dikembalikan. Jika context terlalu lebar, dipotong. |
| `options.line_numbers` | tidak | `false` | Sama seperti line numbers di `execute`/`fetch_page` (§5A) |

#### 5B.3 Response: `search_jump_result`

```json
{
  "request_id": "req_060",
  "response_id": "res_xxxxxxxx",
  "type": "search_jump_result",
  "keyword": "ERROR",
  "occurrence": 1,
  "total_occurrences": 47,
  "found": true,
  "found_line": 1842,
  "line_numbers": true,
  "line_start": 1839,
  "line_end": 1845,
  "output": "1839 │ [INFO] Starting worker process 3\n1840 │ [INFO] Connected to database\n1841 │ [WARN] High memory usage detected\n1842 │ [ERROR] Connection timeout to replica-3\n1843 │ [INFO] Retrying connection (attempt 1/3)\n1844 │ [INFO] Connected to replica-3\n1845 │ [INFO] Worker process 3 ready",
  "output_truncated": false,
  "page_hint": 37,
  "cursor": "cur_page_37_start"
}
```

| Field | Arti |
|---|---|
| `found` | `true` jika keyword ditemukan, `false` jika tidak ada kejadian. |
| `total_occurrences` | Total jumlah baris yang mengandung keyword di seluruh output. Berguna buat AI menilai seberapa sering error muncul. |
| `found_line` | Nomor baris absolut (1-based) di mana keyword ditemukan pada occurrence yang diminta. |
| `line_start` / `line_end` | Range baris yang dikembalikan (found_line ± context_lines, dibatasi output_limit). |
| `output` | Baris-baris konteks dengan keyword di tengahnya. Jika `line_numbers: true`, diformat sesuai §5A. |
| `page_hint` | Nomor halaman yang mengandung `found_line` (berdasarkan page_size). AI bisa langsung `fetch_page` ke halaman ini kalau butuh lebih banyak konteks. |
| `cursor` | Cursor ke awal halaman `page_hint`. AI bisa pakai ini di `fetch_page` untuk langsung loncat. |

#### 5B.4 Kasus `found: false`

```json
{
  "request_id": "req_061",
  "response_id": "res_xxxxxxxx",
  "type": "search_jump_result",
  "keyword": "CRITICAL",
  "occurrence": 1,
  "total_occurrences": 0,
  "found": false,
  "output": "",
  "output_truncated": false
}
```

Tidak ada error — ini bukan error, keyword memang tidak ada di output. AI bisa lanjut coba keyword lain.

#### 5B.5 Kasus occurrence melebihi total_occurrences

```json
{
  "request_id": "req_062",
  "response_id": "res_xxxxxxxx",
  "type": "search_jump_result",
  "keyword": "ERROR",
  "occurrence": 50,
  "total_occurrences": 47,
  "found": false,
  "output": "",
  "output_truncated": false
}
```

Sama seperti tidak ditemukan — `found: false`. AI tahu dari `total_occurrences` bahwa ada 47 kejadian, tapi occurrence ke-50 tidak ada.

#### 5B.6 Error Cases

| Kondisi | Error Code |
|---|---|
| `response_id` sudah di-evict dari ring buffer | `ERR_RESPONSE_EXPIRED` |
| `keyword` kosong string | `ERR_INVALID_REQUEST` |
| `occurrence` = 0 | `ERR_INVALID_REQUEST` (occurrence 1-based) |

#### 5B.7 Implementasi: `visibox_search.c`

```c
// visibox_search.c

// Hasil pencarian
typedef struct {
    bool found;
    size_t found_line;          // 1-based, nomor baris absolut
    size_t total_occurrences;
} SearchResult;

// Cari keyword di seluruh output buffer.
// Mengembalikan SearchResult yang berisi posisi occurrence ke-N.
// Pencarian dilakukan di raw buffer (tanpa line numbers).
SearchResult visibox_search_keyword(const OutputBuffer *buf,
                                     const char *keyword,
                                     bool case_sensitive,
                                     int occurrence);

// Ambil baris-baris sekitar found_line sebagai string.
// line_start dan line_end di-set ke range absolut yang diambil.
// Jika line_numbers=true, output sudah diformat dengan line numbers.
// Mengembalikan string baru, caller free().
char *visibox_search_get_context(const OutputBuffer *buf,
                                  size_t found_line,
                                  int context_lines,
                                  bool line_numbers,
                                  size_t output_limit,
                                  size_t *out_line_start,
                                  size_t *out_line_end);
```

**Algoritma pencarian:**

```
1. Akses raw buffer dari OutputBuffer (ter-index per baris)
2. Iterasi semua baris, lakukan substring match (case-sensitive atau tidak)
3. Setiap match, increment counter
4. Ketika counter == occurrence (atau -1 untuk last), catat found_line
5. Return SearchResult
```

**Kompleksitas:** O(N × M) dimana N = jumlah baris, M = panjang keyword. Untuk output 100K baris dengan keyword pendek, ini berjalan dalam hitungan milidetik. Tidak perlu index khusus — linear scan cukup karena pencarian hanya dilakukan sekali per request, bukan di hot path.

**Optimasi (jika diperlukan di kemudian hari, bukan di Fase 1):**
- Boyer-Moore atau memchr untuk keyword pendek
- Line index (array of offsets) yang sudah ada di OutputBuffer — akses baris ke-N adalah O(1)

#### 5B.8 Hubungan `search_jump` ↔ `fetch_page`

`search_jump` dan `fetch_page` saling melengkapi:

```
AI menerima output 10.000 baris (truncated)
  ↓
AI ingin cari "ERROR" → search_jump
  ↓
Response: found_line=1842, page_hint=37, cursor=cur_xxx
  ↓
AI ingin lihat lebih banyak konteks → fetch_page(cursor=cur_xxx)
  ↓
AI mendapat halaman 37 (baris 1801-1900) dengan line_numbers
  ↓
AI merujuk baris spesifik: "baris 1842 menunjukkan ERROR pada replica-3"
```

Flow ini memungkinkan AI:
1. Cepat menemukan apa yang dicari tanpa scroll manual
2. Loncat ke halaman terkait untuk analisis lebih dalam
3. Merujuk baris spesifik dengan nomor absolut yang unik

---

## 6. PROTOKOL — KONTRAK LENGKAP

Setiap request type di bawah punya: field wajib, field opsional dengan default, dan daftar error yang mungkin. Ini supaya tidak ada celah "AI kirim X, VisiBox tidak tahu harus jawab apa".

### 6.0 Error format standar (dipakai semua request type)

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
| `ERR_SEARCH_KEYWORD_EMPTY` | `search_jump` dengan keyword kosong (v3) |
| `ERR_INTERNAL` | Bug internal VisiBox, sertakan detail untuk debug |

### 6.1 `execute`

**Request:**
```json
{
  "request_id": "req_001",
  "type": "execute",
  "command": "df -h",
  "options": {
    "output_limit": 50,
    "output_unit": "lines",
    "timeout_ms": 30000,
    "line_numbers": true
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
| `options.line_numbers` | tidak | `false` | *[BARU v3]* Tambahkan nomor barit absolut ke setiap baris output |

**Response sukses:**
```json
{
  "request_id": "req_001",
  "response_id": "res_xxxxxxxx",
  "type": "execute_result",
  "exit_code": 0,
  "output": "  1 │ Filesystem      Size  Used Avail Use% Mounted on\n  2 │ /dev/sda1       50G   28G   20G  59% /",
  "output_lines": 8,
  "output_bytes": 512,
  "output_truncated": false,
  "line_numbers": true,
  "line_start": 1,
  "line_end": 8,
  "duration_ms": 12
}
```

**Response tanpa line numbers** (default, backward compatible):
```json
{
  "request_id": "req_001",
  "response_id": "res_xxxxxxxx",
  "type": "execute_result",
  "exit_code": 0,
  "output": "Filesystem      Size  Used Avail Use% Mounted on\n/dev/sda1       50G   28G   20G  59% /",
  "output_lines": 8,
  "output_bytes": 512,
  "output_truncated": false,
  "line_numbers": false,
  "duration_ms": 12
}
```

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

### 6.2 `session_start`

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

Field `initial_read_timeout_ms` (terpisah dari `read_timeout_ms` default session, karena initial banner/prompt biasanya butuh waktu beda dengan read berikutnya). Kalau gagal alloc PTY atau `MAX_SESSIONS` tercapai → `ERR_SESSION_LIMIT_REACHED` atau `ERR_EXEC_FAILED`, tidak ada session yang teregister.

### 6.3 `session_input`

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
    "output_limit": 100,
    "line_numbers": true
  }
}
```

`prompt_pattern` dicocokkan terhadap output yang sudah di-strip ANSI escape sequence (lewat `visibox_prompt.c`). Field `output` di response tetap berisi raw output (termasuk ANSI). `line_numbers` *[BARU v3]* bekerja sama seperti di `execute`.

Kalau `session_id` tidak ditemukan → `ERR_SESSION_NOT_FOUND`, tidak ada side effect.

### 6.4 `session_read`

Sama seperti v1 — baca tanpa kirim input, untuk polling output baru (misal proses background di session yang sedang jalan lama). Mendukung `line_numbers` *[BARU v3]*.

### 6.5 `session_list`

Sama seperti v1, tambah field `uptime_ms` per session untuk kemudahan AI menilai session mana yang sudah lama idle dan layak ditutup.

### 6.6 `session_close`

Sama seperti v1. Tambahan: kalau `session_id` sudah closed sebelumnya (dipanggil dua kali), response bukan error — kembalikan status `already_closed: true` dengan `final_output` kosong, supaya AI yang retry karena network flaky tidak salah anggap gagal.

### 6.7 `fetch_page` / `session_fetch_page`

```json
{
  "request_id": "req_051",
  "type": "fetch_page",
  "response_id": "res_page01",
  "cursor": "cur_8f7e6d5c4b3a",
  "options": {
    "output_limit": 50,
    "line_numbers": true
  }
}
```

Kalau `response_id` sudah di-evict dari ring buffer → `ERR_RESPONSE_EXPIRED`. Kalau cursor valid formatnya tapi sudah lewat `cursor_expiry_ms` → `ERR_CURSOR_INVALID`.

### 6.8 `search_jump` *(BARU v3)*

Lihat detail lengkap di §5B.2 dan §5B.3.

Ringkasan kontrak:
- **Wajib:** `response_id`, `keyword`
- **Opsional:** `options.case_sensitive`, `options.occurrence`, `options.context_lines`, `options.output_limit`, `options.line_numbers`
- **Error:** `ERR_RESPONSE_EXPIRED`, `ERR_INVALID_REQUEST` (keyword kosong / occurrence=0)
- **Bukan error:** `found: false` — keyword memang tidak ada, AI lanjut coba keyword lain

---

## 7. DATA STRUCTURES

### 7.1 Dari v2 (tidak berubah, diringkas)

```c
// ═══════════════════════════════════════
// RESPONSE STORE — bounded ring buffer
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
// fetch_page/search_jump terhadap response_id yang sudah di-evict → ERR_RESPONSE_EXPIRED.

// ═══════════════════════════════════════
// EVENT LOOP — satu loop untuk semua session
// ═══════════════════════════════════════
typedef struct {
    int epoll_fd;
    struct epoll_event events[MAX_SESSIONS];
    Session *fd_to_session[VISIBOX_MAX_FD];
} EventLoop;

// ═══════════════════════════════════════
// PROMPT DETECTION dengan ANSI strip
// ═══════════════════════════════════════
char *visibox_strip_ansi(const char *raw, size_t len);
bool visibox_detect_prompt(const OutputBuffer *buf, const char *pattern);
```

### 7.2 Baru di v3

```c
// ═══════════════════════════════════════
// LINE NUMBERS (visibox_linenums.c)
// ═══════════════════════════════════════

// Format separator line number (default: " │ ")
#define VISIBOX_LINENUM_SEP_DEFAULT  " \xe2\x94\x82 "  // UTF-8: │ (U+2502)

typedef struct {
    char separator[16];         // default: " │ "
    bool enabled;               // global default dari config
} LineNumberConfig;

// ═══════════════════════════════════════
// SEARCH (visibox_search.c)
// ═══════════════════════════════════════

typedef struct {
    bool found;
    size_t found_line;          // 1-based, nomor baris absolut di seluruh output
    size_t total_occurrences;   // total baris yang mengandung keyword
} VisiboxSearchResult;

// ═══════════════════════════════════════
// OUTPUT BUFFER — ditambah field untuk line index
// ═══════════════════════════════════════
// OutputBuffer dari v2 sudah menyimpan data per-baris.
// Untuk v3, ditambahkan line index (array of {offset, length} per baris)
// supaya akses baris ke-N adalah O(1) — penting untuk search_jump.

typedef struct {
    size_t offset;    // byte offset di buffer utama
    size_t length;    // panjang baris (termasuk \n kalau ada)
} LineEntry;

// Di dalam OutputBuffer:
//   LineEntry *lines;         // array, di-alloc setelah semua data terkumpul
//   size_t line_count;        // = total_lines
//   bool line_index_built;    // lazily built saat pertama kali search_jump/fetch_page dipanggil
```

**Mengapa line index perlu di-build lazily:** Saat drain pipe, kita tidak tahu berapa total baris sampai selesai. Line index di-build setelah drain selesai, sebelum buffer dimasukkan ke response store. Ini O(N) sekali, lalu setiap akses baris ke-N adalah O(1).

### 7.3 Response struct — ditambah field v3

```c
// VisiboxResponse — field baru v3
typedef struct {
    // ... (field v2 tidak berubah) ...

    // [BARU v3] Line number metadata
    bool line_numbers;          // apakah output saat ini berisi line numbers
    size_t line_start;          // nomor baris pertama di output ini (absolut)
    size_t line_end;            // nomor baris terakhir di output ini (absolut)
} VisiboxResponse;
```

---

## 8. CONFIG (UPDATE v3)

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
  "line_numbers": {
    "default_enabled": false,
    "separator": " │ ",
    "max_line_number_width": 6
  },
  "search": {
    "default_context_lines": 3,
    "max_context_lines": 50,
    "max_keyword_length": 256,
    "case_sensitive_default": false
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

Field baru v3:
- `line_numbers.default_enabled` — global default, bisa di-override per-request via `options.line_numbers`
- `line_numbers.separator` — string pemisah antara nomor barit dan konten (default: `" │ "`)
- `line_numbers.max_line_number_width` — batas maksimum lebar kolom nomor barit (untuk output yang sangat panjang, tidak lebih dari 6 digit)
- `search.default_context_lines` — default jumlah baris konteks di `search_jump`
- `search.max_context_lines` — batas maksimum context_lines (cegah AI minta 1000 konteks)
- `search.max_keyword_length` — batas panjang keyword (cegah abuse)
- `search.case_sensitive_default` — default case sensitivity untuk search

---

## 9. ROADMAP (UPDATE v3)

```
FASE 1: CORE EXECUTE + LINE NUMBERS + SEARCH (Minggu 1-4)
├── Fork bash + build system, identifikasi execute_command() di source
├── Eksperimen: verifikasi cd/export persist via hook di eval.c (test_execute_state.c)
├── Command ID system (request_id + response_id)
├── Output capture via fd redirect (BUKAN fork tambahan) — §4.2
├── Output pagination (lines + bytes mode) + cursor encoding
├── Line number formatting & injection (visibox_linenums.c) — §5A
├── Line index pada OutputBuffer (LineEntry array, lazy build) — §7.2
├── Keyword search engine (visibox_search.c) + search_jump request — §5B
├── Response store dengan eviction policy (§7) — bukan unbounded
├── Pipe mode (echo JSON | visibox)
├── Test suite Fase 1:
│   ├── test_execute_state.c (regression P1)
│   ├── test_pagination.c
│   ├── test_linenums.c (konsistensi nomor barit antar halaman)
│   └── test_search_jump.c (pencarian, page_hint, context)
└── CLI client: support line_numbers + search_jump

FASE 2: SESSIONS + EVENT LOOP (Minggu 5-8)
├── PTY session manager (openpty, login_tty)
├── Event loop (epoll) dari AWAL — bukan busy-poll yang direfactor nanti
├── session_start / session_input / session_read / session_close
├── ANSI strip + prompt detection (visibox_prompt.c)
├── Session pagination (output_limit per read) + line_numbers support
├── Session search_jump (cari keyword di output session yang terbuffer)
├── Idle session sweeper (background thread, cek idle_timeout_ms)
├── Session list + status
└── Test: concurrent session (minimal 4 session paralel, verifikasi tidak ada cross-talk)

FASE 3: API & INTERFACE (Minggu 9-11)
├── REPL mode
├── Unix socket daemon mode (pakai event loop yang sudah ada dari Fase 2)
├── CLI client (visibox-cli) dengan line_numbers + search_jump support
└── Multi-client handling lewat daemon (event loop sudah siap, ini cuma wiring)

FASE 4: POLISH (Minggu 12-14)
├── Error handling lengkap untuk semua kode di §6.0
├── Memory limit enforcement (response_store.max_total_bytes, session buffer limit)
├── Search performance testing (output 100K+ baris)
├── Line number edge cases (ANSI, empty lines, very long lines)
├── Dokumentasi protokol final (generate dari §6 di PRD ini)
└── Test suite end-to-end (skenario §10 di bawah, dijalankan sebagai integration test)
```

Catatan: Fase 1 diperluas 1 minggu (dari 3 ke 4) untuk mengakomodasi implementasi line numbers dan search_jump. Fitur-fitur ini ditempatkan di Fase 1 karena mereka beroperasi pada output buffer yang sudah ada — tidak perlu session/PTY infrastructure.

---

## 10. SKENARIO END-TO-END (untuk integration test)

Skenario dasar sama dengan v2 §9 (investigasi server: df, journalctl dengan pagination, ssh session, restart service, close session) sebagai test case Fase 4.

### 10.1 Regression test v2: shell state persist

```bash
{"request_id":"t1","type":"execute","command":"cd /tmp"}
→ exit_code: 0

{"request_id":"t2","type":"execute","command":"pwd"}
→ output harus "/tmp", BUKAN direktori awal proses VisiBox.
  Kalau output bukan /tmp, berarti perbaikan #1 di §4.2 gagal — STOP, jangan lanjut ke fase berikutnya.
```

### 10.2 Test baru v3: line numbers konsistensi

```bash
# Step 1: Execute dengan line_numbers, output 25 baris, page_size=10
{"request_id":"t3","type":"execute","command":"seq 1 25","options":{"output_limit":10,"line_numbers":true}}
→ line_start: 1, line_end: 10
→ baris pertama: "  1 │ 1"
→ baris terakhir: " 10 │ 10"

# Step 2: Fetch page 2
{"request_id":"t4","type":"fetch_page","response_id":"res_t3","cursor":"cur_xxx","options":{"line_numbers":true}}
→ line_start: 11, line_end: 20
→ baris pertama: " 11 │ 11"
→ baris terakhir: " 20 │ 20"

# Step 3: Fetch page 3
{"request_id":"t5","type":"fetch_page","response_id":"res_t3","cursor":"cur_yyy","options":{"line_numbers":true}}
→ line_start: 21, line_end: 25
→ baris pertama: " 21 │ 21"
→ baris terakhir: " 25 │ 25"
```

**Assert:** line_start halaman N = (N-1) × page_size + 1. Nomor barit TIDAK reset per halaman.

### 10.3 Test baru v3: search_jump akurasi

```bash
# Step 1: Generate output besar
{"request_id":"t6","type":"execute","command":"seq 1 1000"}
→ output_lines: 1000, output_truncated: true

# Step 2: Cari keyword "500"
{"request_id":"t7","type":"search_jump","response_id":"res_t6","keyword":"500","options":{"line_numbers":true}}
→ found: true
→ found_line: 500
→ total_occurrences: 1
→ output berisi: "497 │ 497\n498 │ 498\n499 │ 499\n500 │ 500\n501 │ 501\n502 │ 502\n503 │ 503"
→ page_hint: 5 (dengan page_size=100, baris 500 ada di halaman 5)
→ cursor valid untuk fetch_page ke halaman 5

# Step 3: Cari keyword yang tidak ada
{"request_id":"t8","type":"search_jump","response_id":"res_t6","keyword":"2000"}
→ found: false
→ total_occurrences: 0

# Step 4: Cari dengan occurrence > total
{"request_id":"t9","type":"search_jump","response_id":"res_t6","keyword":"5","occurrence":300}
→ found: false
→ total_occurrences: 100 (ada "5", "15", "25", ..., "995")
```

### 10.4 Test baru v3: search_jump + fetch_page workflow

```bash
# AI workflow: cari error di log, loncat ke halaman, analisis
{"type":"execute","command":"journalctl -n 5000 --no-pager","options":{"output_limit":100,"line_numbers":true}}
→ 5000 baris, 50 halaman

{"type":"search_jump","response_id":"res_above","keyword":"segfault"}
→ found_line: 2341, page_hint: 24, cursor: cur_xxx

{"type":"fetch_page","response_id":"res_above","cursor":"cur_xxx","options":{"line_numbers":true}}
→ Mendapat halaman 24 (baris 2301-2400) untuk analisis detail
→ AI bisa merujuk: "baris 2341 menunjukkan segfault di process nginx"
```

---

## 11. PERTANYAAN TERBUKA (perlu diputuskan sebelum/selama Fase 1, bukan diasumsikan)

1. Versi bash source mana yang jadi basis fork? (Pengaruh ke nama fungsi internal seperti `execute_command()` — perlu dicek persis di versi yang dipilih.) → **Sudah diputuskan: GNU Bash 5.3-p15**
2. Apakah builtin yang mengubah file descriptor (`exec 3<>file`) perlu penanganan khusus saat fd 1/2 di-redirect sementara untuk capture? (Kemungkinan edge case di §4.2 yang belum diuji.)
3. Untuk daemon mode multi-client: apakah satu proses VisiBox melayani banyak AI agent dengan shell state terpisah per client, atau shared state? (Ini keputusan besar — kalau terpisah, butuh banyak instance proses, bukan satu proses dengan banyak koneksi soket ke satu shell yang sama.)
4. *[BARU v3]* Apakah `search_jump` perlu mendukung regex atau cukup plaintext substring match? (PRD ini menspesifikasikan plaintext substring untuk v3, regex bisa ditambahkan di versi berikutnya jika ada permintaan.)
5. *[BARU v3]* Apakah line numbers perlu mendukung mode "gutter only" (hanya nomor barit, tanpa separator dan content bergabung) untuk bandwidth efficiency? (Saat ini nomor barit di-inject ke dalam string output — tidak ada mode terpisah.)