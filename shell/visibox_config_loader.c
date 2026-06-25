/*
 * visibox_config_loader.c — Minimal JSON Config File Loader (Fase 3)
 *
 * Loads config/visibox.conf (or a custom path) and overrides
 * the hardcoded defaults in visibox_init_config().
 *
 * Format: JSON with the same structure as PRD v3 §8.
 * Unknown keys are silently ignored (forward-compatible).
 * Only top-level numeric and string values are read.
 */

#include "visibox.h"

/* ═══════════════════════════════════════════════════════════════
 * MINIMAL JSON VALUE EXTRACTORS
 *
 * Reuses the same approach as visibox_protocol.c:
 * find key via strstr, then read value.
 * ═══════════════════════════════════════════════════════════════ */

/* Read a non-negative integer from a JSON value position.
 * Handles raw numbers (no quotes). */
static const char *skip_ws_cfg(const char *p) {
    while (*p && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r'))
        p++;
    return p;
}

static const char *json_cfg_find_key(const char *json, const char *key) {
    char needle[256];
    snprintf(needle, sizeof(needle), "\"%s\"", key);
    size_t nlen = strlen(needle);

    const char *p = json;
    while (*p) {
        p = strstr(p, needle);
        if (!p) return NULL;
        const char *after = p + nlen;
        after = skip_ws_cfg(after);
        if (*after == ':') return after + 1;
        p = after;
    }
    return NULL;
}

static int json_cfg_read_int(const char *json, const char *key, long *out) {
    const char *p = json_cfg_find_key(json, key);
    if (!p) return -1;

    p = skip_ws_cfg(p);

    /* Try raw number first */
    if (*p == '-' || (*p >= '0' && *p <= '9')) {
        char *end;
        long val = strtol(p, &end, 10);
        if (end != p) {
            *out = val;
            return 0;
        }
    }

    /* Try quoted string that contains a number */
    if (*p == '"') {
        char buf[64];
        size_t i = 0;
        p++;
        while (*p && *p != '"' && i < sizeof(buf) - 1) {
            if (*p == '\\') { p++; if (!*p) break; }
            buf[i++] = *p++;
        }
        buf[i] = '\0';
        *out = strtol(buf, NULL, 10);
        return 0;
    }

    return -1;
}

static int json_cfg_read_size(const char *json, const char *key, size_t *out) {
    long val;
    if (json_cfg_read_int(json, key, &val) == 0 && val >= 0) {
        *out = (size_t)val;
        return 0;
    }
    return -1;
}

static int json_cfg_read_bool(const char *json, const char *key, int *out) {
    const char *p = json_cfg_find_key(json, key);
    if (!p) return -1;
    p = skip_ws_cfg(p);
    if (strncmp(p, "true", 4) == 0) { *out = 1; return 0; }
    if (strncmp(p, "false", 5) == 0) { *out = 0; return 0; }
    return -1;
}

static int json_cfg_read_string(const char *json, const char *key,
                                char *out, size_t out_size) {
    const char *p = json_cfg_find_key(json, key);
    if (!p) return -1;
    p = skip_ws_cfg(p);
    if (*p != '"') return -1;
    p++; /* skip opening quote */

    size_t i = 0;
    while (*p && *p != '"' && i < out_size - 1) {
        if (*p == '\\') {
            p++;
            if (!*p) break;
            switch (*p) {
                case 'n':  out[i++] = '\n'; break;
                case 't':  out[i++] = '\t'; break;
                case 'r':  out[i++] = '\r'; break;
                case '\\': out[i++] = '\\'; break;
                case '"':  out[i++] = '"';  break;
                default:   out[i++] = *p;   break;
            }
        } else {
            out[i++] = *p;
        }
        p++;
    }
    out[i] = '\0';
    return (i > 0) ? 0 : -1;
}

/* Find a sub-object by key, return pointer past ':' to '{'.
 * Returns NULL if not found or not an object. */
static const char *json_cfg_find_subobject(const char *json, const char *key) {
    const char *p = json_cfg_find_key(json, key);
    if (!p) return NULL;
    p = skip_ws_cfg(p);
    if (*p == '{') return p;
    return NULL;
}

/* ═══════════════════════════════════════════════════════════════
 * CONFIG LOADER
 * ═══════════════════════════════════════════════════════════════ */

int visibox_load_config(const char *path) {
    if (!path) return -1;

    FILE *f = fopen(path, "r");
    if (!f) {
        /* Config file not found is not fatal — use defaults */
        fprintf(stderr, "visibox: config not found at %s, using defaults\n", path);
        return 0;
    }

    /* Read entire file */
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    if (fsize <= 0 || fsize > 1024 * 1024) {
        fclose(f);
        return 0;
    }
    fseek(f, 0, SEEK_SET);

    char *json = (char *)malloc((size_t)fsize + 1);
    if (!json) {
        fclose(f);
        return -1;
    }

    size_t nread = fread(json, 1, (size_t)fsize, f);
    json[nread] = '\0';
    fclose(f);

    /* Find sub-objects */
    const char *pagination = json_cfg_find_subobject(json, "pagination");
    const char *line_numbers = json_cfg_find_subobject(json, "line_numbers");
    const char *search = json_cfg_find_subobject(json, "search");
    const char *store = json_cfg_find_subobject(json, "response_store");
    const char *sessions = json_cfg_find_subobject(json, "sessions");
    const char *execute = json_cfg_find_subobject(json, "execute");

    /* --- Pagination --- */
    if (pagination) {
        size_t val;
        if (json_cfg_read_size(pagination, "default_page_size", &val) == 0 &&
            val >= VISIBOX_MIN_PAGE_SIZE && val <= VISIBOX_MAX_PAGE_SIZE)
            visibox_config.pagination.default_page_size = val;

        if (json_cfg_read_size(pagination, "max_page_size", &val) == 0 && val > 0)
            visibox_config.pagination.max_page_size = val;

        if (json_cfg_read_size(pagination, "min_page_size", &val) == 0 && val > 0)
            visibox_config.pagination.min_page_size = val;

        int bval;
        if (json_cfg_read_bool(pagination, "include_total", &bval) == 0)
            visibox_config.pagination.include_total = bval;

        if (json_cfg_read_size(pagination, "cursor_expiry_ms", &val) == 0)
            visibox_config.pagination.cursor_expiry_ms = val;
    }

    /* --- Line numbers --- */
    if (line_numbers) {
        int bval;
        if (json_cfg_read_bool(line_numbers, "default_enabled", &bval) == 0)
            visibox_config.line_numbers.enabled = bval;

        char sep[16];
        if (json_cfg_read_string(line_numbers, "separator", sep, sizeof(sep)) == 0)
            strncpy(visibox_config.line_numbers.separator, sep,
                    sizeof(visibox_config.line_numbers.separator) - 1);

        size_t val;
        if (json_cfg_read_size(line_numbers, "max_line_number_width", &val) == 0 && val > 0)
            visibox_config.line_numbers.max_width = (int)val;
    }

    /* --- Search --- */
    if (search) {
        long ival;
        if (json_cfg_read_int(search, "default_context_lines", &ival) == 0 && ival >= 0)
            visibox_config.search.default_context_lines = (int)ival;

        if (json_cfg_read_int(search, "max_context_lines", &ival) == 0 && ival >= 0)
            visibox_config.search.max_context_lines = (int)ival;

        size_t val;
        if (json_cfg_read_size(search, "max_keyword_length", &val) == 0 && val > 0)
            visibox_config.search.max_keyword_length = (int)val;

        int bval;
        if (json_cfg_read_bool(search, "case_sensitive_default", &bval) == 0)
            visibox_config.search.case_sensitive_default = bval;
    }

    /* --- Response store --- */
    if (store) {
        size_t val;
        if (json_cfg_read_size(store, "max_entries", &val) == 0 && val > 0)
            visibox_config.store.max_entries = val;
        if (json_cfg_read_size(store, "max_total_bytes", &val) == 0)
            visibox_config.store.max_total_bytes = val;
    }

    /* --- Sessions --- */
    if (sessions) {
        size_t val;
        if (json_cfg_read_size(sessions, "max_concurrent", &val) == 0 && val > 0 &&
            val <= VISIBOX_MAX_SESSIONS)
            visibox_config.sessions.max_concurrent = val;

        if (json_cfg_read_size(sessions, "default_read_timeout_ms", &val) == 0)
            visibox_config.sessions.default_read_timeout_ms = val;

        if (json_cfg_read_size(sessions, "idle_timeout_ms", &val) == 0)
            visibox_config.sessions.idle_timeout_ms = val;

        if (json_cfg_read_size(sessions, "max_output_buffer_bytes_per_session", &val) == 0)
            visibox_config.sessions.max_output_buffer_bytes_per_session = val;
    }

    /* --- Execute --- */
    if (execute) {
        size_t val;
        if (json_cfg_read_size(execute, "default_timeout_ms", &val) == 0)
            visibox_config.execute.default_timeout_ms = val;
        if (json_cfg_read_size(execute, "max_timeout_ms", &val) == 0)
            visibox_config.execute.max_timeout_ms = val;
    }

    free(json);
    return 0;
}