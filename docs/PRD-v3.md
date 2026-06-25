# VisiBox — PRD v3
## Shell Execution Engine untuk AI Agent
### Dokumen internal — panduan implementasi

---

## 0. RINGKASAN EKSEKUTIF

VisiBox adalah fork bash yang menambahkan 5 kapabilitas untuk dikendalikan AI agent lewat protokol JSON:

1. **ID system** — setiap operasi punya `request_id` (milik caller) dan `response_id` (milik VisiBox), untuk audit dan referensi silang.
2. **Interactive session control** — AI bisa spawn proses interaktif (ssh, python, mysql), kirim input, baca output, kapan saja, tanpa proses itu mati di antara request.
3. **Output pagination** — output besar dipotong per halaman dengan cursor, supaya AI tidak overload context window.
4. **Line numbers** — setiap baris output diberi nomor baris global (1-indexed), konsisten lintas halaman. AI bisa mereferensikan baris spesifik secara presisi, seperti editor kode.
5. **Keyword search & jump** — AI bisa mencari keyword di seluruh output buffer, melihat baris mana yang match beserta nomor halamannya, lalu langsung jump ke halaman tersebut tanpa paging manual satu-satu.

Dokumen ini adalah **v3** — evolusi dari v2. Semua perbaikan v2 terhadap v1 tetap berlaku (lihat §1). Dua fitur baru di v3 dijelaskan di §1 bagian bawah, dengan desain lengkap di §5.8–§5.10 dan §6.

---

## 1. APA YANG BERUBAH DARI v2 (DAN KENAPA)

### 1.1 Perbaikan v1 → v2 (tetap berlaku)

| # | Masalah di v1 | Perbaikan di v2 |
|---|---|---|
| 1 | `execute` fork ulang shell baru per command → `cd`, `export` tidak persist | Command dieksekusi di proses bash induk yang sama via jalur asli. Shell state persist native. |
| 2 | Truncation tidak menghemat compute | `output_limit` adalah proteksi token, bukan proteksi compute. Didokumentasikan eksplisit. |
| 3 | Ring buffer tanpa kebijakan retensi | Bounded ring buffer: kapasitas eksplisit, FIFO eviction, `ERR_RESPONSE_EXPIRED`. |
| 4 | PTY read loop busy-poll | `poll()`/`epoll` dari Fase 2. Satu event loop untuk semua session fd. |
| 5 | Prompt detection rapuh terhadap ANSI | ANSI di-strip sebelum regex match. Raw output tetap utuh. |
| 6 | Tidak jelas kapan pakai `execute` vs `session_start` | Tabel keputusan eksplisit di §3. |

### 1.2 Penambahan v2 → v3

| # | Fitur Baru | Motivasi |
|---|---|---|
| 7 | **Line numbers di setiap baris output** | AI agent sering perlu mereferensikan baris spesifik — "baris 42 menunjukkan error permission denied". Tanpa line number, AI harus menghitung manual (rawan salah) atau mengirim seluruh output (boros token). Dengan line number, AI bisa bilang "cek baris 42" dan user/agent lain tahu persis mana. Mirip editor kode (VS Code, JetBrains). |
| 8 | **Keyword search dengan jump-to-page** | Output besar (misal `dmesg` 50K baris) yang sudah dipaginasi sulit dinavigasi. Kalau AI ingin mencari "OOM killer", ia harus fetch page satu-satu — sangat tidak efisien. Dengan search, AI kirim keyword, VisiBox kembali daftar baris yang match + nomor halamannya, dan AI langsung jump ke halaman relevan. |

---

## 2. PRINSIP DESAIN

Semua prinsip v2 (P1–P5) tetap berlaku. Dua prinsip baru untuk v3:

**P1–P5:** Tidak berubah dari v2 (lihat PRD v2 §2):
- P1: Shell state itu sakral.
- P2: Pagination melindungi token, bukan compute.
- P3: Tidak ada busy-wait di hot path.
- P4: Semua resource punya batas eksplisit dan kebijakan eviction.
- P5: Ambiguitas protokol = bug spec.

**P6 — Line numbers selalu global, bukan per-halaman.**
Nomor baris harus konsisten di seluruh output. Baris pertama selalu 1, baris terakhir = `total_lines`. Page 2 tidak mulai dari 1 lagi — melanjutkan dari page sebelumnya. Ini memungkinkan AI dan manusia mereferensikan baris secara unik tanpa perlu tahu di halaman mana baris itu berada.

