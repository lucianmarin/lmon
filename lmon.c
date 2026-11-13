/*
 * LMON (Light Machine / Lucian Marin Object Notation) — single-file C11 implementation.
 *
 * Features:
 *   - Flat contiguous value-array DOM (zero pointer-to-pointer indirection)
 *   - Zero-copy string & key slicing with ARM NEON SIMD / fast ASCII scanning
 *   - Auto-detected indentation (2, 4, 6, 8 spaces, or '\t') & universal newlines (\n, \r\n, \r)
 *   - Complete LMON parser, canonical serializer, and RFC 8259 JSON converter
 *   - Embeddable as a library (#define LMON_NO_MAIN before including/compiling)
 *
 * Build & CLI usage:
 *   cc -O3 -std=c11 -Wall -Wextra lmon.c -o lmon_c
 *   ./lmon_c examples/gateway.lmon
 *   ./lmon_c --compact examples/gateway.lmon
 *   ./lmon_c --lmon [--indent 2|4|6|8|tab] examples/gateway.lmon
 *   ./lmon_c --from-json input.json
 *   ./lmon_c --check examples/gateway.lmon
 *   ./lmon_c --test
 *   ./lmon_c --bench [file.lmon]
 */

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>
#define LMON_HAS_NEON 1
#else
#define LMON_HAS_NEON 0
#endif

/* -------------------------------------------------------------------------
 * Public Flat-DOM Types & Arena Allocator
 * ------------------------------------------------------------------------- */

typedef enum {
    LMON_NULL = 0,
    LMON_BOOL,
    LMON_INT,
    LMON_FLOAT,
    LMON_STRING,
    LMON_ARRAY,
    LMON_OBJECT
} LmonType;

typedef struct {
    const char *ptr;
    size_t len;
} LmonStr;

typedef struct LmonValue LmonValue;
typedef struct LmonMember LmonMember;

struct LmonValue {
    LmonType type;
    union {
        bool b;
        int64_t i;
        double f;
        LmonStr s;
        struct {
            LmonValue *items;
            size_t len;
        } arr;
        struct {
            LmonMember *members;
            size_t len;
        } obj;
    } u;
};

struct LmonMember {
    LmonStr key;
    LmonValue val;
};

struct LmonArenaBlock {
    struct LmonArenaBlock *next;
    size_t cap;
    size_t used;
    _Alignas(8) unsigned char data[];
};

typedef struct LmonArenaBlock LmonArenaBlock;

typedef struct {
    LmonArenaBlock *head;
    size_t default_block_size;
} LmonArena;

typedef struct {
    const char *message;
    size_t pos;
    size_t line;
    size_t col;
} LmonError;

static void lmon_arena_init(LmonArena *arena, size_t hint_bytes) {
    arena->head = NULL;
    arena->default_block_size = hint_bytes < 4096 ? 4096 : hint_bytes;
}

static void lmon_arena_reset(LmonArena *arena) {
    if (!arena->head) return;
    LmonArenaBlock *keep = arena->head;
    LmonArenaBlock *blk = keep->next;
    while (blk) {
        LmonArenaBlock *next = blk->next;
        free(blk);
        blk = next;
    }
    keep->next = NULL;
    keep->used = 0;
}

static void lmon_arena_free(LmonArena *arena) {
    LmonArenaBlock *blk = arena->head;
    while (blk) {
        LmonArenaBlock *next = blk->next;
        free(blk);
        blk = next;
    }
    arena->head = NULL;
}

static inline void *lmon_arena_alloc(LmonArena *arena, size_t size) {
    size_t aligned = (size + 7u) & ~7u;
    LmonArenaBlock *blk = arena->head;
    if (!blk || blk->cap - blk->used < aligned) {
        size_t cap = arena->default_block_size;
        if (aligned > cap) cap = aligned;
        blk = (LmonArenaBlock *)malloc(sizeof(LmonArenaBlock) + cap);
        if (!blk) return NULL;
        blk->cap = cap;
        blk->used = 0;
        blk->next = arena->head;
        arena->head = blk;
    }
    void *ptr = blk->data + blk->used;
    blk->used += aligned;
    return ptr;
}

/* Growable byte buffer for serialization */
typedef struct {
    char *data;
    size_t len;
    size_t cap;
} LmonBuf;

static bool lmon_buf_init(LmonBuf *b, size_t initial_cap) {
    b->cap = initial_cap < 256 ? 256 : initial_cap;
    b->data = (char *)malloc(b->cap);
    b->len = 0;
    if (b->data) b->data[0] = '\0';
    return b->data != NULL;
}

static bool lmon_buf_reserve(LmonBuf *b, size_t extra) {
    if (b->cap - b->len >= extra) return true;
    size_t new_cap = b->cap * 2;
    while (new_cap - b->len < extra) new_cap *= 2;
    char *p = (char *)realloc(b->data, new_cap);
    if (!p) return false;
    b->data = p;
    b->cap = new_cap;
    return true;
}

static void lmon_buf_Append(LmonBuf *b, const char *s, size_t n) {
    if (!lmon_buf_reserve(b, n + 1)) return;
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
}

static void lmon_buf_putc(LmonBuf *b, char c) {
    if (!lmon_buf_reserve(b, 2)) return;
    b->data[b->len++] = c;
    b->data[b->len] = '\0';
}

static void lmon_buf_spaces(LmonBuf *b, size_t spaces) {
    if (!lmon_buf_reserve(b, spaces + 1)) return;
    memset(b->data + b->len, ' ', spaces);
    b->len += spaces;
    b->data[b->len] = '\0';
}

static void lmon_buf_repeat_unit(LmonBuf *b, const char *unit, size_t unit_len, size_t depth) {
    if (depth == 0 || unit_len == 0) return;
    size_t total = unit_len * depth;
    if (!lmon_buf_reserve(b, total + 1)) return;
    if (unit_len == 1) {
        memset(b->data + b->len, (unsigned char)unit[0], total);
        b->len += total;
    } else if (unit[0] == ' ') {
        memset(b->data + b->len, ' ', total);
        b->len += total;
    } else {
        for (size_t i = 0; i < depth; i++) {
            memcpy(b->data + b->len, unit, unit_len);
            b->len += unit_len;
        }
    }
    b->data[b->len] = '\0';
}

/* -------------------------------------------------------------------------
 * Character & UTF-8 Helpers
 * ------------------------------------------------------------------------- */

/* 1 = [A-Za-z_], 2 = [0-9-] */
static const uint8_t KEY_CLASS[256] = {
    ['A'] = 1, ['B'] = 1, ['C'] = 1, ['D'] = 1, ['E'] = 1, ['F'] = 1, ['G'] = 1,
    ['H'] = 1, ['I'] = 1, ['J'] = 1, ['K'] = 1, ['L'] = 1, ['M'] = 1, ['N'] = 1,
    ['O'] = 1, ['P'] = 1, ['Q'] = 1, ['R'] = 1, ['S'] = 1, ['T'] = 1, ['U'] = 1,
    ['V'] = 1, ['W'] = 1, ['X'] = 1, ['Y'] = 1, ['Z'] = 1,
    ['_'] = 1,
    ['a'] = 1, ['b'] = 1, ['c'] = 1, ['d'] = 1, ['e'] = 1, ['f'] = 1, ['g'] = 1,
    ['h'] = 1, ['i'] = 1, ['j'] = 1, ['k'] = 1, ['l'] = 1, ['m'] = 1, ['n'] = 1,
    ['o'] = 1, ['p'] = 1, ['q'] = 1, ['r'] = 1, ['s'] = 1, ['t'] = 1, ['u'] = 1,
    ['v'] = 1, ['w'] = 1, ['x'] = 1, ['y'] = 1, ['z'] = 1,
    ['0'] = 2, ['1'] = 2, ['2'] = 2, ['3'] = 2, ['4'] = 2,
    ['5'] = 2, ['6'] = 2, ['7'] = 2, ['8'] = 2, ['9'] = 2,
    ['-'] = 2
};

static inline bool lmon_is_reserved_key(const char *s, size_t len) {
    if (len == 4) {
        return (s[0] == 't' && memcmp(s, "true", 4) == 0) ||
               (s[0] == 'n' && memcmp(s, "null", 4) == 0);
    }
    if (len == 5) {
        return s[0] == 'f' && memcmp(s, "false", 5) == 0;
    }
    return false;
}

/* Validate one non-ASCII UTF-8 sequence starting at *pp and advance *pp */
static inline bool lmon_step_utf8_non_ascii(const char **pp, const char *end) {
    const unsigned char *p = (const unsigned char *)(*pp);
    const unsigned char *uend = (const unsigned char *)end;
    unsigned char c = *p;
    if ((c & 0xE0u) == 0xC0u) {
        if (c < 0xC2u || uend - p < 2 || (p[1] & 0xC0u) != 0x80u) return false;
        *pp += 2;
        return true;
    }
    if ((c & 0xF0u) == 0xE0u) {
        if (uend - p < 3 || (p[1] & 0xC0u) != 0x80u || (p[2] & 0xC0u) != 0x80u) return false;
        if (c == 0xE0u && p[1] < 0xA0u) return false;
        if (c == 0xEDu && p[1] >= 0xA0u) return false; /* UTF-16 surrogates forbidden */
        *pp += 3;
        return true;
    }
    if ((c & 0xF8u) == 0xF0u) {
        if (c > 0xF4u || uend - p < 4 || (p[1] & 0xC0u) != 0x80u ||
            (p[2] & 0xC0u) != 0x80u || (p[3] & 0xC0u) != 0x80u) return false;
        if (c == 0xF0u && p[1] < 0x90u) return false;
        if (c == 0xF4u && p[1] >= 0x90u) return false;
        *pp += 4;
        return true;
    }
    return false;
}

