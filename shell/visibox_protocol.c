/*
 * visibox_protocol.c — Minimal JSON Parser/Serializer for VisiBox
 *
 * Handles parsing of incoming JSON requests and serialization of
 * VisiBox responses back to JSON.
 *
 * IMPORTANT: This is a MINIMAL hand-rolled parser. No external
 * JSON library dependency. Handles the specific JSON shapes
 * defined in the VisiBox PRD v3 §6.
 */

#include "visibox.h"

/* ═══════════════════════════════════════════════════════════════
 * MINIMAL JSON HELPERS
 * ═══════════════════════════════════════════════════════════════ */

/* Skip whitespace */
static const char *skip_ws(const char *p) {
    while (*p && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r'))
        p++;
    return p;
}

/* Extract a string value from JSON. p points to opening '"'.
 * Returns pointer past closing '"', or NULL on error.
 * out: buffer to write the unescaped string to.
 * out_size: size of out buffer.
 * Returns: pointer after the closing quote, or NULL on error.
 */
static const char *json_read_string(const char *p, char *out, size_t out_size) {
    if (!p || *p != '"') return NULL;
    p++; /* skip opening quote */

    size_t i = 0;
    while (*p && *p != '"') {
        if (*p == '\\') {
            p++;
            switch (*p) {
                case '"':  if (i < out_size-1) out[i++] = '"'; break;
                case '\\': if (i < out_size-1) out[i++] = '\\'; break;
                case '/':  if (i < out_size-1) out[i++] = '/'; break;
                case 'n':  if (i < out_size-1) out[i++] = '\n'; break;
                case 't':  if (i < out_size-1) out[i++] = '\t'; break;
                case 'r':  if (i < out_size-1) out[i++] = '\r'; break;
                default:   if (i < out_size-1) out[i++] = *p; break;
            }
        } else {
            if (i < out_size - 1)
                out[i++] = *p;
        }
        p++;
    }

    if (*p == '"') p++; /* skip closing quote */
    out[i] = '\0';
    return p;
}

/* Find a key in a JSON object. Returns pointer to the ':' after the key,
 * or NULL if not found. */
static const char *json_find_key(const char *json, const char *key) {
    char needle[256];
    snprintf(needle, sizeof(needle), "\"%s\"", key);
    size_t nlen = strlen(needle);

    const char *p = json;
    while (*p) {
        p = strstr(p, needle);
        if (!p) return NULL;

        /* Make sure it's actually a key (followed by whitespace and colon) */
        const char *after = p + nlen;
        after = skip_ws(after);
        if (*after == ':') {
            return after + 1; /* past the colon */
        }
        p = after;
    }
    return NULL;
}

/* Read a JSON string value for a key */
static int json_get_string(const char *json, const char *key, char *out, size_t out_size) {
    const char *p = json_find_key(json, key);
    if (!p) return -1;

    p = skip_ws(p);
    if (*p != '"') return -1;

    const char *result = json_read_string(p, out, out_size);
    return result ? 0 : -1;
}

/* Read an integer value for a key */
static int json_get_int(const char *json, const char *key, int *out) {
    char buf[64];
    if (json_get_string(json, key, buf, sizeof(buf)) == 0) {
        *out = atoi(buf);
        return 0;
    }

    /* Try reading as raw number (not a string) */
    const char *p = json_find_key(json, key);
    if (!p) return -1;

    p = skip_ws(p);
    if (*p == '-' || (*p >= '0' && *p <= '9')) {
        *out = (int)strtol(p, NULL, 10);
        return 0;
    }

    return -1;
}

/* Read a boolean value for a key */
static int json_get_bool(const char *json, const char *key, int *out) {
    const char *p = json_find_key(json, key);
    if (!p) return -1;

    p = skip_ws(p);
    if (strncmp(p, "true", 4) == 0) {
        *out = 1;
        return 0;
    }
    if (strncmp(p, "false", 5) == 0) {
        *out = 0;
        return 0;
    }
    if (strncmp(p, "null", 4) == 0) {
        *out = 0;  /* null → false for booleans */
        return 0;
    }

    return -1;
}

/* Read a size_t value for a key */
static int json_get_size(const char *json, const char *key, size_t *out) {
    int val;
    if (json_get_int(json, key, &val) == 0) {
        *out = (val >= 0) ? (size_t)val : 0;
        return 0;
    }
    return -1;
}

/* ═══════════════════════════════════════════════════════════════
 * JSON ESCAPE HELPER FOR OUTPUT
 * ═══════════════════════════════════════════════════════════════ */

static size_t json_escape(const char *src, char *dst, size_t dst_size) {
    size_t j = 0;
    for (size_t i = 0; src[i] && j < dst_size - 1; i++) {
        switch (src[i]) {
            case '"':  if (j+2 < dst_size) { dst[j++] = '\\'; dst[j++] = '"'; } break;
            case '\\': if (j+2 < dst_size) { dst[j++] = '\\'; dst[j++] = '\\'; } break;
            case '\n': if (j+2 < dst_size) { dst[j++] = '\\'; dst[j++] = 'n'; } break;
            case '\r': if (j+2 < dst_size) { dst[j++] = '\\'; dst[j++] = 'r'; } break;
            case '\t': if (j+2 < dst_size) { dst[j++] = '\\'; dst[j++] = 't'; } break;
            default:
                if ((unsigned char)src[i] < 0x20) {
                    /* Control char: \u00XX */
                    if (j+6 < dst_size) {
                        j += snprintf(dst + j, dst_size - j, "\\u%04x", (unsigned char)src[i]);
                    }
                } else {
                    dst[j++] = src[i];
                }
        }
    }
    dst[j] = '\0';
    return j;
}

/* ═══════════════════════════════════════════════════════════════
 * REQUEST TYPE MAPPING
 * ═══════════════════════════════════════════════════════════════ */

static VisiboxRequestType parse_request_type(const char *type_str) {
    if (strcmp(type_str, "execute") == 0)           return VB_REQ_EXECUTE;
    if (strcmp(type_str, "session_start") == 0)     return VB_REQ_SESSION_START;
    if (strcmp(type_str, "session_input") == 0)     return VB_REQ_SESSION_INPUT;
    if (strcmp(type_str, "session_read") == 0)      return VB_REQ_SESSION_READ;
    if (strcmp(type_str, "session_list") == 0)      return VB_REQ_SESSION_LIST;
    if (strcmp(type_str, "session_close") == 0)     return VB_REQ_SESSION_CLOSE;
    if (strcmp(type_str, "fetch_page") == 0)        return VB_REQ_FETCH_PAGE;
    if (strcmp(type_str, "session_fetch_page") == 0) return VB_REQ_SESSION_FETCH_PAGE;
    if (strcmp(type_str, "search_jump") == 0)       return VB_REQ_SEARCH_JUMP;
    return VB_REQ_UNKNOWN;
}

/* ═══════════════════════════════════════════════════════════════
 * PARSE REQUEST
 * ═══════════════════════════════════════════════════════════════ */

int visibox_parse_request(const char *json_str, VisiboxRequest *req) {
    if (!json_str || !*json_str || !req) return -1;

    memset(req, 0, sizeof(VisiboxRequest));

    /* Set defaults */
    req->output_limit = visibox_config.pagination.default_page_size;
    req->output_unit = visibox_config.pagination.mode;
    req->timeout_ms = visibox_config.execute.default_timeout_ms;
    req->line_numbers = visibox_config.line_numbers.enabled;
    req->case_sensitive = visibox_config.search.case_sensitive_default;
    req->occurrence = 1;
    req->context_lines = visibox_config.search.default_context_lines;

    /* Parse "type" (required) */
    char type_str[64];
    if (json_get_string(json_str, "type", type_str, sizeof(type_str)) != 0) {
        return -1;  /* "type" is required */
    }
    req->type = parse_request_type(type_str);
    if (req->type == VB_REQ_UNKNOWN) return -1;

    /* Parse optional "request_id" */
    char req_id[VISIBOX_ID_LEN];
    if (json_get_string(json_str, "request_id", req_id, sizeof(req_id)) == 0) {
        strncpy(req->request_id, req_id, VISIBOX_ID_LEN - 1);
    } else {
        /* Auto-generate */
        visibox_generate_id(req->request_id, "req");
    }

    /* Parse "command" for execute / session_start */
    if (req->type == VB_REQ_EXECUTE || req->type == VB_REQ_SESSION_START) {
        char cmd[VISIBOX_MAX_CMD_LEN];
        if (json_get_string(json_str, "command", cmd, sizeof(cmd)) == 0) {
            req->command = strdup(cmd);
        } else if (req->type == VB_REQ_EXECUTE) {
            return -1;  /* "command" is required for execute */
        }
    }

    /* Parse "session_id" for session_* */
    if (req->type == VB_REQ_SESSION_INPUT || req->type == VB_REQ_SESSION_READ ||
        req->type == VB_REQ_SESSION_CLOSE || req->type == VB_REQ_SESSION_FETCH_PAGE) {
        if (json_get_string(json_str, "session_id", req->session_id,
                            VISIBOX_ID_LEN) != 0) {
            return -1;
        }
    }

    /* Parse "response_id" for fetch_page / search_jump */
    if (req->type == VB_REQ_FETCH_PAGE || req->type == VB_REQ_SEARCH_JUMP) {
        if (json_get_string(json_str, "response_id", req->response_id,
                            VISIBOX_ID_LEN) != 0) {
            return -1;
        }
    }

    /* Parse "keyword" for search_jump */
    if (req->type == VB_REQ_SEARCH_JUMP) {
        char kw[VISIBOX_MAX_KEYWORD];
        if (json_get_string(json_str, "keyword", kw, sizeof(kw)) != 0 ||
            kw[0] == '\0') {
            return -1;
        }
        req->keyword = strdup(kw);
    }

    /* Parse "input" for session_input */
    if (req->type == VB_REQ_SESSION_INPUT) {
        char inp[VISIBOX_MAX_CMD_LEN];
        if (json_get_string(json_str, "input", inp, sizeof(inp)) == 0) {
            req->input = strdup(inp);
        }
    }

    /* Parse "cursor" for fetch_page / session_fetch_page */
    if (req->type == VB_REQ_FETCH_PAGE || req->type == VB_REQ_SESSION_FETCH_PAGE) {
        char cur[VISIBOX_ID_LEN];
        if (json_get_string(json_str, "cursor", cur, sizeof(cur)) == 0) {
            req->cursor = strdup(cur);
        }
    }

    /* ─── Parse options (nested object) ─── */
    /* We look for options by searching for the "options" key and
     * then parsing fields within it. For simplicity, we use a
     * flat search — "options.output_limit" etc.
     * This works because our minimal parser finds keys at any nesting. */

    /* Look for "options" prefix in the JSON */
    const char *opts = json_find_key(json_str, "options");
    if (opts) {
        /* Find the opening brace of the options object */
        opts = skip_ws(opts);
        if (*opts == '{') {
            /* Parse from options block */
            json_get_size(opts, "output_limit", &req->output_limit);

            char unit[16];
            if (json_get_string(opts, "output_unit", unit, sizeof(unit)) == 0) {
                if (strcmp(unit, "bytes") == 0)
                    req->output_unit = VB_PAGE_BYTES;
                else
                    req->output_unit = VB_PAGE_LINES;
            }

            json_get_size(opts, "timeout_ms", &req->timeout_ms);
            json_get_bool(opts, "line_numbers", &req->line_numbers);
            json_get_bool(opts, "case_sensitive", &req->case_sensitive);
            json_get_int(opts, "occurrence", &req->occurrence);
            json_get_int(opts, "context_lines", &req->context_lines);

            /* Fase 2 session options */
            json_get_bool(opts, "wait_for_prompt", &req->wait_for_prompt);
            {
                char pp[VISIBOX_MAX_PROMPT_PATTERN_LEN];
                if (json_get_string(opts, "prompt_pattern", pp, sizeof(pp)) == 0) {
                    req->prompt_pattern = strdup(pp);
                }
            }

            /* page for fetch_page */
            {
                int page_val = 0;
                json_get_int(opts, "page", &page_val);
            }
        }
    }

    /* Fase 2: Parse session_start top-level options */
    if (req->type == VB_REQ_SESSION_START) {
        const char *opts2 = json_find_key(json_str, "options");
        if (opts2) {
            opts2 = skip_ws(opts2);
            if (*opts2 == '{') {
                /* Parse initial_read_timeout_ms */
                json_get_size(opts2, "initial_read_timeout_ms", &req->timeout_ms);
            }
        }
    }

    return 0;
}

/* ═══════════════════════════════════════════════════════════════
 * ERROR CODE TO STRING
 * ═══════════════════════════════════════════════════════════════ */

static const char *error_code_str(VisiboxErrorCode code) {
    switch (code) {
        case VB_ERR_INVALID_REQUEST:      return "ERR_INVALID_REQUEST";
        case VB_ERR_SESSION_NOT_FOUND:    return "ERR_SESSION_NOT_FOUND";
        case VB_ERR_SESSION_LIMIT_REACHED: return "ERR_SESSION_LIMIT_REACHED";
        case VB_ERR_RESPONSE_EXPIRED:     return "ERR_RESPONSE_EXPIRED";
        case VB_ERR_CURSOR_INVALID:       return "ERR_CURSOR_INVALID";
        case VB_ERR_TIMEOUT:              return "ERR_TIMEOUT";
        case VB_ERR_EXEC_FAILED:          return "ERR_EXEC_FAILED";
        case VB_ERR_SEARCH_KEYWORD_EMPTY: return "ERR_SEARCH_KEYWORD_EMPTY";
        case VB_ERR_INTERNAL:             return "ERR_INTERNAL";
    }
    return "ERR_INTERNAL";
}

/* ═══════════════════════════════════════════════════════════════
 * SERIALIZE RESPONSE
 * ═══════════════════════════════════════════════════════════════ */

char *visibox_serialize_response(const VisiboxResponse *res) {
    if (!res) return strdup("{}");

    /* Allocate a large buffer — responses can be big with output */
    size_t cap = sizeof("{\"request_id\":\"\",\"response_id\":\"\",\"type\":\"\"}") +
                 4096 + (res->output ? strlen(res->output) * 2 : 0);
    char *buf = (char *)malloc(cap);
    if (!buf) return strdup("{}");

    size_t pos = 0;
    char escaped[65536];

/* Append macro with bounds checking */
#define APPEND(fmt, ...) do { \
    pos += snprintf(buf + pos, cap - pos, fmt, ##__VA_ARGS__); \
} while(0)

    APPEND("{");

    /* request_id */
    APPEND("\"request_id\":\"%s\",", res->request_id);
    APPEND("\"response_id\":\"%s\",", res->response_id);

    /* type */
    if (res->type == VB_RES_ERROR) {
        APPEND("\"type\":\"error\",");
        APPEND("\"error_code\":\"%s\",", error_code_str(res->error_code));
        json_escape(res->error_message, escaped, sizeof(escaped));
        APPEND("\"error_message\":\"%s\"", escaped);
    } else if (res->type == VB_RES_SEARCH_JUMP_RESULT) {
        APPEND("\"type\":\"search_jump_result\",");
        json_escape(res->keyword, escaped, sizeof(escaped));
        APPEND("\"keyword\":\"%s\",", escaped);
        APPEND("\"occurrence\":%d,", res->search_occurrence);
        APPEND("\"total_occurrences\":%zu,", res->total_occurrences);
        APPEND("\"found\":%s,", res->found ? "true" : "false");
        if (res->found) {
            APPEND("\"found_line\":%zu,", res->found_line);
            APPEND("\"line_numbers\":%s,", res->line_numbers ? "true" : "false");
            APPEND("\"line_start\":%zu,", res->line_start);
            APPEND("\"line_end\":%zu,", res->line_end);
            if (res->output) {
                json_escape(res->output, escaped, sizeof(escaped));
                APPEND("\"output\":\"%s\",", escaped);
            }
            /* page_hint: which page contains found_line */
            size_t ps = visibox_config.pagination.default_page_size;
            size_t page_hint = (res->found_line > 0)
                                   ? ((res->found_line - 1) / ps) + 1
                                   : 0;
            APPEND("\"page_hint\":%zu,", page_hint);
            APPEND("\"cursor\":\"%s\",", res->cursor);
        }
        APPEND("\"output_truncated\":%s", res->output_truncated ? "true" : "false");
    } else if (res->type == VB_RES_PAGE_RESULT) {
        APPEND("\"type\":\"page_result\",");
        if (res->session_id[0]) APPEND("\"session_id\":\"%s\",", res->session_id);
        APPEND("\"page\":%d,", res->page);
        APPEND("\"cursor\":\"%s\",", res->cursor);
        if (res->output) {
            json_escape(res->output, escaped, sizeof(escaped));
            APPEND("\"output\":\"%s\",", escaped);
        }
        APPEND("\"line_numbers\":%s,", res->line_numbers ? "true" : "false");
        APPEND("\"line_start\":%zu,", res->line_start);
        APPEND("\"line_end\":%zu,", res->line_end);
        APPEND("\"output_lines\":%zu,", res->output_lines);
        APPEND("\"has_next\":%s", res->has_next ? "true" : "false");
    } else if (res->type == VB_RES_SESSION_START_RESULT) {
        APPEND("\"type\":\"session_start_result\",");
        APPEND("\"session_id\":\"%s\",", res->session_id);
        APPEND("\"output_lines\":%zu,", res->output_lines);
        APPEND("\"output_bytes\":%zu,", res->output_bytes);
        APPEND("\"output_truncated\":%s,", res->output_truncated ? "true" : "false");
        if (res->line_numbers) {
            APPEND("\"line_numbers\":true,");
            APPEND("\"line_start\":%zu,", res->line_start);
            APPEND("\"line_end\":%zu,", res->line_end);
        }
        if (res->output) {
            json_escape(res->output, escaped, sizeof(escaped));
            APPEND("\"output\":\"%s\",", escaped);
        }
        APPEND("\"has_next\":%s", res->has_next ? "true" : "false");
    } else if (res->type == VB_RES_SESSION_INPUT_RESULT) {
        APPEND("\"type\":\"session_input_result\",");
        APPEND("\"session_id\":\"%s\",", res->session_id);
        APPEND("\"output_lines\":%zu,", res->output_lines);
        APPEND("\"output_bytes\":%zu,", res->output_bytes);
        APPEND("\"prompt_detected\":%s,", res->prompt_detected ? "true" : "false");
        if (res->has_exit_code)
            APPEND("\"exit_code\":%d,", res->exit_code);
        if (res->line_numbers) {
            APPEND("\"line_numbers\":true,");
            APPEND("\"line_start\":%zu,", res->line_start);
            APPEND("\"line_end\":%zu,", res->line_end);
        }
        if (res->output) {
            json_escape(res->output, escaped, sizeof(escaped));
            APPEND("\"output\":\"%s\"", escaped);
        } else {
            APPEND("\"output\":\"\"");
        }
    } else if (res->type == VB_RES_SESSION_READ_RESULT) {
        APPEND("\"type\":\"session_read_result\",");
        APPEND("\"session_id\":\"%s\",", res->session_id);
        APPEND("\"output_lines\":%zu,", res->output_lines);
        APPEND("\"output_bytes\":%zu,", res->output_bytes);
        if (res->has_exit_code)
            APPEND("\"exit_code\":%d,", res->exit_code);
        if (res->line_numbers) {
            APPEND("\"line_numbers\":true,");
            APPEND("\"line_start\":%zu,", res->line_start);
            APPEND("\"line_end\":%zu,", res->line_end);
        }
        if (res->output) {
            json_escape(res->output, escaped, sizeof(escaped));
            APPEND("\"output\":\"%s\"", escaped);
        } else {
            APPEND("\"output\":\"\"");
        }
    } else if (res->type == VB_RES_SESSION_LIST_RESULT) {
        APPEND("\"type\":\"session_list_result\",");
        APPEND("\"session_count\":%zu,", res->output_lines);
        if (res->output) {
            /* session_list output is a JSON array — no need to double-escape */
            APPEND("\"sessions\":%s", res->output);
        } else {
            APPEND("\"sessions\":[]");
        }
    } else if (res->type == VB_RES_SESSION_CLOSE_RESULT) {
        APPEND("\"type\":\"session_close_result\",");
        APPEND("\"session_id\":\"%s\",", res->session_id);
        APPEND("\"already_closed\":%s,", res->already_closed ? "true" : "false");
        if (res->has_exit_code)
            APPEND("\"exit_code\":%d,", res->exit_code);
        APPEND("\"output_lines\":%zu,", res->output_lines);
        APPEND("\"output_bytes\":%zu,", res->output_bytes);
        if (res->output) {
            json_escape(res->output, escaped, sizeof(escaped));
            APPEND("\"output\":\"%s\"", escaped);
        } else {
            APPEND("\"output\":\"\"");
        }
    } else {
        /* execute_result, session_start_result, etc. */
        APPEND("\"type\":\"execute_result\",");
        if (res->has_exit_code)
            APPEND("\"exit_code\":%d,", res->exit_code);
        else
            APPEND("\"exit_code\":null,");
        APPEND("\"duration_ms\":%zu,", res->duration_ms);
        APPEND("\"output_lines\":%zu,", res->output_lines);
        APPEND("\"output_bytes\":%zu,", res->output_bytes);
        APPEND("\"output_truncated\":%s,", res->output_truncated ? "true" : "false");
        if (res->line_numbers) {
            APPEND("\"line_numbers\":true,");
            APPEND("\"line_start\":%zu,", res->line_start);
            APPEND("\"line_end\":%zu,", res->line_end);
        }
        if (res->timed_out) {
            APPEND("\"error_code\":\"ERR_TIMEOUT\",");
            APPEND("\"timed_out\":true,");
        }
        if (res->output) {
            json_escape(res->output, escaped, sizeof(escaped));
            APPEND("\"output\":\"%s\"", escaped);
        } else {
            APPEND("\"output\":\"\"");
        }
    }

    APPEND("}");

#undef APPEND

    return buf;
}