**P7 — Search adalah first-class operation, bukan afterthought.**
`search` bukan opsi di dalam `fetch_page` — ia adalah request type sendiri dengan response type sendiri. Search bekerja pada **seluruh output buffer** yang tersimpan (bukan hanya halaman yang sudah dikirim ke AI). Ini memungkinkan AI menemukan sesuatu di halaman 17 tanpa fetch 16 halaman sebelumnya.

---

## 3. KAPAN PAKAI `execute` VS `session_start`

Tidak berubah dari v2. Aturan ringkas: **kalau command akan exit dan tidak butuh input tambahan, pakai `execute`. Selain itu, `session_start`.**

Lihat PRD v2 §3 untuk tabel keputusan lengkap.

---

## 4. ARSITEKTUR

### 4.1 Struktur File

```
visibox/
├── (bash source, fork dari upstream GNU bash)
│
├── shell/
│   ├── visibox_core.c           # Entry point, mode switcher (pipe/daemon/repl)
│   ├── visibox_id.c             # ID generator & validasi
│   ├── visibox_session.c        # Session manager: lifecycle, registry
│   ├── visibox_session_pty.c    # PTY spawn (openpty, login_tty, fork+exec)
│   ├── visibox_eventloop.c      # epoll/poll multiplexer
│   ├── visibox_prompt.c         # ANSI strip + regex prompt detection
│   ├── visibox_paginator.c      # Output buffer + pagination + cursor encoding
│   ├── visibox_linenums.c       # [BARU v3] Line number formatting engine
│   ├── visibox_search.c         # [BARU v3] Keyword search + jump-to-page
│   ├── visibox_store.c          # Response history ring buffer (bounded, evicting)
│   ├── visibox_protocol.c       # JSON parse/serialize, request validation
│   ├── visibox_dispatch.c       # Routing: request type → handler
│   │
│   ├── execute_cmd.c            # MODIFIED: hook output capture ke jalur asli
│   ├── eval.c                   # MODIFIED: dispatch hook setelah parse
│   └── input.c                  # MODIFIED: baca dari stdin/socket sesuai mode
│
├── include/
│   └── visibox.h                # Semua struct & deklarasi
│
├── config/
│   └── visibox.conf
│
├── client/
│   ├── visibox-cli
│   └── libvisibox.h
│
└── tests/
    ├── test_execute_state.c
    ├── test_pagination.c
    ├── test_linenums.c          # [BARU v3] Line number konsistensi antar page
    ├── test_search_jump.c       # [BARU v3] Search akurasi + jump ke page benar
    ├── test_session_lifecycle.c
    └── test_eventloop_concurrent.c
```

### 4.2 `execute` — TIDAK fork shell baru

Tidak berubah dari v2. Lihat PRD v2 §4.2 untuk mekanisme `dup2` + `execute_command()` bash asli.

### 4.3 Drain dengan limit

Tidak berubah dari v2. Lihat PRD v2 §4.3.

### 4.4 [BARU v3] Line Number Formatting Engine

Line numbers ditambahkan saat **rendering** ke response JSON, bukan saat menyimpan ke buffer. Buffer mentah tetap berisi output asli tanpa prefix, supaya search dan fetch_page bekerja pada data bersih.

```c
// visibox_linenums.c

// Format satu baris output dengan line number prefix.
// start_line: nomor baris pertama di halaman ini (1-indexed, global)
// width: jumlah digit padding (dihitung dari total_lines)
// format: "  %4d | %s" → "   42 | Filesystem Size Used..."
char *visibox_format_line_numbered(size_t line_num, const char *line_content, int width);

// Render seluruh halaman output dengan line numbers.
// Contoh output:
//      1 | Filesystem      Size  Used Avail Use% Mounted on
//      2 | /dev/sda1        50G   35G   13G  73% /
//    101 | (awal halaman 2 — line number LANJUT, bukan reset ke 1)
char *visibox_render_page_linenums(OutputBuffer *buf, int page_num,
                                    size_t page_size, PaginationMode mode);

// Hitung jumlah digit untuk padding
// total_lines=50 → width=2, total_lines=847 → width=3, total_lines=10000 → width=5
int visibox_linenums_width(size_t total_lines);
```