static inline int hex_val(unsigned char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static inline size_t encode_utf8(uint32_t cp, char out[4]) {
    if (cp <= 0x7Fu) {
        out[0] = (char)cp;
        return 1;
    } else if (cp <= 0x7FFu) {
        out[0] = (char)(0xC0u | (cp >> 6));
        out[1] = (char)(0x80u | (cp & 0x3Fu));
        return 2;
    } else if (cp <= 0xFFFFu) {
        out[0] = (char)(0xE0u | (cp >> 12));
        out[1] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
        out[2] = (char)(0x80u | (cp & 0x3Fu));
        return 3;
    } else {
        out[0] = (char)(0xF0u | (cp >> 18));
        out[1] = (char)(0x80u | ((cp >> 12) & 0x3Fu));
        out[2] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
        out[3] = (char)(0x80u | (cp & 0x3Fu));
        return 4;
    }
}

/* -------------------------------------------------------------------------
 * Single-Pass LMON Parser (Auto-Indent + Universal Newlines + Flat DOM)
 * ------------------------------------------------------------------------- */

typedef struct {
    const char *src;
    const char *end;
    const char *p;
    const char *line_start;
    int line_indent;  /* -2 = uncached, -1 = EOF, >= 0 = indent level */
    char indent_char; /* 0 = unlocked, ' ' or '\t' */
    int indent_width; /* 0 = unlocked, 1, 2, 4, 6, or 8 */
    int indent_shift; /* 0, 1, 2, 3, or -1 for width 6 */
    LmonArena *arena;
    LmonError *err;
} LmonParser;

static void lmon_set_error(LmonParser *ps, const char *msg, const char *at) {
    if (!ps->err || ps->err->message) return;
    size_t pos = (size_t)(at - ps->src);
    size_t line = 1;
    size_t col = 1;
    for (size_t i = 0; i < pos; i++) {
        char c = ps->src[i];
        if (c == '\n') {
            line++;
            col = 1;
        } else if (c == '\r') {
            line++;
            col = 1;
            if (i + 1 < pos && ps->src[i + 1] == '\n') {
                i++;
            }
        } else if (((unsigned char)c & 0xC0u) != 0x80u) {
            col++;
        }
    }
    ps->err->message = msg;
    ps->err->pos = pos;
    ps->err->line = line;
    ps->err->col = col;
}

static inline int lmon_peek_line(LmonParser *ps) {
    if (ps->line_indent != -2) return ps->line_indent;

    const char *p = ps->p;
    const char *end = ps->end;

    while (p < end) {
        const char *line_begin = p;
        unsigned char first = (unsigned char)*p;

        if (first == ' ') {
            while (end - p >= 8) {
                uint64_t w8;
                memcpy(&w8, p, 8);
                if (w8 != 0x2020202020202020ULL) break;
                p += 8;
            }
            while (end - p >= 4) {
                uint32_t w4;
                memcpy(&w4, p, 4);
                if (w4 != 0x20202020u) break;
                p += 4;
            }
            while (p < end && *p == ' ') p++;

            if (p >= end) {
                ps->p = p;
                ps->line_indent = -1;
                return -1;
            }
            unsigned char c = (unsigned char)*p;
            if (c == '\n') {
                p++;
                continue;
            }
            if (c == '\r') {
                p += (p + 1 < end && p[1] == '\n') ? 2 : 1;
                continue;
            }
            if (c == '\t') {
                /* Blank lines may contain spaces + tabs before newline/EOF */
                const char *q = p + 1;
                while (q < end && (*q == ' ' || *q == '\t')) q++;
                if (q >= end) {
                    ps->p = q;
                    ps->line_indent = -1;
                    return -1;
                }
                if (*q == '\n') { p = q + 1; continue; }
                if (*q == '\r') { p = (q + 1 < end && q[1] == '\n') ? q + 2 : q + 1; continue; }
                lmon_set_error(ps, "mixed spaces and tabs in indentation", p);
                return -3;
            }

            size_t count = (size_t)(p - line_begin);
            int level;
            if (ps->indent_char == 0) {
                if (count != 2 && count != 4 && count != 6 && count != 8) {
                    lmon_set_error(ps, "indentation unit must be 2, 4, 6, or 8 spaces (or 1 tab)", line_begin);
                    return -3;
                }
                ps->indent_char = ' ';
                ps->indent_width = (int)count;
                ps->indent_shift = (count == 2) ? 1 : (count == 4) ? 2 : (count == 8) ? 3 : -1;
                level = 1;
            } else if (ps->indent_char != ' ') {
                lmon_set_error(ps, "inconsistent indentation: document uses tabs, got spaces", line_begin);
                return -3;
            } else if (ps->indent_shift >= 0) {
                if ((count & ((size_t)ps->indent_width - 1u)) != 0) {
                    lmon_set_error(ps, "indentation is not a multiple of detected space width", line_begin);
                    return -3;
                }
                level = (int)(count >> ps->indent_shift);
            } else {
                if ((count % 6u) != 0) {
                    lmon_set_error(ps, "indentation is not a multiple of 6 spaces", line_begin);
                    return -3;
                }
                level = (int)(count / 6u);
            }

            ps->p = p;
            ps->line_start = line_begin;
            ps->line_indent = level;
            return level;
        }

        if (first == '\t') {
            do { p++; } while (p < end && *p == '\t');

            if (p >= end) {
                ps->p = p;
                ps->line_indent = -1;
                return -1;
            }
            unsigned char c = (unsigned char)*p;
            if (c == '\n') {
                p++;
                continue;
            }
            if (c == '\r') {
                p += (p + 1 < end && p[1] == '\n') ? 2 : 1;
                continue;
            }
            if (c == ' ') {
                const char *q = p + 1;
                while (q < end && (*q == ' ' || *q == '\t')) q++;
                if (q >= end) {
                    ps->p = q;
                    ps->line_indent = -1;
                    return -1;
                }
                if (*q == '\n') { p = q + 1; continue; }
                if (*q == '\r') { p = (q + 1 < end && q[1] == '\n') ? q + 2 : q + 1; continue; }
                lmon_set_error(ps, "mixed tabs and spaces in indentation", p);
                return -3;
            }

            size_t count = (size_t)(p - line_begin);
            if (ps->indent_char == 0) {
                if (count != 1) {
                    lmon_set_error(ps, "first indented block must start at 1 tab", line_begin);
                    return -3;
                }
                ps->indent_char = '\t';
                ps->indent_width = 1;
                ps->indent_shift = 0;
            } else if (ps->indent_char != '\t') {
                lmon_set_error(ps, "inconsistent indentation: document uses spaces, got tab", line_begin);
                return -3;
            }

            ps->p = p;
            ps->line_start = line_begin;
            ps->line_indent = (int)count;
            return (int)count;
        }

        if (first == '\n') {
            p++;
            continue;
        }
        if (first == '\r') {
            p += (p + 1 < end && p[1] == '\n') ? 2 : 1;
            continue;
        }

        ps->p = p;
        ps->line_start = line_begin;
        ps->line_indent = 0;
        return 0;
    }

    ps->p = p;
    ps->line_indent = -1;
    return -1;
}

static inline bool lmon_consume_line_end(LmonParser *ps, bool allow_eof) {
    const char *p = ps->p;
    if (p >= ps->end) {
        if (!allow_eof) {
            lmon_set_error(ps, "unexpected end of input; expected nested block", p);
            return false;
        }
        ps->line_indent = -1;
        return true;
    }
    unsigned char c = (unsigned char)*p;
    if (c == '\n') {
        ps->p = p + 1;
        ps->line_indent = -2;
        return true;
    }
    if (c == '\r') {
        ps->p = (p + 1 < ps->end && p[1] == '\n') ? p + 2 : p + 1;
        ps->line_indent = -2;
        return true;
    }
    if (c == '\t') {
        lmon_set_error(ps, "tabs are not allowed outside strings", p);
        return false;
    }
    if (c == ' ') {
        lmon_set_error(ps, "trailing spaces are not allowed", p);
        return false;
    }
    lmon_set_error(ps, "unexpected character at end of line", p);
    return false;
}

static bool lmon_parse_string_raw(LmonParser *ps, LmonStr *out) {
    const char *p = ps->p;
    const char *end = ps->end;
    if (p >= end || *p != '"') {
        lmon_set_error(ps, "expected '\"'", p);
        return false;
    }
    p++;
    const char *start = p;

#if LMON_HAS_NEON
    /* 16-byte SIMD fast path: skip clean printable ASCII (0x20..0x7F excluding '"' and '\\') */
    const uint8x16_t v_quote = vdupq_n_u8('"');
    const uint8x16_t v_bslash = vdupq_n_u8('\\');
    const int8x16_t v_ctrl_or_utf8 = vdupq_n_s8(0x20);
    while (end - p >= 16) {
        uint8x16_t chunk = vld1q_u8((const uint8_t *)p);
        /* Signed < 0x20 catches BOTH control bytes (0x00..0x1F) AND high UTF-8 bytes (0x80..0xFF)! */
        uint8x16_t special = vorrq_u8(
            vorrq_u8(vceqq_u8(chunk, v_quote), vceqq_u8(chunk, v_bslash)),
            vreinterpretq_u8_s8(vcltq_s8(vreinterpretq_s8_u8(chunk), v_ctrl_or_utf8))
        );
        if (vmaxvq_u8(special) != 0) break;
        p += 16;
    }
#endif

    while (p < end) {
        unsigned char c = (unsigned char)*p;
        if (c == '"') {
            out->ptr = start;
            out->len = (size_t)(p - start);
            ps->p = p + 1;
            return true;
        }
        if (c == '\\') break;
        if (c < 0x20u) {
            lmon_set_error(ps, "unescaped control character in string", p);
            return false;
        }
        if (c >= 0x80u) {
            if (!lmon_step_utf8_non_ascii(&p, end)) {
                lmon_set_error(ps, "invalid UTF-8 byte sequence in string", p);
                return false;
            }
            continue;
        }
        p++;
    }

    if (p >= end) {
        lmon_set_error(ps, "unterminated string literal", p);
        return false;
    }

    /* Slow path: string contains backslash escapes; unescape into arena */
    size_t prefix_len = (size_t)(p - start);
    const char *scan_end = p;
    while (scan_end < end) {
        if (*scan_end == '"') break;
        if (*scan_end == '\\') {
            scan_end++;
            if (scan_end >= end) break;
        }
        scan_end++;
    }
    if (scan_end >= end || *scan_end != '"') {
        lmon_set_error(ps, "unterminated string literal", scan_end);
        return false;
    }

    size_t max_len = (size_t)(scan_end - start);
    char *buf = (char *)lmon_arena_alloc(ps->arena, max_len + 1);
    if (!buf) {
        lmon_set_error(ps, "out of memory", p);
        return false;
    }
    if (prefix_len > 0) memcpy(buf, start, prefix_len);
    size_t w = prefix_len;

    while (p < end) {
        unsigned char c = (unsigned char)*p;
        if (c == '"') {
            buf[w] = '\0';
            out->ptr = buf;
            out->len = w;
            ps->p = p + 1;
            return true;
        }
        if (c < 0x20u) {
            lmon_set_error(ps, "unescaped control character in string", p);
            return false;
        }
        if (c >= 0x80u) {
            const char *u0 = p;
            if (!lmon_step_utf8_non_ascii(&p, end)) {
                lmon_set_error(ps, "invalid UTF-8 byte sequence in string", u0);
                return false;
            }
            size_t ulen = (size_t)(p - u0);
            memcpy(buf + w, u0, ulen);
            w += ulen;
            continue;
        }
        if (c != '\\') {
            buf[w++] = (char)c;
            p++;
            continue;
        }
        p++; /* skip '\\' */
        if (p >= end) {
            lmon_set_error(ps, "unterminated escape sequence", p);
            return false;
        }
        unsigned char esc = (unsigned char)*p++;
        switch (esc) {
            case '"':  buf[w++] = '"'; break;
            case '\\': buf[w++] = '\\'; break;
            case '/':  buf[w++] = '/'; break;
            case 'b':  buf[w++] = '\b'; break;
            case 'f':  buf[w++] = '\f'; break;
            case 'n':  buf[w++] = '\n'; break;
            case 'r':  buf[w++] = '\r'; break;
            case 't':  buf[w++] = '\t'; break;
            case 'u': {
                if (end - p < 4) {
                    lmon_set_error(ps, "invalid \\uXXXX escape sequence", p);
                    return false;
                }
                int h0 = hex_val((unsigned char)p[0]);
                int h1 = hex_val((unsigned char)p[1]);
                int h2 = hex_val((unsigned char)p[2]);
                int h3 = hex_val((unsigned char)p[3]);
                if ((h0 | h1 | h2 | h3) < 0) {
                    lmon_set_error(ps, "invalid \\uXXXX escape sequence", p);
                    return false;
                }
                uint32_t cp = (uint32_t)((h0 << 12) | (h1 << 8) | (h2 << 4) | h3);
                p += 4;
                if (cp >= 0xD800u && cp <= 0xDBFFu) {
                    if (end - p < 6 || p[0] != '\\' || p[1] != 'u') {
                        lmon_set_error(ps, "unpaired high surrogate in string", p);
                        return false;
                    }
                    int l0 = hex_val((unsigned char)p[2]);
                    int l1 = hex_val((unsigned char)p[3]);
                    int l2 = hex_val((unsigned char)p[4]);
                    int l3 = hex_val((unsigned char)p[5]);
                    if ((l0 | l1 | l2 | l3) < 0) {
                        lmon_set_error(ps, "invalid low surrogate \\uXXXX escape", p + 2);
                        return false;
                    }
                    uint32_t low = (uint32_t)((l0 << 12) | (l1 << 8) | (l2 << 4) | l3);
                    if (low < 0xDC00u || low > 0xDFFFu) {
                        lmon_set_error(ps, "expected low surrogate (DC00..DFFF)", p + 2);
                        return false;
                    }
                    cp = 0x10000u + ((cp - 0xD800u) << 10) + (low - 0xDC00u);
                    p += 6;
                } else if (cp >= 0xDC00u && cp <= 0xDFFFu) {
                    lmon_set_error(ps, "unpaired low surrogate in string", p - 4);
                    return false;
                }
                w += encode_utf8(cp, buf + w);
                break;
            }
            default:
                lmon_set_error(ps, "invalid escape sequence in string", p - 1);
                return false;
        }
    }
    lmon_set_error(ps, "unterminated string literal", p);
    return false;
}

static const double POW10_POS[16] = {
    1e0, 1e1, 1e2, 1e3, 1e4, 1e5, 1e6, 1e7,
    1e8, 1e9, 1e10, 1e11, 1e12, 1e13, 1e14, 1e15
};

static const uint64_t POW10_U64[16] = {
    1ULL, 10ULL, 100ULL, 1000ULL, 10000ULL, 100000ULL, 1000000ULL, 10000000ULL,
    100000000ULL, 1000000000ULL, 10000000000ULL, 100000000000ULL,
    1000000000000ULL, 10000000000000ULL, 100000000000000ULL, 1000000000000000ULL
};

static bool lmon_parse_number(LmonParser *ps, LmonValue *out) {
    const char *start = ps->p;
    const char *p = start;
    const char *end = ps->end;

    bool neg = false;
    if (p < end && *p == '-') {
        neg = true;
        p++;
    }
    if (p >= end || *p < '0' || *p > '9') {
        lmon_set_error(ps, "invalid number literal", start);
        return false;
    }

    uint64_t mantissa = 0;
    size_t int_digits = 0;
    if (*p == '0') {
        p++;
        int_digits = 1;
        if (p < end && *p >= '0' && *p <= '9') {
            lmon_set_error(ps, "leading zeros are not allowed in numbers", p);
            return false;
        }
    } else {
        while (p < end && *p >= '0' && *p <= '9') {
            if (int_digits < 19) {
                mantissa = mantissa * 10u + (uint64_t)(*p - '0');
            }
            int_digits++;
            p++;
        }
    }

    bool is_float = false;
    size_t frac_digits = 0;
    uint64_t frac_val = 0;

    if (p < end && *p == '.') {
        is_float = true;
        p++;
        if (p >= end || *p < '0' || *p > '9') {
            lmon_set_error(ps, "expected digit after decimal point", p);
            return false;
        }
        while (p < end && *p >= '0' && *p <= '9') {
            if (frac_digits < 16) {
                frac_val = frac_val * 10u + (uint64_t)(*p - '0');
            }
            frac_digits++;
            p++;
        }
    }

    bool has_exp = false;
    if (p < end && (*p == 'e' || *p == 'E')) {
        is_float = true;
        has_exp = true;
        p++;
        if (p < end && (*p == '+' || *p == '-')) p++;
        if (p >= end || *p < '0' || *p > '9') {
            lmon_set_error(ps, "expected digit in exponent", p);
            return false;
        }
        while (p < end && *p >= '0' && *p <= '9') p++;
    }

    if (p < end && (*p == '.' || *p == 'e' || *p == 'E' || *p == '+' || *p == '-')) {
        lmon_set_error(ps, "malformed number literal", p);
        return false;
    }

    ps->p = p;

    if (!is_float) {
        if (int_digits <= 18) {
            out->type = LMON_INT;
            out->u.i = (mantissa == 0) ? 0 : (neg ? -(int64_t)mantissa : (int64_t)mantissa);
            return true;
        }
        if (int_digits == 19) {
            const char *d = neg ? start + 1 : start;
            const char *max_s = neg ? "9223372036854775808" : "9223372036854775807";
            if (memcmp(d, max_s, 19) <= 0) {
                out->type = LMON_INT;
                out->u.i = neg ? ((mantissa == 9223372036854775808ULL) ? INT64_MIN : -(int64_t)mantissa)
                               : (int64_t)mantissa;
                return true;
            }
        }
    }

    double dbl;
    if (!has_exp && int_digits + frac_digits <= 15) {
        uint64_t combined = mantissa * POW10_U64[frac_digits] + frac_val;
        dbl = (double)combined / POW10_POS[frac_digits];
        if (neg) dbl = -dbl;
    } else {
        size_t nlen = (size_t)(p - start);
        char stack_buf[64];
        char *heap_buf = NULL;
        char *nbuf = stack_buf;
        if (nlen >= sizeof(stack_buf)) {
            heap_buf = (char *)malloc(nlen + 1);
            if (!heap_buf) {
                lmon_set_error(ps, "out of memory", start);
                return false;
            }
            nbuf = heap_buf;
        }
        memcpy(nbuf, start, nlen);
        nbuf[nlen] = '\0';
        errno = 0;
        char *endptr = NULL;
        dbl = strtod(nbuf, &endptr);
        free(heap_buf);
        if (isnan(dbl) || isinf(dbl)) {
            lmon_set_error(ps, "numeric literal out of finite IEEE-754 range", start);
            return false;
        }
    }

    out->type = LMON_FLOAT;
    out->u.f = dbl;
    return true;
}

static bool lmon_parse_inline_value(LmonParser *ps, LmonValue *out);

static bool lmon_parse_inline_array(LmonParser *ps, LmonValue *out) {
    ps->p++; /* '[' */
    while (ps->p < ps->end && *ps->p == ' ') ps->p++;
    if (ps->p < ps->end && *ps->p == ']') {
        ps->p++;
        out->type = LMON_ARRAY;
        out->u.arr.items = NULL;
        out->u.arr.len = 0;
        return true;
    }

    LmonValue stack_buf[16];
    size_t cap = 16;
    size_t len = 0;
    LmonValue *tmp = stack_buf;

    for (;;) {
        if (len == cap) {
            cap *= 2;
            LmonValue *grown = (tmp == stack_buf)
                                   ? (LmonValue *)malloc(cap * sizeof(LmonValue))
                                   : (LmonValue *)realloc(tmp, cap * sizeof(LmonValue));
            if (!grown) {
                if (tmp != stack_buf) free(tmp);
                lmon_set_error(ps, "out of memory", ps->p);
                return false;
            }
            if (tmp == stack_buf) memcpy(grown, stack_buf, len * sizeof(LmonValue));
            tmp = grown;
        }
        if (!lmon_parse_inline_value(ps, &tmp[len])) {
            if (tmp != stack_buf) free(tmp);
            return false;
        }
        len++;

        while (ps->p < ps->end && *ps->p == ' ') ps->p++;
        if (ps->p >= ps->end) {
            if (tmp != stack_buf) free(tmp);
            lmon_set_error(ps, "unterminated inline array", ps->p);
            return false;
        }
        char c = *ps->p;
        if (c == ',') {
            ps->p++;
            while (ps->p < ps->end && *ps->p == ' ') ps->p++;
            if (ps->p < ps->end && *ps->p == ']') {
                if (tmp != stack_buf) free(tmp);
                lmon_set_error(ps, "trailing comma in inline array", ps->p);
                return false;
            }
        } else if (c == ']') {
            ps->p++;
            break;
        } else if (c == '\t') {
            if (tmp != stack_buf) free(tmp);
            lmon_set_error(ps, "tabs are not allowed outside strings", ps->p);
            return false;
        } else {
            if (tmp != stack_buf) free(tmp);
            lmon_set_error(ps, "expected ',' or ']' in inline array", ps->p);
            return false;
        }
    }

    LmonValue *arena_items = (LmonValue *)lmon_arena_alloc(ps->arena, len * sizeof(LmonValue));
    if (!arena_items) {
        if (tmp != stack_buf) free(tmp);
        lmon_set_error(ps, "out of memory", ps->p);
        return false;
    }
    memcpy(arena_items, tmp, len * sizeof(LmonValue));
    if (tmp != stack_buf) free(tmp);
    out->type = LMON_ARRAY;
    out->u.arr.items = arena_items;
    out->u.arr.len = len;
    return true;
}

static bool lmon_parse_inline_value(LmonParser *ps, LmonValue *out) {
    if (ps->p >= ps->end) {
        lmon_set_error(ps, "expected value", ps->p);
        return false;
    }
    unsigned char c = (unsigned char)*ps->p;
    switch (c) {
        case '"':
            out->type = LMON_STRING;
            return lmon_parse_string_raw(ps, &out->u.s);
        case '-':
        case '0': case '1': case '2': case '3': case '4':
        case '5': case '6': case '7': case '8': case '9':
            return lmon_parse_number(ps, out);
        case 't':
            if (ps->end - ps->p >= 4 && memcmp(ps->p, "true", 4) == 0) {
                ps->p += 4;
                out->type = LMON_BOOL;
                out->u.b = true;
                return true;
            }
            lmon_set_error(ps, "invalid literal; expected 'true'", ps->p);
            return false;
        case 'f':
            if (ps->end - ps->p >= 5 && memcmp(ps->p, "false", 5) == 0) {
                ps->p += 5;
                out->type = LMON_BOOL;
                out->u.b = false;
                return true;
            }
            lmon_set_error(ps, "invalid literal; expected 'false'", ps->p);
            return false;
        case 'n':
            if (ps->end - ps->p >= 4 && memcmp(ps->p, "null", 4) == 0) {
                ps->p += 4;
                out->type = LMON_NULL;
                return true;
            }
            lmon_set_error(ps, "invalid literal; expected 'null'", ps->p);
            return false;
        case '[':
            return lmon_parse_inline_array(ps, out);
        case '{':
            if (ps->end - ps->p >= 2 && ps->p[1] == '}') {
                ps->p += 2;
                out->type = LMON_OBJECT;
                out->u.obj.members = NULL;
                out->u.obj.len = 0;
                return true;
            }
            lmon_set_error(ps, "curly-brace objects are not allowed in LMON; use indentation or '{}'", ps->p);
            return false;
        default:
            lmon_set_error(ps, "unexpected character when parsing value", ps->p);
            return false;
    }
}

static inline bool lmon_is_dash_item(const LmonParser *ps) {
    const char *p = ps->p;
    if (p >= ps->end || *p != '-') return false;
    if (p + 1 >= ps->end) return true;
    char nxt = p[1];
    return nxt == ' ' || nxt == '\n' || nxt == '\r';
}

static inline bool lmon_parse_key(LmonParser *ps, LmonStr *out) {
    const char *p = ps->p;
    if (p < ps->end && *p == '"') {
        return lmon_parse_string_raw(ps, out);
    }
    if (p >= ps->end || KEY_CLASS[(unsigned char)*p] != 1) {
        lmon_set_error(ps, "expected object key", p);
        return false;
    }
    const char *start = p;
    p++;
    while (p < ps->end && KEY_CLASS[(unsigned char)*p] != 0) {
        p++;
    }
    size_t len = (size_t)(p - start);
    if (lmon_is_reserved_key(start, len)) {
        lmon_set_error(ps, "reserved literal must be quoted when used as a key", start);
        return false;
    }
    out->ptr = start;
    out->len = len;
    ps->p = p;
    return true;
}

static bool lmon_parse_block_object(LmonParser *ps, int expected_indent, const LmonStr *first_key, LmonValue *out);
static bool lmon_parse_block_array(LmonParser *ps, int expected_indent, bool in_object, LmonValue *out);

static bool lmon_parse_nested_block(LmonParser *ps, int expected_indent, bool from_object, LmonValue *out) {
    int indent = lmon_peek_line(ps);
    if (indent == -3) return false;
    if (indent < 0) {
        lmon_set_error(ps, "expected indented block, got end of input", ps->p);
        return false;
    }
    if (from_object && indent == expected_indent - 1 && lmon_is_dash_item(ps)) {
        return lmon_parse_block_array(ps, expected_indent - 1, true, out);
    }
    if (indent != expected_indent) {
        lmon_set_error(ps, "unexpected block indentation level", ps->line_start);
        return false;
    }
    if (lmon_is_dash_item(ps)) {
        if (from_object) {
            lmon_set_error(ps, "block array under object key must not be indented", ps->line_start);
            return false;
        }
        return lmon_parse_block_array(ps, expected_indent, false, out);
    }
    unsigned char c = (unsigned char)*ps->p;
    if (c == '"') {
        const char *str_start = ps->p;
        LmonStr k;
        if (!lmon_parse_string_raw(ps, &k)) return false;
        if (ps->p >= ps->end || *ps->p != ':') {
            lmon_set_error(ps, "nested block cannot be a standalone scalar", str_start);
            return false;
        }
        return lmon_parse_block_object(ps, expected_indent, &k, out);
    }
    if (KEY_CLASS[c] == 1) {
        return lmon_parse_block_object(ps, expected_indent, NULL, out);
    }
    lmon_set_error(ps, "nested block must be a block object or block array", ps->p);
    return false;
}

static bool lmon_parse_block_object(LmonParser *ps, int expected_indent, const LmonStr *first_key, LmonValue *out) {
    LmonMember stack_buf[16];
    size_t cap = 16;
    size_t len = 0;
    LmonMember *tmp = stack_buf;

    bool use_first = (first_key != NULL);
    for (;;) {
        LmonStr key;
        if (use_first) {
            key = *first_key;
            use_first = false;
        } else {
            int indent = lmon_peek_line(ps);
            if (indent == -3) { if (tmp != stack_buf) free(tmp); return false; }
            if (indent < expected_indent) break;
            if (indent > expected_indent) {
                if (tmp != stack_buf) free(tmp);
                lmon_set_error(ps, "unexpected indentation inside object", ps->line_start);
                return false;
            }
            if (lmon_is_dash_item(ps)) {
                if (tmp != stack_buf) free(tmp);
                lmon_set_error(ps, "cannot mix array items and object keys at same indentation", ps->p);
                return false;
            }
            if (!lmon_parse_key(ps, &key)) {
                if (tmp != stack_buf) free(tmp);
                return false;
            }
        }

        if (ps->p >= ps->end || *ps->p != ':') {
            if (tmp != stack_buf) free(tmp);
            lmon_set_error(ps, "expected ':' after object key", ps->p);
            return false;
        }
        ps->p++;

        if (len == cap) {
            cap *= 2;
            LmonMember *grown = (tmp == stack_buf)
                                    ? (LmonMember *)malloc(cap * sizeof(LmonMember))
                                    : (LmonMember *)realloc(tmp, cap * sizeof(LmonMember));
            if (!grown) {
                if (tmp != stack_buf) free(tmp);
                lmon_set_error(ps, "out of memory", ps->p);
                return false;
            }
            if (tmp == stack_buf) memcpy(grown, stack_buf, len * sizeof(LmonMember));
            tmp = grown;
        }

        tmp[len].key = key;
        if (ps->p < ps->end && *ps->p == ' ') {
            ps->p++;
            if (!lmon_parse_inline_value(ps, &tmp[len].val) || !lmon_consume_line_end(ps, true)) {
                if (tmp != stack_buf) free(tmp);
                return false;
            }
        } else {
            if (!lmon_consume_line_end(ps, false) ||
                !lmon_parse_nested_block(ps, expected_indent + 1, true, &tmp[len].val)) {
                if (tmp != stack_buf) free(tmp);
                return false;
            }
        }
        len++; /* O(1) flat-DOM append */
    }

    LmonMember *arena_members = (LmonMember *)lmon_arena_alloc(ps->arena, len * sizeof(LmonMember));
    if (!arena_members) {
        if (tmp != stack_buf) free(tmp);
        lmon_set_error(ps, "out of memory", ps->p);
        return false;
    }
    memcpy(arena_members, tmp, len * sizeof(LmonMember));
    if (tmp != stack_buf) free(tmp);
    out->type = LMON_OBJECT;
    out->u.obj.members = arena_members;
    out->u.obj.len = len;
    return true;
}

static bool lmon_parse_block_array(LmonParser *ps, int expected_indent, bool in_object, LmonValue *out) {
    LmonValue stack_buf[16];
    size_t cap = 16;
    size_t len = 0;
    LmonValue *tmp = stack_buf;

    for (;;) {
        int indent = lmon_peek_line(ps);
        if (indent == -3) { if (tmp != stack_buf) free(tmp); return false; }
        if (indent < expected_indent) break;
        if (indent > expected_indent) {
            if (tmp != stack_buf) free(tmp);
            lmon_set_error(ps, "unexpected indentation inside array", ps->line_start);
            return false;
        }
        if (!lmon_is_dash_item(ps)) {
            if (in_object) break;
            if (tmp != stack_buf) free(tmp);
            lmon_set_error(ps, "cannot mix object keys and array items at same indentation", ps->p);
            return false;
        }
        ps->p++; /* '-' */

        if (len == cap) {
            cap *= 2;
            LmonValue *grown = (tmp == stack_buf)
                                   ? (LmonValue *)malloc(cap * sizeof(LmonValue))
                                   : (LmonValue *)realloc(tmp, cap * sizeof(LmonValue));
            if (!grown) {
                if (tmp != stack_buf) free(tmp);
                lmon_set_error(ps, "out of memory", ps->p);
                return false;
            }
            if (tmp == stack_buf) memcpy(grown, stack_buf, len * sizeof(LmonValue));
            tmp = grown;
        }

        if (ps->p < ps->end && *ps->p == ' ') {
            ps->p++;
            if (!lmon_parse_inline_value(ps, &tmp[len]) || !lmon_consume_line_end(ps, true)) {
                if (tmp != stack_buf) free(tmp);
                return false;
            }
        } else {
            if (!lmon_consume_line_end(ps, false) ||
                !lmon_parse_nested_block(ps, expected_indent + 1, false, &tmp[len])) {
                if (tmp != stack_buf) free(tmp);
                return false;
            }
        }
        len++;
    }

    LmonValue *arena_items = (LmonValue *)lmon_arena_alloc(ps->arena, len * sizeof(LmonValue));
    if (!arena_items) {
        if (tmp != stack_buf) free(tmp);
        lmon_set_error(ps, "out of memory", ps->p);
        return false;
    }
    memcpy(arena_items, tmp, len * sizeof(LmonValue));
    if (tmp != stack_buf) free(tmp);
    out->type = LMON_ARRAY;
    out->u.arr.items = arena_items;
    out->u.arr.len = len;
    return true;
}

/* Note: returned strings slice directly into `src` when unescaped, so `src` must outlive `arena`. */
LmonValue *lmon_parse(const char *src, size_t len, LmonArena *arena, LmonError *err) {
    if (err) memset(err, 0, sizeof(*err));

    LmonParser ps;
    ps.src = src;
    ps.end = src + len;
    ps.p = src;
    ps.line_start = src;
    ps.line_indent = -2;
    ps.indent_char = 0;
    ps.indent_width = 0;
    ps.indent_shift = 0;
    ps.arena = arena;
    ps.err = err;

    if (len >= 3 && (unsigned char)src[0] == 0xEFu &&
        (unsigned char)src[1] == 0xBBu && (unsigned char)src[2] == 0xBFu) {
        lmon_set_error(&ps, "UTF-8 BOM is not allowed", src);
        return NULL;
    }

    int indent = lmon_peek_line(&ps);
    if (indent == -3) return NULL;
    if (indent < 0) {
        lmon_set_error(&ps, "empty LMON document", ps.p);
        return NULL;
    }
    if (indent != 0) {
        lmon_set_error(&ps, "root value must not be indented", ps.line_start);
        return NULL;
    }

    LmonValue *root = (LmonValue *)lmon_arena_alloc(arena, sizeof(LmonValue));
    if (!root) {
        lmon_set_error(&ps, "out of memory", src);
        return NULL;
    }

    bool ok = false;
    if (lmon_is_dash_item(&ps)) {
        ok = lmon_parse_block_array(&ps, 0, false, root);
    } else {
        unsigned char c = (unsigned char)*ps.p;
        if (c == '"') {
            LmonStr first_s;
            if (!lmon_parse_string_raw(&ps, &first_s)) return NULL;
            if (ps.p < ps.end && *ps.p == ':') {
                ok = lmon_parse_block_object(&ps, 0, &first_s, root);
            } else {
                if (!lmon_consume_line_end(&ps, true)) return NULL;
                root->type = LMON_STRING;
                root->u.s = first_s;
                ok = true;
            }
        } else if (KEY_CLASS[c] == 1) {
            const char *q = ps.p + 1;
            while (q < ps.end && KEY_CLASS[(unsigned char)*q] != 0) q++;
            size_t ilen = (size_t)(q - ps.p);
            if (lmon_is_reserved_key(ps.p, ilen) && (q >= ps.end || *q != ':')) {
                ok = lmon_parse_inline_value(&ps, root) && lmon_consume_line_end(&ps, true);
            } else {
                ok = lmon_parse_block_object(&ps, 0, NULL, root);
            }
        } else {
            ok = lmon_parse_inline_value(&ps, root) && lmon_consume_line_end(&ps, true);
        }
    }

    if (!ok) return NULL;

    int trailing = lmon_peek_line(&ps);
    if (trailing == -3) return NULL;
    if (trailing != -1) {
        lmon_set_error(&ps, "unexpected trailing content after root value", ps.line_start);
        return NULL;
    }

    return root;
}

/* -------------------------------------------------------------------------
 * RFC 8259 JSON Parser (same Flat DOM + SIMD String Scanner for fair bench)
 * ------------------------------------------------------------------------- */

static inline void json_skip_ws(LmonParser *ps) {
    while (ps->p < ps->end) {
        unsigned char c = (unsigned char)*ps->p;
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') ps->p++;
        else break;
    }
}

static bool json_parse_value(LmonParser *ps, LmonValue *out);

static bool json_parse_array(LmonParser *ps, LmonValue *out) {
    ps->p++; /* '[' */
    json_skip_ws(ps);
    if (ps->p < ps->end && *ps->p == ']') {
        ps->p++;
        out->type = LMON_ARRAY;
        out->u.arr.items = NULL;
        out->u.arr.len = 0;
        return true;
    }
    LmonValue stack_buf[16];
    size_t cap = 16, len = 0;
    LmonValue *tmp = stack_buf;
    for (;;) {
        json_skip_ws(ps);
        if (len == cap) {
            cap *= 2;
            LmonValue *g = (tmp == stack_buf)
                               ? (LmonValue *)malloc(cap * sizeof(LmonValue))
                               : (LmonValue *)realloc(tmp, cap * sizeof(LmonValue));
            if (!g) { if (tmp != stack_buf) free(tmp); return false; }
            if (tmp == stack_buf) memcpy(g, stack_buf, len * sizeof(LmonValue));
            tmp = g;
        }
        if (!json_parse_value(ps, &tmp[len])) {
            if (tmp != stack_buf) free(tmp);
            return false;
        }
        len++;
        json_skip_ws(ps);
        if (ps->p >= ps->end) {
            if (tmp != stack_buf) free(tmp);
            lmon_set_error(ps, "unterminated JSON array", ps->p);
            return false;
        }
        if (*ps->p == ',') {
            ps->p++;
        } else if (*ps->p == ']') {
            ps->p++;
            break;
        } else {
            if (tmp != stack_buf) free(tmp);
            lmon_set_error(ps, "expected ',' or ']' in JSON array", ps->p);
            return false;
        }
    }
    LmonValue *items = (LmonValue *)lmon_arena_alloc(ps->arena, len * sizeof(LmonValue));
    if (!items) { if (tmp != stack_buf) free(tmp); return false; }
    memcpy(items, tmp, len * sizeof(LmonValue));
    if (tmp != stack_buf) free(tmp);
    out->type = LMON_ARRAY;
    out->u.arr.items = items;
    out->u.arr.len = len;
    return true;
}

static bool json_parse_object(LmonParser *ps, LmonValue *out) {
    ps->p++; /* '{' */
    json_skip_ws(ps);
    if (ps->p < ps->end && *ps->p == '}') {
        ps->p++;
        out->type = LMON_OBJECT;
        out->u.obj.members = NULL;
        out->u.obj.len = 0;
        return true;
    }
    LmonMember stack_buf[16];
    size_t cap = 16, len = 0;
    LmonMember *tmp = stack_buf;
    for (;;) {
        json_skip_ws(ps);
        LmonStr key;
        if (!lmon_parse_string_raw(ps, &key)) { if (tmp != stack_buf) free(tmp); return false; }
        json_skip_ws(ps);
        if (ps->p >= ps->end || *ps->p != ':') {
            if (tmp != stack_buf) free(tmp);
            lmon_set_error(ps, "expected ':' in JSON object", ps->p);
            return false;
        }
        ps->p++;
        json_skip_ws(ps);
        if (len == cap) {
            cap *= 2;
            LmonMember *g = (tmp == stack_buf)
                                ? (LmonMember *)malloc(cap * sizeof(LmonMember))
                                : (LmonMember *)realloc(tmp, cap * sizeof(LmonMember));
            if (!g) { if (tmp != stack_buf) free(tmp); return false; }
            if (tmp == stack_buf) memcpy(g, stack_buf, len * sizeof(LmonMember));
            tmp = g;
        }
        tmp[len].key = key;
        if (!json_parse_value(ps, &tmp[len].val)) {
            if (tmp != stack_buf) free(tmp);
            return false;
        }
        len++; /* O(1) flat-DOM append */
        json_skip_ws(ps);
        if (ps->p >= ps->end) {
            if (tmp != stack_buf) free(tmp);
            lmon_set_error(ps, "unterminated JSON object", ps->p);
            return false;
        }
        if (*ps->p == ',') {
            ps->p++;
        } else if (*ps->p == '}') {
            ps->p++;
            break;
        } else {
            if (tmp != stack_buf) free(tmp);
            lmon_set_error(ps, "expected ',' or '}' in JSON object", ps->p);
            return false;
        }
    }
    LmonMember *members = (LmonMember *)lmon_arena_alloc(ps->arena, len * sizeof(LmonMember));
    if (!members) { if (tmp != stack_buf) free(tmp); return false; }
    memcpy(members, tmp, len * sizeof(LmonMember));
    if (tmp != stack_buf) free(tmp);
    out->type = LMON_OBJECT;
    out->u.obj.members = members;
    out->u.obj.len = len;
    return true;
}

static bool json_parse_value(LmonParser *ps, LmonValue *out) {
    if (ps->p >= ps->end) {
        lmon_set_error(ps, "expected JSON value", ps->p);
        return false;
    }
    switch ((unsigned char)*ps->p) {
        case '{': return json_parse_object(ps, out);
        case '[': return json_parse_array(ps, out);
        case '"':
            out->type = LMON_STRING;
            return lmon_parse_string_raw(ps, &out->u.s);
        case 't':
            if (ps->end - ps->p >= 4 && memcmp(ps->p, "true", 4) == 0) {
                ps->p += 4;
                out->type = LMON_BOOL;
                out->u.b = true;
                return true;
            }
            lmon_set_error(ps, "invalid JSON literal", ps->p);
            return false;
        case 'f':
            if (ps->end - ps->p >= 5 && memcmp(ps->p, "false", 5) == 0) {
                ps->p += 5;
                out->type = LMON_BOOL;
                out->u.b = false;
                return true;
            }
            lmon_set_error(ps, "invalid JSON literal", ps->p);
            return false;
        case 'n':
            if (ps->end - ps->p >= 4 && memcmp(ps->p, "null", 4) == 0) {
                ps->p += 4;
                out->type = LMON_NULL;
                return true;
            }
            lmon_set_error(ps, "invalid JSON literal", ps->p);
            return false;
        default:
            return lmon_parse_number(ps, out);
    }
}

LmonValue *lmon_parse_json(const char *src, size_t len, LmonArena *arena, LmonError *err) {
    if (err) memset(err, 0, sizeof(*err));
    LmonParser ps;
    memset(&ps, 0, sizeof(ps));
    ps.src = src;
    ps.end = src + len;
    ps.p = src;
    ps.arena = arena;
    ps.err = err;

    LmonValue *root = (LmonValue *)lmon_arena_alloc(arena, sizeof(LmonValue));
    if (!root) return NULL;

    json_skip_ws(&ps);
    if (!json_parse_value(&ps, root)) return NULL;
    json_skip_ws(&ps);
    if (ps.p != ps.end) {
        lmon_set_error(&ps, "trailing data after JSON value", ps.p);
        return NULL;
    }
    return root;
}

/* -------------------------------------------------------------------------
 * JSON & Canonical LMON Serializers
 * ------------------------------------------------------------------------- */

static void lmon_write_escaped_str(LmonBuf *b, LmonStr s) {
    lmon_buf_putc(b, '"');
    const char *p = s.ptr;
    const char *end = p + s.len;
    const char *chunk = p;
    while (p < end) {
        unsigned char c = (unsigned char)*p;
        if (c == '"' || c == '\\' || c < 0x20u) {
            if (p > chunk) lmon_buf_Append(b, chunk, (size_t)(p - chunk));
            switch (c) {
                case '"':  lmon_buf_Append(b, "\\\"", 2); break;
                case '\\': lmon_buf_Append(b, "\\\\", 2); break;
                case '\b': lmon_buf_Append(b, "\\b", 2); break;
                case '\f': lmon_buf_Append(b, "\\f", 2); break;
                case '\n': lmon_buf_Append(b, "\\n", 2); break;
                case '\r': lmon_buf_Append(b, "\\r", 2); break;
                case '\t': lmon_buf_Append(b, "\\t", 2); break;
                default: {
                    char u[7];
                    snprintf(u, sizeof(u), "\\u%04x", (unsigned int)c);
                    lmon_buf_Append(b, u, 6);
                    break;
                }
            }
            p++;
            chunk = p;
        } else {
            p++;
        }
    }
    if (p > chunk) lmon_buf_Append(b, chunk, (size_t)(p - chunk));
    lmon_buf_putc(b, '"');
}

static void lmon_write_float(LmonBuf *b, double f) {
    if (f == 0.0) {
        lmon_buf_Append(b, "0.0", 3);
        return;
    }
    char num[64];
    int n = snprintf(num, sizeof(num), "%.17g", f);
    if (n < 0) return;
    char short_num[64];
    int sn = snprintf(short_num, sizeof(short_num), "%.15g", f);
    if (sn > 0 && strtod(short_num, NULL) == f) {
        memcpy(num, short_num, (size_t)sn + 1);
        n = sn;
    }
    bool has_dot_or_exp = false;
    for (int i = 0; i < n; i++) {
        if (num[i] == '.' || num[i] == 'e' || num[i] == 'E') {
            has_dot_or_exp = true;
            break;
        }
    }
    lmon_buf_Append(b, num, (size_t)n);
    if (!has_dot_or_exp) {
        lmon_buf_Append(b, ".0", 2);
    }
}

static void lmon_write_json_rec(LmonBuf *b, const LmonValue *v, bool pretty, size_t depth) {
    switch (v->type) {
        case LMON_NULL:
            lmon_buf_Append(b, "null", 4);
            break;
        case LMON_BOOL:
            if (v->u.b) lmon_buf_Append(b, "true", 4);
            else lmon_buf_Append(b, "false", 5);
            break;
        case LMON_INT: {
            char num[32];
            int n = snprintf(num, sizeof(num), "%" PRId64, v->u.i);
            if (n > 0) lmon_buf_Append(b, num, (size_t)n);
            break;
        }
        case LMON_FLOAT:
            lmon_write_float(b, v->u.f);
            break;
        case LMON_STRING:
            lmon_write_escaped_str(b, v->u.s);
            break;
        case LMON_ARRAY: {
            if (v->u.arr.len == 0) {
                lmon_buf_Append(b, "[]", 2);
                break;
            }
            lmon_buf_putc(b, '[');
            for (size_t i = 0; i < v->u.arr.len; i++) {
                if (i > 0) lmon_buf_putc(b, ',');
                if (pretty) {
                    lmon_buf_putc(b, '\n');
                    lmon_buf_spaces(b, (depth + 1) * 2);
                }
                lmon_write_json_rec(b, &v->u.arr.items[i], pretty, depth + 1);
            }
            if (pretty) {
                lmon_buf_putc(b, '\n');
                lmon_buf_spaces(b, depth * 2);
            }
            lmon_buf_putc(b, ']');
            break;
        }
        case LMON_OBJECT: {
            if (v->u.obj.len == 0) {
                lmon_buf_Append(b, "{}", 2);
                break;
            }
            lmon_buf_putc(b, '{');
            for (size_t i = 0; i < v->u.obj.len; i++) {
                if (i > 0) lmon_buf_putc(b, ',');
                if (pretty) {
                    lmon_buf_putc(b, '\n');
                    lmon_buf_spaces(b, (depth + 1) * 2);
                }
                lmon_write_escaped_str(b, v->u.obj.members[i].key);
                if (pretty) lmon_buf_Append(b, ": ", 2);
                else lmon_buf_putc(b, ':');
                lmon_write_json_rec(b, &v->u.obj.members[i].val, pretty, depth + 1);
            }
            if (pretty) {
                lmon_buf_putc(b, '\n');
                lmon_buf_spaces(b, depth * 2);
            }
            lmon_buf_putc(b, '}');
            break;
        }
    }
}

char *lmon_to_json(const LmonValue *v, bool pretty, size_t *out_len) {
    LmonBuf b;
    if (!lmon_buf_init(&b, 512)) return NULL;
    lmon_write_json_rec(&b, v, pretty, 0);
    lmon_buf_putc(&b, '\n');
    if (out_len) *out_len = b.len;
    return b.data;
}

static bool lmon_key_can_be_unquoted(LmonStr k) {
    if (k.len == 0) return false;
    if (lmon_is_reserved_key(k.ptr, k.len)) return false;
    if (KEY_CLASS[(unsigned char)k.ptr[0]] != 1) return false;
    for (size_t i = 1; i < k.len; i++) {
        if (KEY_CLASS[(unsigned char)k.ptr[i]] == 0) return false;
    }
    return true;
}

static bool lmon_try_write_inline(LmonBuf *b, const LmonValue *v) {
    switch (v->type) {
        case LMON_NULL:
            lmon_buf_Append(b, "null", 4);
            return true;
        case LMON_BOOL:
            if (v->u.b) lmon_buf_Append(b, "true", 4);
            else lmon_buf_Append(b, "false", 5);
            return true;
        case LMON_INT: {
            char num[32];
            int n = snprintf(num, sizeof(num), "%" PRId64, v->u.i);
            if (n > 0) lmon_buf_Append(b, num, (size_t)n);
            return true;
        }
        case LMON_FLOAT:
            lmon_write_float(b, v->u.f);
            return true;
        case LMON_STRING:
            lmon_write_escaped_str(b, v->u.s);
            return true;
        case LMON_OBJECT:
            if (v->u.obj.len == 0) {
                lmon_buf_Append(b, "{}", 2);
                return true;
            }
            return false;
        case LMON_ARRAY: {
            if (v->u.arr.len == 0) {
                lmon_buf_Append(b, "[]", 2);
                return true;
            }
            size_t start_len = b->len;
            lmon_buf_putc(b, '[');
            for (size_t i = 0; i < v->u.arr.len; i++) {
                if (i > 0) lmon_buf_Append(b, ", ", 2);
                if (!lmon_try_write_inline(b, &v->u.arr.items[i])) {
                    b->len = start_len;
                    b->data[b->len] = '\0';
                    return false;
                }
                if (b->len - start_len + 1 > 80) {
                    b->len = start_len;
                    b->data[b->len] = '\0';
                    return false;
                }
            }
            lmon_buf_putc(b, ']');
            if (b->len - start_len > 80) {
                b->len = start_len;
                b->data[b->len] = '\0';
                return false;
            }
            return true;
        }
    }
    return false;
}

static bool lmon_valid_indent_unit(const char *unit, size_t *out_len) {
    if (!unit) return false;
    size_t n = strlen(unit);
    if (n == 1 && unit[0] == '\t') {
        *out_len = 1;
        return true;
    }
    if (n == 2 || n == 4 || n == 6 || n == 8) {
        for (size_t i = 0; i < n; i++) {
            if (unit[i] != ' ') return false;
        }
        *out_len = n;
        return true;
    }
    return false;
}

static void lmon_emit_block(LmonBuf *b, const LmonValue *v, size_t depth, const char *unit, size_t unit_len) {
    if (v->type == LMON_OBJECT) {
        for (size_t i = 0; i < v->u.obj.len; i++) {
            lmon_buf_repeat_unit(b, unit, unit_len, depth);
            LmonStr k = v->u.obj.members[i].key;
            if (lmon_key_can_be_unquoted(k)) {
                lmon_buf_Append(b, k.ptr, k.len);
            } else {
                lmon_write_escaped_str(b, k);
            }
            lmon_buf_putc(b, ':');

            size_t colon_pos = b->len;
            lmon_buf_putc(b, ' ');
            if (lmon_try_write_inline(b, &v->u.obj.members[i].val)) {
                lmon_buf_putc(b, '\n');
            } else {
                b->len = colon_pos;
                b->data[b->len] = '\0';
                lmon_buf_putc(b, '\n');
                size_t child_depth = (v->u.obj.members[i].val.type == LMON_ARRAY) ? depth : depth + 1;
                lmon_emit_block(b, &v->u.obj.members[i].val, child_depth, unit, unit_len);
            }
        }
    } else if (v->type == LMON_ARRAY) {
        for (size_t i = 0; i < v->u.arr.len; i++) {
            lmon_buf_repeat_unit(b, unit, unit_len, depth);
            lmon_buf_putc(b, '-');

            size_t dash_pos = b->len;
            lmon_buf_putc(b, ' ');
            if (lmon_try_write_inline(b, &v->u.arr.items[i])) {
                lmon_buf_putc(b, '\n');
            } else {
                b->len = dash_pos;
                b->data[b->len] = '\0';
                lmon_buf_putc(b, '\n');
                lmon_emit_block(b, &v->u.arr.items[i], depth + 1, unit, unit_len);
            }
        }
    }
}

char *lmon_to_lmon_indent(const LmonValue *v, const char *indent_unit, size_t *out_len) {
    const char *unit = indent_unit ? indent_unit : "    ";
    size_t unit_len = 0;
    if (!lmon_valid_indent_unit(unit, &unit_len)) return NULL;

    LmonBuf b;
    if (!lmon_buf_init(&b, 512)) return NULL;
    if (lmon_try_write_inline(&b, v)) {
        lmon_buf_putc(&b, '\n');
    } else {
        lmon_emit_block(&b, v, 0, unit, unit_len);
    }
    if (out_len) *out_len = b.len;
    return b.data;
}

char *lmon_to_lmon(const LmonValue *v, size_t *out_len) {
    return lmon_to_lmon_indent(v, "    ", out_len);
}

/* -------------------------------------------------------------------------
 * Standalone CLI, Conformance Tests, and Head-to-Head Benchmark
 * ------------------------------------------------------------------------- */
#ifndef LMON_NO_MAIN

static char *read_stream_all(FILE *fp, size_t *out_len) {
    size_t cap = 4096;
    size_t len = 0;
    char *buf = (char *)malloc(cap);
    if (!buf) return NULL;
    for (;;) {
        if (cap - len < 1024) {
            cap *= 2;
            char *nb = (char *)realloc(buf, cap);
            if (!nb) { free(buf); return NULL; }
            buf = nb;
        }
        size_t n = fread(buf + len, 1, cap - len - 1, fp);
        len += n;
        if (n == 0) {
            if (ferror(fp)) { free(buf); return NULL; }
            break;
        }
    }
    buf[len] = '\0';
    *out_len = len;
    return buf;
}

static char *read_file_all(const char *path, size_t *out_len) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;
    char *data = read_stream_all(fp, out_len);
    fclose(fp);
    return data;
}

static double monotonic_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static int run_tests(void) {
    const char *json_cases[] = {
        "{}",
        "[]",
        "-99",
        "\"hello: world\"",
        "true",
        "null",
        "{\"s\":\"Lemon\",\"i\":-42,\"f\":3.25,\"t\":true,\"fa\":false,\"n\":null}",
        "{\"service\":\"lmon\",\"tags\":[\"a\",\"b\",\"c\"],\"empty_map\":{},\"empty_list\":[],"
        "\"matrix\":[[1,2],[3,4]],\"nodes\":[{\"id\":1,\"meta\":{\"ok\":true}},{\"id\":2,\"meta\":{}}],"
        "\"\":\"empty\",\"true\":\"keyword\",\"false\":false,\"null\":null,\"123\":\"numeric-key\","
        "\"-dash\":\"dash-key\",\"with space\":\"quoted\\nline\\t\\ud83c\\udf4b\"}"
    };
    const char *indents[] = {"  ", "    ", "      ", "        ", "\t"};
    size_t num_cases = sizeof(json_cases) / sizeof(json_cases[0]);
    size_t num_indents = sizeof(indents) / sizeof(indents[0]);

    for (size_t i = 0; i < num_cases; i++) {
        for (size_t u = 0; u < num_indents; u++) {
            LmonArena a1, a2;
            LmonError err;
            lmon_arena_init(&a1, 4096);
            lmon_arena_init(&a2, 4096);

            LmonValue *v1 = lmon_parse_json(json_cases[i], strlen(json_cases[i]), &a1, &err);
            if (!v1) {
                fprintf(stderr, "FAIL [case %zu json parse]: %s\n", i, err.message);
                return 1;
            }
            size_t lmon_len = 0;
            char *lmon_txt = lmon_to_lmon_indent(v1, indents[u], &lmon_len);
            LmonValue *v2 = lmon_parse(lmon_txt, lmon_len, &a2, &err);
            if (!v2) {
                fprintf(stderr, "FAIL [case %zu indent %zu lmon parse]: %s\n", i, u, err.message);
                free(lmon_txt);
                return 1;
            }
            char *j1 = lmon_to_json(v1, false, NULL);
            char *j2 = lmon_to_json(v2, false, NULL);
            if (strcmp(j1, j2) != 0) {
                fprintf(stderr, "FAIL [case %zu mismatch]\nGot: %sExp: %s", i, j2, j1);
                return 1;
            }
            free(j1);
            free(j2);
            free(lmon_txt);
            lmon_arena_free(&a1);
            lmon_arena_free(&a2);
        }
    }

    /* Universal newlines test (\r\n and bare \r mixed with \n) */
    {
        const char *univ = "a:\r\n\tb: 1\rc:\r-\r\n\tx: true\r- 2\r";
        LmonArena a;
        LmonError err;
        lmon_arena_init(&a, 1024);
        LmonValue *v = lmon_parse(univ, strlen(univ), &a, &err);
        if (!v) {
            fprintf(stderr, "FAIL [universal_newlines]: %s\n", err.message);
            return 1;
        }
        char *j = lmon_to_json(v, false, NULL);
        if (strcmp(j, "{\"a\":{\"b\":1},\"c\":[{\"x\":true},2]}\n") != 0) {
            fprintf(stderr, "FAIL [universal_newlines value]: %s", j);
            return 1;
        }
        free(j);
        lmon_arena_free(&a);
    }

    const char *invalid_cases[] = {
        "",
        "   \n",
        "   bad_indent_3: 1\n",
        "a:\n bad_indent_1: 1\n",
        "a:\n     bad_indent_5: 1\n",
        "a:\n          bad_indent_10: 1\n",
        "a:\n\t\ttwo_tabs_first: 1\n",
        "a:\n  b: 1\n\tc: 2\n",
        "a:\n\tb: 1\n  c: 2\n",
        "a:\n \tb: 1\n",
        "a:\n    - 1\n",
        "dangling:\n",
        "dangling:\nstill_root: 1\n",
        "curly: {\"a\": 1}\n",
        "true: 1\n",
        "trailing_space: 1 \n",
        "two_spaces:  1\n",
        "bad_num: 01\n",
        "trailing_comma: [1, 2,]\n",
        "mixed: 1\n- 2\n",
        "# comments are forbidden\na: 1\n"
    };
    size_t num_invalid = sizeof(invalid_cases) / sizeof(invalid_cases[0]);

    for (size_t i = 0; i < num_invalid; i++) {
        LmonArena a;
        LmonError err;
        lmon_arena_init(&a, 1024);
        LmonValue *v = lmon_parse(invalid_cases[i], strlen(invalid_cases[i]), &a, &err);
        lmon_arena_free(&a);
        if (v != NULL) {
            fprintf(stderr, "FAIL [invalid %zu accepted]: %s\n", i, invalid_cases[i]);
            return 1;
        }
    }

    printf("C LMON: all %zu conformance tests passed.\n", num_cases * num_indents + 1 + num_invalid);
    return 0;
}