**Kenapa format `   42 | ...` (pipe separator)?**
- Separator ` | ` memudahkan parsing — AI/tool bisa split pada ` | ` untuk mendapatkan line number dan content terpisah.
- Right-aligned padding memastikan content selalu start di kolom yang sama.
- Format konsisten dengan `cat -n`, `less -N`, dan editor kode.

**Contoh response dengan line numbers:**
```json
{
  "type": "execute_result",
  "output": "     1 | Filesystem      Size  Used Avail Use% Mounted on\n     2 | /dev/sda1        50G   35G   13G  73% /\n     3 | tmpfs           3.9G     0  3.9G   0% /dev/shm",
  "output_lines": 3,
  "line_number_start": 1,
  "line_number_end": 3
}
```

**Contoh page 2 (paginated):**
```json
{
  "type": "page_result",
  "output": "   101 | [4.0] ACPI BIOS Error...\n   102 | [4.1] ata1.00: failed...",
  "line_number_start": 101,
  "line_number_end": 102,
  "pagination": { "current_page": 2, "total_lines": 847, "total_pages": 9, "has_next": true, "next_cursor": "cur_abc" }
}
```

### 4.5 [BARU v3] Keyword Search Engine

Search bekerja pada **buffer mentah** (tanpa line number prefix), bukan pada output yang sudah di-format.

```c
// visibox_search.c

typedef struct {
    size_t line_number;        // nomor baris global (1-indexed)
    size_t page_number;        // halaman mana baris ini berada
    size_t match_column_start; // posisi kolom awal keyword (0-indexed)
    size_t match_column_end;   // posisi kolom akhir keyword
    char *line_preview;        // preview baris (truncated jika panjang)
} SearchResult;

typedef struct {
    SearchResult *results;
    size_t match_count;        // total match di seluruh output
    size_t results_returned;   // jumlah yang dikembalikan
    size_t *pages_hit;         // sorted unique array of page numbers
    size_t pages_hit_count;
    bool truncated;            // true jika match_count > max_results
} SearchResults;

SearchResults *visibox_search(OutputBuffer *buf, const char *keyword,
                               bool case_sensitive, bool regex_mode,
                               size_t max_results);
void visibox_search_results_free(SearchResults *results);
```

**Algoritma:** Linear scan line by line via `line_index`. Untuk setiap line, cek keyword (substring atau regex). Hitung `page_number = (line_number - 1) / page_size + 1`. Tidak ada inverted index — linear scan cukup untuk skala VisiBox (< 10ms untuk 100K baris).

---

## 5. PROTOKOL — KONTRAK LENGKAP

### 5.0 Error format standar

Error codes v2 tetap berlaku, ditambah:

| Kode | Arti |
|---|---|
| `ERR_SEARCH_PATTERN_INVALID` | Regex pattern tidak valid (hanya saat `regex: true`) |
| `ERR_PAGE_OUT_OF_RANGE` | `page_number` di luar jangkauan (lebih besar dari `total_pages`) |

### 5.1 `execute` — Response ditambah line numbers

Request: sama dengan v2.

Response sukses (v3 — tambahan field baru):
```json
{
  "request_id": "req_001",
  "response_id": "res_a7f3c2d1",
  "type": "execute_result",
  "exit_code": 0,
  "duration_ms": 45,
  "output": "     1 | Filesystem      Size  Used Avail Use% Mounted on\n     2 | /dev/sda1        50G   35G   13G  73% /",
  "output_lines": 2,
  "output_bytes": 98,
  "output_truncated": false,
  "line_number_start": 1,
  "line_number_end": 2,
  "pagination": null
}
```

| Field Baru | Tipe | Keterangan |
|---|---|---|
| `line_number_start` | `int` | Nomor baris pertama di output ini (selalu hadir) |
| `line_number_end` | `int` | Nomor baris terakhir di output ini (selalu hadir) |

### 5.2–5.6 Session requests — TIDAK BERUBAH dari v2

Lihat PRD v2 §5.2–§5.6. Semua session response juga mendapat `line_number_start` dan `line_number_end`.

### 5.7 `fetch_page` / `session_fetch_page`

Sama seperti v2 + `line_number_start` / `line_number_end` di response. Lihat contoh di §4.4.

### 5.8 [BARU v3] `search`