static int run_bench(const char *path) {
    char *lmon4_sample = NULL;
    size_t lmon4_len = 0;

    if (path) {
        lmon4_sample = read_file_all(path, &lmon4_len);
        if (!lmon4_sample) {
            fprintf(stderr, "lmon error: cannot read '%s'\n", path);
            return 1;
        }
    } else {
        LmonBuf b;
        lmon_buf_init(&b, 65536);
        lmon_buf_Append(&b, "cluster: \"lmon-bench\"\ncount: 500\nnodes:\n", 40);
        for (int i = 0; i < 500; i++) {
            char row[256];
            int n = snprintf(
                row, sizeof(row),
                "-\n"
                "    id: %d\n"
                "    name: \"service-node-%d\"\n"
                "    active: %s\n"
                "    latency_ms: %.3f\n"
                "    tags: [\"edge\", \"lemon\", \"v1\"]\n"
                "    meta:\n"
                "        region: \"eu-central\"\n"
                "        capacity: %d\n"
                "        extra: {}\n",
                i, i, (i % 2 == 0) ? "true" : "false", 1.25 * (double)(i % 17), 256 + i
            );
            lmon_buf_Append(&b, row, (size_t)n);
        }
        lmon4_sample = b.data;
        lmon4_len = b.len;
    }

    LmonArena setup_arena;
    LmonError err;
    lmon_arena_init(&setup_arena, lmon4_len * 2 + 4096);
    LmonValue *parsed = lmon_parse(lmon4_sample, lmon4_len, &setup_arena, &err);
    if (!parsed) {
        fprintf(stderr, "lmon bench parse error: %s (line %zu)\n", err.message, err.line);
        free(lmon4_sample);
        lmon_arena_free(&setup_arena);
        return 1;
    }
    size_t lmon_tab_len = 0;
    char *lmon_tab_sample = lmon_to_lmon_indent(parsed, "\t", &lmon_tab_len);
    size_t json_len = 0;
    char *json_sample = lmon_to_json(parsed, true, &json_len);
    if (!path) {
        free(lmon4_sample);
        lmon4_sample = lmon_to_lmon_indent(parsed, "    ", &lmon4_len);
    }
    lmon_arena_free(&setup_arena);

    double kb = (double)lmon4_len / 1024.0;
    size_t iterations = (size_t)(300000.0 / (kb < 1.0 ? 1.0 : kb));
    if (iterations < 200) iterations = 200;

    LmonArena bench_arena;
    lmon_arena_init(&bench_arena, lmon4_len * 4 + 16384);

    for (int w = 0; w < 20; w++) {
        lmon_arena_reset(&bench_arena);
        (void)lmon_parse(lmon4_sample, lmon4_len, &bench_arena, &err);
    }

    volatile size_t sink = 0;
    double t0 = monotonic_sec();
    for (size_t i = 0; i < iterations; i++) {
        lmon_arena_reset(&bench_arena);
        LmonValue *v = lmon_parse(lmon4_sample, lmon4_len, &bench_arena, &err);
        sink += (size_t)v->type;
    }
    double lmon4_sec = monotonic_sec() - t0;

    double t_tab = monotonic_sec();
    for (size_t i = 0; i < iterations; i++) {
        lmon_arena_reset(&bench_arena);
        LmonValue *v = lmon_parse(lmon_tab_sample, lmon_tab_len, &bench_arena, &err);
        sink += (size_t)v->type;
    }
    double lmon_tab_sec = monotonic_sec() - t_tab;

    double t1 = monotonic_sec();
    for (size_t i = 0; i < iterations; i++) {
        lmon_arena_reset(&bench_arena);
        LmonValue *v = lmon_parse_json(json_sample, json_len, &bench_arena, &err);
        sink += (size_t)v->type;
    }
    double json_sec = monotonic_sec() - t1;
    (void)sink;

    printf("C LMON benchmark (%zu iterations, flat arena DOM):\n", iterations);
    printf("  LMON (4-space): %5.1f KiB/doc -> %.3fs (%6.1f MiB/s, %5.0f docs/s)\n",
           (double)lmon4_len / 1024.0, lmon4_sec,
           ((double)lmon4_len * (double)iterations) / (1048576.0 * lmon4_sec),
           (double)iterations / lmon4_sec);
    printf("  LMON (tab):     %5.1f KiB/doc -> %.3fs (%6.1f MiB/s, %5.0f docs/s)\n",
           (double)lmon_tab_len / 1024.0, lmon_tab_sec,
           ((double)lmon_tab_len * (double)iterations) / (1048576.0 * lmon_tab_sec),
           (double)iterations / lmon_tab_sec);
    printf("  JSON (pretty):  %5.1f KiB/doc -> %.3fs (%6.1f MiB/s, %5.0f docs/s)\n",
           (double)json_len / 1024.0, json_sec,
           ((double)json_len * (double)iterations) / (1048576.0 * json_sec),
           (double)iterations / json_sec);
    printf("  Result: LMON parsed %.2fx (4-space) / %.2fx (tab) faster per document than JSON.\n",
           json_sec / lmon4_sec, json_sec / lmon_tab_sec);

    lmon_arena_free(&bench_arena);
    free(lmon_tab_sample);
    free(json_sample);
    free(lmon4_sample);
    return 0;
}

static const char *parse_indent_arg(const char *arg) {
    if (strcmp(arg, "2") == 0) return "  ";
    if (strcmp(arg, "4") == 0) return "    ";
    if (strcmp(arg, "6") == 0) return "      ";
    if (strcmp(arg, "8") == 0) return "        ";
    if (strcmp(arg, "tab") == 0 || strcmp(arg, "\\t") == 0 || strcmp(arg, "\t") == 0) return "\t";
    return NULL;
}

static void print_usage(FILE *out) {
    fprintf(out,
            "Usage: lmon_c [options] <file.lmon | ->\n"
            "Options:\n"
            "  -c, --compact          Output compact single-line JSON\n"
            "  -l, --lmon             Output canonical LMON\n"
            "  --indent <2|4|6|8|tab> Set LMON output indentation (default: 4)\n"
            "  --from-json            Parse input as JSON and output LMON\n"
            "  --check                Validate LMON syntax only\n"
            "  --test                 Run conformance test suite\n"
            "  --bench [file]         Run LMON vs JSON parse benchmark\n");
}

int main(int argc, char **argv) {
    if (argc < 2) {
        print_usage(stdout);
        return 1;
    }
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(stdout);
            return 0;
        }
    }
    if (strcmp(argv[1], "--test") == 0) return run_tests();
    if (strcmp(argv[1], "--bench") == 0) return run_bench(argc > 2 ? argv[2] : NULL);

    bool compact = false;
    bool out_lmon = false;
    bool from_json = false;
    bool check_only = false;
    const char *indent_unit = "    ";
    const char *target = NULL;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "-c") == 0 || strcmp(a, "--compact") == 0) compact = true;
        else if (strcmp(a, "-l") == 0 || strcmp(a, "--lmon") == 0) out_lmon = true;
        else if (strcmp(a, "--indent") == 0) {
            if (i + 1 >= argc) { print_usage(stderr); return 2; }
            indent_unit = parse_indent_arg(argv[++i]);
            if (!indent_unit) {
                fprintf(stderr, "lmon error: --indent must be 2, 4, 6, 8, or tab\n");
                return 2;
            }
            out_lmon = true;
        } else if (strcmp(a, "--from-json") == 0) { from_json = true; out_lmon = true; }
        else if (strcmp(a, "--check") == 0) check_only = true;
        else if (a[0] == '-' && a[1] != '\0') {
            fprintf(stderr, "Unknown option: %s\n", a);
            print_usage(stderr);
            return 2;
        } else {
            target = a;
        }
    }

    if (!target) {
        print_usage(stderr);
        return 2;
    }

    size_t raw_len = 0;
    char *raw = NULL;
    if (strcmp(target, "-") == 0) {
        raw = read_stream_all(stdin, &raw_len);
    } else {
        raw = read_file_all(target, &raw_len);
        size_t tlen = strlen(target);
        if (tlen >= 5 && strcmp(target + tlen - 5, ".json") == 0) {
            from_json = true;
        }
    }

    if (!raw) {
        fprintf(stderr, "lmon error: failed to read '%s': %s\n", target, strerror(errno));
        return 1;
    }

    LmonArena arena;
    LmonError err;
    lmon_arena_init(&arena, raw_len * 2 + 4096);

    LmonValue *val = from_json
                         ? lmon_parse_json(raw, raw_len, &arena, &err)
                         : lmon_parse(raw, raw_len, &arena, &err);
    if (!val) {
        fprintf(stderr, "lmon error: %s: line %zu column %zu (byte %zu)\n",
                err.message ? err.message : "parse error", err.line, err.col, err.pos);
        lmon_arena_free(&arena);
        free(raw);
        return 1;
    }

    if (!check_only) {
        size_t out_len = 0;
        char *rendered = out_lmon
                             ? lmon_to_lmon_indent(val, indent_unit, &out_len)
                             : lmon_to_json(val, !compact, &out_len);
        if (!rendered) {
            fprintf(stderr, "lmon error: out of memory while formatting output\n");
            lmon_arena_free(&arena);
            free(raw);
            return 1;
        }
        fwrite(rendered, 1, out_len, stdout);
        free(rendered);
    }

    lmon_arena_free(&arena);
    free(raw);
    return 0;
}

#endif /* LMON_NO_MAIN */