Mencari keyword di seluruh output buffer sebuah response. Operasi read-only — tidak mengubah cursor pagination.

**Request:**
```json
{
  "request_id": "req_100",
  "type": "search",
  "response_id": "res_page01",
  "keyword": "OOM killer",
  "options": {
    "case_sensitive": false,
    "regex": false,
    "max_results": 20
  }
}
```

| Field | Wajib | Default | Catatan |
|---|---|---|---|
| `response_id` | ya | — | Response mana yang output-nya dicari |
| `keyword` | ya | — | String dicari. Jika `regex: true`, ini pattern regex. |
| `options.case_sensitive` | tidak | `false` | Default case-insensitive — lebih praktis untuk AI |
| `options.regex` | tidak | `false` | `false` = literal substring, `true` = regex via `regcomp()` |
| `options.max_results` | tidak | `50` | Maks match yang dikembalikan. `0` = tanpa limit. |

**Response sukses:**
```json
{
  "request_id": "req_100",
  "response_id": "res_search01",
  "type": "search_result",
  "reference_response_id": "res_page01",
  "keyword": "OOM killer",
  "match_count": 3,
  "results_returned": 3,
  "results_truncated": false,
  "pages_hit": [5, 12, 14],
  "matches": [
    {
      "line_number": 423,
      "page": 5,
      "line_preview": "[  423.456789] Out of memory: Killed process 1234 (java) total-vm:8G",
      "match_column_start": 27,
      "match_column_end": 36
    },
    {
      "line_number": 1102,
      "page": 12,
      "line_preview": "[ 1102.111111] OOM killer invoked, selecting process 5678",
      "match_column_start": 3,
      "match_column_end": 12
    }
  ]
}
```

| Field | Tipe | Catatan |
|---|---|---|
| `match_count` | `int` | Total match di seluruh output (bisa > `results_returned`) |
| `results_truncated` | `bool` | `true` jika `match_count > max_results` |
| `pages_hit` | `int[]` | Daftar nomor halaman yang punya match. Sorted unik. |
| `matches[].line_number` | `int` | Nomor baris global (konsisten dengan line numbers di output) |
| `matches[].page` | `int` | Nomor halaman |
| `matches[].line_preview` | `string` | Isi baris (truncated ke 500 char jika panjang) |
| `matches[].match_column_start` | `int` | Posisi kolom awal keyword (0-indexed) |
| `matches[].match_column_end` | `int` | Posisi kolom akhir keyword |

**Errors:** `ERR_RESPONSE_EXPIRED`, `ERR_SEARCH_PATTERN_INVALID`

### 5.9 [BARU v3] `jump_to_page`

Fetch halaman spesifik berdasarkan nomor halaman langsung — tanpa perlu cursor dari halaman sebelumnya.

**Request:**
```json
{
  "request_id": "req_101",
  "type": "jump_to_page",
  "response_id": "res_page01",
  "page_number": 5,
  "options": { "output_limit": 50 }
}
```

| Field | Wajib | Default | Catatan |
|---|---|---|---|
| `response_id` | ya | — | Response yang output-nya mau diakses |
| `page_number` | ya | — | Nomor halaman (1-indexed) |
| `options.output_limit` | tidak | page_size dari response asli | Override ukuran halaman |

**Response:** Format sama dengan `fetch_page` response (type: `page_result`) + `line_number_start` / `line_number_end`.

**Errors:** `ERR_RESPONSE_EXPIRED`, `ERR_PAGE_OUT_OF_RANGE`

**Flow tipikal AI:**
```
1. search("OOM killer")  →  match di page 5, 12, 14
2. jump_to_page(5)       →  langsung lihat page 5
// BANDINGKAN v2: fetch_page × 5 berurutan baru sampai page 5
```

### 5.10 [BARU v3] `search_and_jump`

Kombinasi search + jump dalam satu request. Untuk kasus paling umum: AI ingin cari dan langsung lihat konteksnya.

**Request:**
```json
{
  "request_id": "req_102",
  "type": "search_and_jump",
  "response_id": "res_page01",
  "keyword": "segfault at",
  "options": {
    "jump_to_match": 0,
    "context_lines": 3,
    "case_sensitive": false,
    "max_results": 10
  }
}
```

| Field | Wajib | Default | Catatan |
|---|---|---|---|
| `jump_to_match` | tidak | `0` | Index match (0-based) untuk langsung jump |
| `context_lines` | tidak | `0` | Baris tambahan sebelum/sesudah match |
| *(field lain)* | — | — | Sama seperti `search` |

**Response sukses:**
```json
{
  "request_id": "req_102",
  "response_id": "res_sj01",
  "type": "search_and_jump_result",
  "reference_response_id": "res_page01",
  "keyword": "segfault at",
  "match_count": 2,
  "jumped_to_match": 0,
  "jumped_to_line": 567,
  "jumped_to_page": 6,
  "page_output": "   564 | some previous line\n   565 | context before\n   566 | more context\n   567 | segfault at 0x7fff1234 ip:00005555 error:4\n   568 | context after\n   569 | more context after\n   570 | next line",
  "line_number_start": 564,
  "line_number_end": 570,
  "output_lines": 7,
  "all_matches": [
    { "line_number": 567, "page": 6, "line_preview": "segfault at 0x7fff1234 ip:00005555 error:4" },
    { "line_number": 1203, "page": 13, "line_preview": "segfault at 0xdeadbeef ip:00007777 error:14" }
  ]
}
```

`page_output` berisi baris di sekitar match (ditentukan `context_lines`), dengan line numbers. `all_matches` berisi semua match supaya AI tahu match lain ada di page mana.

---

## 6. DATA STRUCTURES

### 6.1 OutputBuffer — ditambah LineEntry index

```c
typedef struct {
    size_t offset;    // byte offset awal baris di buffer
    size_t length;    // panjang baris (termasuk \n)
} LineEntry;

typedef struct {
    // === TIDAK BERUBAH dari v2 ===
    char *buffer;
    size_t buffer_size;
    size_t buffer_capacity;
    size_t total_lines;
    size_t total_bytes;
    PaginationMode mode;
    size_t page_size;
    bool truncated;

    // === BARU v3 ===
    LineEntry *line_index;         // array[total_lines], di-build saat finalize
    bool line_index_built;         // false selama append, true setelah finalize
    size_t line_index_capacity;    // untuk reallocation progresif
} OutputBuffer;
```

Line index di-build secara progresif saat `visibox_output_buffer_append()` — setiap `\n` ditemukan, tambah `LineEntry` baru. Setelah EOF, panggil `visibox_output_buffer_finalize(buf)` untuk lock index.

### 6.2 VisiboxResponse — tambahan line numbers

```c
typedef struct {
    // ... semua field v2 ...

    // === BARU v3 ===
    int line_number_start;
    int line_number_end;
} VisiboxResponse;
```

### 6.3 RequestType — penambahan

```c
typedef enum {
    REQ_EXECUTE,
    REQ_SESSION_START,
    REQ_SESSION_INPUT,
    REQ_SESSION_READ,
    REQ_SESSION_CLOSE,
    REQ_SESSION_LIST,
    REQ_FETCH_PAGE,
    REQ_SEARCH,               // [BARU v3]
    REQ_JUMP_TO_PAGE,         // [BARU v3]
    REQ_SEARCH_AND_JUMP       // [BARU v3]
} RequestType;
```

### 6.4 VisiboxRequest — penambahan

```c
typedef struct {
    // ... semua field v2 ...

    // BARU v3 — search
    char *keyword;
    bool case_sensitive;
    bool regex_mode;
    size_t max_results;
    size_t jump_to_match;
    size_t context_lines;

    // BARU v3 — jump
    size_t page_number;
} VisiboxRequest;
```

### 6.5 ResponseStore, EventLoop, Prompt Detection — TIDAK BERUBAH

Lihat PRD v2 §6.

---

## 7. CONFIG

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
    "enabled": true,
    "separator": " | ",
    "padding": "auto"
  },
  "search": {
    "default_max_results": 50,
    "max_max_results": 500,
    "line_preview_max_chars": 500,
    "regex_enabled": true
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

| Field Baru | Default | Catatan |
|---|---|---|
| `line_numbers.enabled` | `true` | Bisa dimatikan jika AI tidak butuh |
| `line_numbers.separator` | `" \| "` | Separator antara line number dan content |
| `line_numbers.padding` | `"auto"` | `"auto"` = hitung dari total_lines. Bisa hardcode angka. |
| `search.default_max_results` | `50` | Default jika request tidak tentukan |
| `search.max_max_results` | `500` | Batas atas yang boleh diminta AI |
| `search.line_preview_max_chars` | `500` | Truncate preview baris panjang |
| `search.regex_enabled` | `true` | Jika false, `regex: true` → error |

---

## 8. ROADMAP

```
FASE 1: CORE EXECUTE + LINE NUMBERS + SEARCH (Minggu 1-4)
├── Fork bash + build system, identifikasi execute_command() di source
├── Eksperimen: verifikasi cd/export persist via hook di eval.c
├── Command ID system (request_id + response_id)
├── Output capture via fd redirect (BUKAN fork tambahan)
├── Output buffer dengan line index (LineEntry[])
├── Line number formatting engine (visibox_linenums.c)
├── Output pagination (lines + bytes) + cursor encoding
├── Keyword search engine (visibox_search.c)
├── jump_to_page + search_and_jump request types
├── Response store dengan eviction policy
├── Pipe mode (echo JSON | visibox)
└── Test: state persist + pagination + line numbers + search

FASE 2: SESSIONS + EVENT LOOP (Minggu 5-8)
├── PTY session manager (openpty, login_tty)
├── Event loop (epoll) dari AWAL
├── session_start / session_input / session_read / session_close
├── ANSI strip + prompt detection
├── Session output dengan line numbers + session search
├── Idle session sweeper
├── Session list + status
└── Test: concurrent session + search dalam session output

FASE 3: API & INTERFACE (Minggu 9-11)
├── REPL mode
├── Unix socket daemon mode
├── CLI client (visibox-cli)
└── Multi-client handling

FASE 4: POLISH (Minggu 12-13)
├── Error handling lengkap
├── Memory limit enforcement
├── Dokumentasi protokol final
└── Test suite end-to-end
```

**Perubahan dari v2:** Line numbers & search masuk Fase 1 — bukan ditunda ke Fase 4. Alasannya: AI yang menerima output besar di Fase 1 tanpa cara efisien mencari sesuatu akan sangat terbatas kemampuannya.

---

## 9. SKENARIO END-TO-END

### 9.1 Line number konsistensi antar page

```json
{"request_id":"t1","type":"execute","command":"dmesg","options":{"output_limit":10}}
// page 1: line_number_start=1, line_number_end=10

{"request_id":"t2","type":"fetch_page","response_id":"res_t1","cursor":"cur_xxx"}
// page 2: line_number_start=11, line_number_end=20
// JIKA line_number_start != 11, TEST GAGAL.
```

### 9.2 Search + jump flow

```json
{"request_id":"s1","type":"execute","command":"journalctl --no-pager -n 5000","options":{"output_limit":50}}
{"request_id":"s2","type":"search","response_id":"res_s1","keyword":"segfault"}
// match di page 4, 17, 23

{"request_id":"s3","type":"jump_to_page","response_id":"res_s1","page_number":17}
// output page 17, line_number_start=801
// VERIFIKASI: search match line 823 ada di range 801-850
```

### 9.3 search_and_jump one-shot

```json
{"request_id":"s4","type":"search_and_jump","response_id":"res_s1","keyword":"OOM killer","options":{"context_lines":2}}
// langsung tampilkan konteks match pertama + semua match locations
```

### 9.4 Shell state persist (regression wajib dari v2)

```json
{"type":"execute","command":"cd /tmp"}    // exit_code: 0
{"type":"execute","command":"pwd"}         // output HARUS: /tmp
// Jika bukan /tmp, STOP. Perbaikan #1 gagal.
```

---

## 10. PERTANYAAN TERBUKA

1. *(v2)* Versi bash source mana yang jadi basis fork?
2. *(v2)* Apakah builtin yang mengubah fd (`exec 3<>file`) perlu penanganan khusus saat fd 1/2 di-redirect?
3. *(v2)* Daemon multi-client: satu shell per client atau shared?
4. **[BARU]** Apakah line numbers perlu auto-disable untuk binary output (misal `xxd`)? Atau biarkan saja — tidak merusak, hanya tidak berguna?
5. **[BARU]** Apakah `search` perlu multi-keyword (AND/OR)? Saran v3 awal: **tidak** — biarkan AI lakukan 2 search terpisah dan hitung intersection sendiri. Bisa ditambahkan v4 jika ada demand.