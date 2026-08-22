/* io/q_csv.c — the incremental CSV core: `.csv.i.read` / `.csv.i.info`, the C floor under lib/csv.q.
 *
 * The core is bytes-in -> complete typed rows out + remainder carry: a feed appends to the carry, rows end
 * at an unquoted '\n' (RFC-4180 — a quoted field keeps its newlines, and may straddle any number of feeds),
 * and the incomplete tail waits for the next feed.  Nothing here assumes it knows the file size or can seek,
 * so a future decompressor can feed the same core; today's one driver is chunked q_io_read_slice reads.
 *
 * Types FREEZE after the sniff sample (field parsers + the promote lattice + the sym-cardinality rule adapted
 * from src/io/csv.c): a later cell that fails its frozen type signals 'csv — never a silent null, never a
 * mid-load re-promotion.  Anything structurally malformed — ragged row, unterminated quote, bytes between a
 * closing quote and the delimiter, a duplicate column name — is 'csv too: aborting is this PR's rejects story.
 * An option key is NEVER silently ignored: unknown or not-yet-implemented -> 'option. */
#include "qlang/q_registry_internal.h" /* q_insert_wrap (the by-name row-append), q_list_collapse via q_prim */
#include "qlang/base/q_calendar.h" /* the ONE civil-calendar home: date validity + day/ts composition */
#include "qlang/base/q_err.h"
#include "qlang/eval/q_eval.h"  /* q_eval_apply_value/_is_fn/_rank — the lambda-target seam */
#include "qlang/io/q_csv.h"
#include "qlang/io/q_io.h"      /* the byte core: paths + the slice read */
#include "qlang/q_env.h"        /* q_env_bind — the .csv.i.* bindings */
#include "core/numparse.h"      /* ray_parse_i64/f64 */
#include "lang/env.h"           /* ray_fn_vary */
#include "lang/eval.h"          /* RAY_FN_NONE, ray_at_fn */
#include "ops/hash.h"           /* ray_hash_bytes — the cardinality sample */
#include "table/sym.h"          /* ray_sym_intern_runtime / ray_sym_str */
#include <rayforce.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CSV_DEF_SAMPLE 20480          /* sniff rows (header included) */
#define CSV_DEF_BUFFER (1 << 20)      /* read-chunk bytes */
#define CSV_CARD_CAP   100            /* distinct-sample cap per column */
#define CSV_ESC_STACK  8192           /* stack un-escape buffer; larger fields malloc */

/* sniff lattice — BOOL < I64 < F64 order is load-bearing (numeric promotion) */
typedef enum { CT_UNKNOWN = 0, CT_BOOL, CT_I64, CT_F64, CT_DATE, CT_TIME, CT_TS, CT_STR } ct_t;

typedef struct {
    uint32_t h[CSV_CARD_CAP];
    uint16_t l[CSV_CARD_CAP];
    uint16_t distinct;
    uint32_t non_null;
} card_t;

typedef struct csv_st {
    char     delim;
    int      header;              /* -1 = sniff, else forced 0/1 */
    int      info_only;
    int      frozen;
    int      in_quote;
    int64_t  sample, bufsz;
    char*    carry;
    size_t   clen, ccap, scan;    /* carry always starts at the current row start */
    ray_t*   sniff;               /* pre-freeze complete rows, as charv copies */
    int64_t  ncols;
    int64_t* names;               /* every column's sym id */
    char*    ctypes;              /* every column's type char; ' ' = skipped */
    int64_t* kidx;                /* col -> kept index, -1 skipped */
    int64_t* knames;
    char*    kchars;
    int64_t  nkept;
    ray_t**  acc;                 /* per kept column: RAY_LIST of parsed atoms */
    int64_t  pend, rows_total, batches;
    int      sink_kind;           /* 0 return-table, 1 global name, 2 lambda */
    ray_t*   sink;                /* borrowed */
    ray_t*   types_arg;           /* borrowed */
    int      has_tgt, use_upsert; /* an EXISTING target table: its schema outranks the sniff */
    int64_t* tgt_names;
    char*    tgt_chars;
    int64_t  tgt_n;
    int64_t* ign;                 /* CSV columns the subsetting rider dropped */
    int64_t  nign;
    ray_t*   f_hdr;               /* freeze transients */
    ct_t*    f_ct;
    card_t*  f_card;
} csv_st;

/* ---- sniffing (adapted from src/io/csv.c detect_type / promote_csv_type) --- */

/* the DEFAULT null is the EMPTY field only — DuckDB's default; richer
 * vocabularies (NA/NULL/…) arrive later behind the nullstr option */
static int csv_null_tok(const char* f, size_t n) {
    (void)f;
    return n == 0;
}

/* ---- temporal shape validators — ONE home each, shared by the sniffer and the
 * frozen parsers, so a cell can never detect as a type its parser refuses ------ */

/* YYYY-MM-DD at f (caller guarantees >= 10 bytes); validity and day composition
 * belong to the calendar home — an impossible date fails, never normalizes */
static int csv_date10(const char* f, int32_t* out) {
    if (f[4] != '-' || f[7] != '-') return 0;
    for (int i = 0; i < 10; i++)
        if (i != 4 && i != 7 && (unsigned)(f[i] - '0') > 9) return 0;
    int64_t y = (f[0] - '0') * 1000 + (f[1] - '0') * 100 + (f[2] - '0') * 10 + (f[3] - '0');
    int64_t m = (f[5] - '0') * 10 + (f[6] - '0');
    int64_t d = (f[8] - '0') * 10 + (f[9] - '0');
    if (!q_calendar_date_valid(y, m, d)) return 0;
    *out = (int32_t)q_calendar_days_from_civil(y, m, d);
    return 1;
}

/* HH:MM:SS[.digits] -> ns since midnight; *used = bytes consumed (a fraction
 * needs at least one digit; what follows is the caller's to judge) */
static int csv_time_part(const char* f, size_t n, int64_t* ns, size_t* used) {
    if (n < 8 || f[2] != ':' || f[5] != ':') return 0;
    static const int tp[] = { 0, 1, 3, 4, 6, 7 };
    for (int i = 0; i < 6; i++)
        if ((unsigned)(f[tp[i]] - '0') > 9) return 0;
    int h = (f[0] - '0') * 10 + (f[1] - '0');
    int m = (f[3] - '0') * 10 + (f[4] - '0');
    int s = (f[6] - '0') * 10 + (f[7] - '0');
    if (h > 23 || m > 59 || s > 59) return 0;
    int64_t v = (int64_t)h * 3600000000000LL + (int64_t)m * 60000000000LL + (int64_t)s * 1000000000LL;
    size_t i = 8;
    if (n > 8 && f[8] == '.') {
        int64_t frac = 0;
        int digits = 0;
        for (i = 9; i < n && (unsigned)(f[i] - '0') <= 9; i++)
            if (digits < 9) { frac = frac * 10 + (f[i] - '0'); digits++; }
        if (i == 9) return 0;                      /* a bare trailing dot */
        while (digits < 9) { frac *= 10; digits++; }
        v += frac;
    }
    *ns = v;
    *used = i;
    return 1;
}

static int csv_time_cell(const char* f, size_t n, int64_t* ns) {
    size_t used;
    return csv_time_part(f, n, ns, &used) && used == n;
}

/* Z | ±HH[[:]MM] — the whole tail must be the offset */
static int csv_tz_ns(const char* f, size_t n, int64_t* out) {
    if (n == 1 && (f[0] == 'Z' || f[0] == 'z')) { *out = 0; return 1; }
    int sign = f[0] == '+' ? 1 : f[0] == '-' ? -1 : 0;
    if (!sign || n < 3 || (unsigned)(f[1] - '0') > 9 || (unsigned)(f[2] - '0') > 9) return 0;
    int hh = (f[1] - '0') * 10 + (f[2] - '0');
    int mm = 0;
    size_t i = 3;
    if (i < n && f[i] == ':') i++;
    if (i < n) {
        if (n != i + 2 || (unsigned)(f[i] - '0') > 9 || (unsigned)(f[i + 1] - '0') > 9) return 0;
        mm = (f[i] - '0') * 10 + (f[i + 1] - '0');
    }
    if (hh > 23 || mm > 59) return 0;
    *out = (int64_t)sign * ((int64_t)hh * 3600 + (int64_t)mm * 60) * 1000000000LL;
    return 1;
}

/* the whole cell as YYYY-MM-DD{T| }HH:MM:SS[.digits][tz] -> ns since 2000.01.01 */
static int csv_ts_cell(const char* f, size_t n, int64_t* out) {
    int32_t d;
    if (n < 19 || !csv_date10(f, &d) || (f[10] != 'T' && f[10] != ' ')) return 0;
    int64_t tns;
    size_t used;
    if (!csv_time_part(f + 11, n - 11, &tns, &used)) return 0;
    size_t off = 11 + used;
    if (off < n) {
        int64_t adj;
        if (!csv_tz_ns(f + off, n - off, &adj)) return 0;
        tns -= adj;                                /* to UTC before the exact compose */
    }
    return q_calendar_ts_compose_checked(d, tns, out);
}

static ct_t csv_detect(const char* f, size_t n) {
    if (csv_null_tok(f, n)) return CT_UNKNOWN;
    if (n == 3 && ((f[0] == 'n' || f[0] == 'N') ? ((f[1] == 'a' || f[1] == 'A') && (f[2] == 'n' || f[2] == 'N'))
                                                : ((f[0] == 'i' || f[0] == 'I') && (f[1] == 'n' || f[1] == 'N') &&
                                                   (f[2] == 'f' || f[2] == 'F'))))
        return CT_F64;
    if (n == 4 && (f[0] == '+' || f[0] == '-') &&
        (f[1] == 'i' || f[1] == 'I') && (f[2] == 'n' || f[2] == 'N') && (f[3] == 'f' || f[3] == 'F'))
        return CT_F64;
    if ((n == 4 && (!memcmp(f, "true", 4) || !memcmp(f, "TRUE", 4))) ||
        (n == 5 && (!memcmp(f, "false", 5) || !memcmp(f, "FALSE", 5))))
        return CT_BOOL;
    const char* p = f;
    const char* end = f + n;
    if (*p == '-' || *p == '+') p++;
    int has_dot = 0, has_e = 0, has_digit = 0, exp_digit = 0;
    while (p < end) {
        unsigned char c = (unsigned char)*p;
        if (c >= '0' && c <= '9') {
            if (has_e) exp_digit = 1; else has_digit = 1;
            p++;
            continue;
        }
        if (c == '.' && !has_dot && !has_e) { has_dot = 1; p++; continue; }
        if ((c == 'e' || c == 'E') && !has_e && has_digit) {
            has_e = 1; p++;
            if (p < end && (*p == '-' || *p == '+')) p++;
            continue;
        }
        break;
    }
    if (p == end && has_digit && (!has_e || exp_digit)) {
        if (has_dot || has_e) return CT_F64;
        int64_t iv;
        size_t u = ray_parse_i64(f, n, &iv);
        return u == n ? CT_I64 : CT_F64;           /* past the long domain it reads as float, like "F"$ */
    }
    /* the temporal shapes go through the SAME validators the frozen parsers use
     * (csv_date10 / csv_time_cell / csv_ts_cell): whatever this function types,
     * csv_cell_atom must accept — a near-miss must sniff as text */
    if (n >= 10 && f[4] == '-' && f[7] == '-') {
        int32_t d;
        int64_t v;
        if (n == 10 && csv_date10(f, &d)) return CT_DATE;
        if (csv_ts_cell(f, n, &v)) return CT_TS;
    }
    if (n >= 8 && f[2] == ':' && f[5] == ':') {
        int64_t v;
        if (csv_time_cell(f, n, &v)) return CT_TIME;
    }
    return CT_STR;
}

static ct_t csv_promote(ct_t cur, ct_t obs) {
    if (cur == CT_UNKNOWN) return obs;
    if (obs == CT_UNKNOWN || cur == obs) return cur;
    if (cur == CT_STR || obs == CT_STR) return CT_STR;
    if ((cur == CT_DATE && obs == CT_TS) || (cur == CT_TS && obs == CT_DATE)) return CT_TS;
    if (cur <= CT_F64 && obs <= CT_F64) return cur > obs ? cur : obs;
    return CT_STR;
}

static void csv_card_note(card_t* c, const char* f, size_t n) {
    if (n == 0) return;
    uint32_t h = (uint32_t)ray_hash_bytes(f, n);
    uint16_t l = n > UINT16_MAX ? UINT16_MAX : (uint16_t)n;
    for (uint16_t i = 0; i < c->distinct; i++)
        if (c->h[i] == h && c->l[i] == l) { c->non_null++; return; }
    if (c->distinct < CSV_CARD_CAP) {
        c->h[c->distinct] = h;
        c->l[c->distinct] = l;
        c->distinct++;
    }
    c->non_null++;
}

/* Sniffed text is ALWAYS a string column: type-as-cardinality made same-shaped files
 * sniff different schemas and default-interned into the global sym table.  Sym-ness
 * is a schema decision, so the cardinality sample survives only as ADVICE — `advise`
 * (the .csv.info lane) answers 's' where the quarry's 64/80% rule suggests one, and
 * the read lane never syms on its own. */
static char csv_resolve(ct_t t, const card_t* c, int advise) {
    switch (t) {
        case CT_BOOL: return 'b';
        case CT_I64:  return 'j';
        case CT_F64:  return 'f';
        case CT_DATE: return 'd';
        case CT_TIME: return 't';
        case CT_TS:   return 'p';
        case CT_STR:
            if (!advise) return '*';
            return (c->non_null >= 64 && (uint32_t)c->distinct * 100u >= c->non_null * 80u) ? '*' : 's';
        default:      return advise ? 's' : '*';   /* nothing observed */
    }
}

/* ---- the frozen-type cell parser ------------------------------------------- */

/* one cell -> an OWNED atom under its FROZEN type char, or 'csv */
static ray_t* csv_cell_atom(char c, const char* f, size_t n) {
    if (c == 's') return ray_sym(ray_sym_intern_runtime(f, n));
    if (c == '*') return ray_charv(f, (int64_t)n);
    if (csv_null_tok(f, n)) {
        switch (c) {
            case 'b': return ray_bool(0);          /* q booleans have no null — "B"$"" is 0b */
            case 'j': return ray_typed_null(-RAY_I64);
            case 'f': return ray_typed_null(-RAY_F64);
            case 'd': return ray_typed_null(-RAY_DATE);
            case 't': return ray_typed_null(-RAY_TIME);
            default:  return ray_typed_null(-RAY_TIMESTAMP);
        }
    }
    switch (c) {
        case 'b': {
            if ((n == 4 && (!memcmp(f, "true", 4) || !memcmp(f, "TRUE", 4))) || (n == 1 && f[0] == '1'))
                return ray_bool(1);
            if ((n == 5 && (!memcmp(f, "false", 5) || !memcmp(f, "FALSE", 5))) || (n == 1 && f[0] == '0'))
                return ray_bool(0);
            return q_err(QE_CSV);
        }
        case 'j': {
            int64_t v;
            size_t u = ray_parse_i64(f, n, &v);
            return (u && u == n) ? ray_i64(v) : q_err(QE_CSV);
        }
        case 'f': {
            double v;
            size_t u = ray_parse_f64(f, n, &v);
            if (!u || u != n) return q_err(QE_CSV);
            return v != v ? ray_typed_null(-RAY_F64) : ray_f64(v);
        }
        case 'd': {
            int32_t d;
            if (n != 10 || !csv_date10(f, &d)) return q_err(QE_CSV);
            return ray_date(d);
        }
        case 't': {
            int64_t ns;
            if (!csv_time_cell(f, n, &ns)) return q_err(QE_CSV);
            return ray_time(ns / 1000000);         /* RAY_TIME is ms since midnight */
        }
        case 'p': {
            int32_t d;
            int64_t v;
            if (n == 10 && csv_date10(f, &d))      /* a date cell in a p column reads as midnight */
                return q_calendar_ts_compose_checked(d, 0, &v) ? ray_timestamp(v) : q_err(QE_CSV);
            if (!csv_ts_cell(f, n, &v)) return q_err(QE_CSV);
            return ray_timestamp(v);
        }
        default: return q_err(QE_CSV);
    }
}

/* ---- one row -> fields ----------------------------------------------------- */

typedef ray_t* (*csv_cell_fn)(csv_st*, int64_t col, const char* f, size_t n);

/* One field at *pp.  RFC-4180 quoted fields un-escape "" (into esc, or a malloc'd
 * *dyn past CSV_ESC_STACK); bytes between a closing quote and the delimiter are
 * 'csv.  *ate says a delimiter was consumed, so a trailing one yields the empty
 * final field on the next call. */
static ray_t* csv_field(const char** pp, const char* end, char delim,
                        const char** out, size_t* outn, char* esc, char** dyn, int* ate) {
    const char* p = *pp;
    *ate = 0;
    if (p < end && *p == '"') {
        p++;
        const char* s = p;
        int esc_seen = 0;
        while (p < end) {
            if (*p != '"') { p++; continue; }
            if (p + 1 < end && p[1] == '"') { esc_seen = 1; p += 2; }
            else break;
        }
        if (p >= end) return q_err(QE_CSV);        /* no closing quote */
        size_t raw = (size_t)(p - s);
        p++;
        if (p < end && *p != delim) return q_err(QE_CSV);
        if (esc_seen) {
            char* d = esc;
            if (raw > CSV_ESC_STACK) {
                d = (char*)malloc(raw);
                if (!d) return q_err(QE_WSFULL);
                *dyn = d;
            }
            size_t o = 0;
            for (const char* q = s; q < s + raw; q++) {
                d[o++] = *q;
                if (*q == '"') q++;                /* the doubled twin */
            }
            *out = d;
            *outn = o;
        } else {
            *out = s;
            *outn = raw;
        }
    } else {
        const char* s = p;
        while (p < end && *p != delim) {
            /* RFC-4180: no quote inside an unquoted field — and the row-boundary
             * parity scan already counted it, so accepting it would silently
             * shift every later row boundary */
            if (*p == '"') return q_err(QE_CSV);
            p++;
        }
        *out = s;
        *outn = (size_t)(p - s);
    }
    if (p < end) { p++; *ate = 1; }
    *pp = p;
    return NULL;
}

static ray_t* csv_row_each(csv_st* st, const char* p, size_t n, csv_cell_fn fn, int64_t* ncols_out) {
    const char* end = p + n;
    char esc[CSV_ESC_STACK];
    int64_t col = 0;
    for (;;) {
        const char* f;
        size_t fl;
        char* dyn = NULL;
        int ate = 0;
        ray_t* bad = csv_field(&p, end, st->delim, &f, &fl, esc, &dyn, &ate);
        if (!bad) bad = fn(st, col, f, fl);
        free(dyn);
        if (bad) return bad;
        col++;
        if (!ate) break;
    }
    *ncols_out = col;
    return NULL;
}

/* ---- the frozen-phase row parser ------------------------------------------- */

static ray_t* csv_cell_parse(csv_st* st, int64_t col, const char* f, size_t n) {
    if (col >= st->ncols) return q_err(QE_CSV);
    int64_t k = st->kidx[col];
    if (k < 0) return NULL;
    ray_t* a = csv_cell_atom(st->kchars[k], f, n);
    if (!a) return q_err(QE_OOM);
    if (RAY_IS_ERR(a)) return a;
    st->acc[k] = ray_list_append(st->acc[k], a);
    ray_release(a);
    if (RAY_IS_ERR(st->acc[k])) {
        ray_t* e = st->acc[k];
        st->acc[k] = NULL;
        return e;
    }
    return NULL;
}

static ray_t* csv_parse_row(csv_st* st, const char* p, size_t n) {
    int64_t cnt = 0;
    ray_t* bad = csv_row_each(st, p, n, csv_cell_parse, &cnt);
    if (bad) return bad;
    if (cnt != st->ncols) return q_err(QE_CSV);
    st->rows_total++;
    st->pend++;
    return NULL;
}

/* ---- freeze: header + types from the sniff sample -------------------------- */

static ray_t* csv_cell_hdr(csv_st* st, int64_t col, const char* f, size_t n) {
    (void)col;
    ray_t* c = ray_charv(f, (int64_t)n);
    if (RAY_IS_ERR(c)) return c;
    st->f_hdr = ray_list_append(st->f_hdr, c);
    ray_release(c);
    if (RAY_IS_ERR(st->f_hdr)) {
        ray_t* e = st->f_hdr;
        st->f_hdr = NULL;
        return e;
    }
    return NULL;
}

static ray_t* csv_cell_detect(csv_st* st, int64_t col, const char* f, size_t n) {
    if (col >= st->ncols) return q_err(QE_CSV);
    st->f_ct[col] = csv_promote(st->f_ct[col], csv_detect(f, n));
    csv_card_note(&st->f_card[col], f, n);
    return NULL;
}

/* 'b'jfsdtp / '*' / ' ', upper case (the `0:` spelling) folded down; 0 = unknown */
static char csv_type_canon(char c) {
    if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    return strchr("bjfsdtp* ", c) && c ? c : 0;
}

/* merge the user's types argument onto the sniffed chars */
static ray_t* csv_types_apply(csv_st* st) {
    ray_t* ty = st->types_arg;
    if (!ty) return NULL;
    if (ty->type == RAY_CHARV || ty->type == -RAY_CHARV) {
        int64_t n = ty->type == RAY_CHARV ? ray_len(ty) : 1;
        if (n != st->ncols) return q_err(QE_LENGTH);
        const char* p = ty->type == RAY_CHARV ? (const char*)ray_data(ty) : (const char*)&ty->u8;
        for (int64_t j = 0; j < n; j++) {
            char c = csv_type_canon(p[j]);
            if (!c) return q_err(QE_TYPE);
            st->ctypes[j] = c;
        }
        return NULL;
    }
    /* dict: column sym -> type char */
    ray_t* ks = ray_dict_keys(ty);
    ray_t* vs = ray_dict_vals(ty);
    int64_t n = ks ? ray_len(ks) : 0;
    for (int64_t i = 0; i < n; i++) {
        ray_t* ia = ray_i64(i);
        ray_t* k = ray_at_fn(ks, ia);
        ray_t* v = ray_at_fn(vs, ia);
        ray_release(ia);
        ray_t* bad = NULL;
        if (!k || !v || RAY_IS_ERR(k) || RAY_IS_ERR(v)) bad = q_err(QE_OOM);
        else if (k->type != -RAY_SYM || v->type != -RAY_CHARV) bad = q_err(QE_TYPE);
        else {
            char c = csv_type_canon((char)v->u8);
            if (!c) bad = q_err(QE_TYPE);
            else {
                int64_t j = 0;
                while (j < st->ncols && st->names[j] != k->i64) j++;
                if (j == st->ncols) bad = q_err(QE_DOMAIN);
                else st->ctypes[j] = c;
            }
        }
        if (k && !RAY_IS_ERR(k)) ray_release(k);
        if (v && !RAY_IS_ERR(v)) ray_release(v);
        if (bad) return bad;
    }
    return NULL;
}

static int64_t csv_tgt_find(const csv_st* st, int64_t name) {
    for (int64_t k = 0; k < st->tgt_n; k++)
        if (st->tgt_names[k] == name) return k;
    return -1;
}

/* An EXISTING symbol target fixes the parse map: per-column chars from its meta
 * (key table first for a keyed target, which also routes emission to upsert).
 * A list/str column is the string COLUMN -> '*' (the tag<->char owner's chars
 * serve every other tag; one without a parser arm aborts 'csv at its first cell). */
static ray_t* csv_target_schema(csv_st* st, ray_t* g) {
    ray_t* parts[2] = { g, NULL };
    if (q_type_is_keyed(g)) {
        parts[0] = ray_dict_keys(g);
        parts[1] = ray_dict_vals(g);
        st->use_upsert = 1;
    }
    int64_t n = ray_table_ncols(parts[0]) + (parts[1] ? ray_table_ncols(parts[1]) : 0);
    st->tgt_names = (int64_t*)malloc((size_t)n * sizeof(int64_t));
    st->tgt_chars = (char*)malloc((size_t)n);
    if (!st->tgt_names || !st->tgt_chars) return q_err(QE_WSFULL);
    int64_t k = 0;
    for (int p = 0; p < 2 && parts[p]; p++)
        for (int64_t c = 0; c < ray_table_ncols(parts[p]); c++, k++) {
            st->tgt_names[k] = ray_table_col_name(parts[p], c);
            ray_t* col = ray_table_get_col_idx(parts[p], c);   /* borrowed */
            st->tgt_chars[k] = (col->type == RAY_LIST || col->type == RAY_STR) ? '*' : q_type_char(col->type);
        }
    st->tgt_n = n;
    st->has_tgt = 1;
    return NULL;
}

static ray_t* csv_freeze(csv_st* st) {
    int64_t m = st->sniff ? ray_len(st->sniff) : 0;
    if (m == 0) return q_err(QE_CSV);
    ray_t** rows = (ray_t**)ray_data(st->sniff);
    st->f_hdr = ray_list_new(8);
    if (RAY_IS_ERR(st->f_hdr)) {
        ray_t* e = st->f_hdr;
        st->f_hdr = NULL;
        return e;
    }
    int64_t nc = 0;
    ray_t* bad = csv_row_each(st, (const char*)ray_data(rows[0]), (size_t)ray_len(rows[0]), csv_cell_hdr, &nc);
    if (bad) return bad;
    st->ncols = nc;
    st->names = (int64_t*)malloc((size_t)nc * sizeof(int64_t));
    st->ctypes = (char*)malloc((size_t)nc);
    st->f_ct = (ct_t*)calloc((size_t)nc, sizeof(ct_t));
    st->f_card = (card_t*)calloc((size_t)nc, sizeof(card_t));
    st->kidx = (int64_t*)malloc((size_t)nc * sizeof(int64_t));
    st->knames = (int64_t*)malloc((size_t)nc * sizeof(int64_t));
    st->kchars = (char*)malloc((size_t)nc);
    if (!st->names || !st->ctypes || !st->f_ct || !st->f_card || !st->kidx || !st->knames || !st->kchars)
        return q_err(QE_WSFULL);
    ray_t** hf = (ray_t**)ray_data(st->f_hdr);
    for (int64_t i = 1; i < m; i++) {              /* sniff the sure data rows; row 0 is judged below */
        int64_t cnt = 0;
        bad = csv_row_each(st, (const char*)ray_data(rows[i]), (size_t)ray_len(rows[i]), csv_cell_detect, &cnt);
        if (!bad && cnt != nc) bad = q_err(QE_CSV);
        if (bad) return bad;
    }
    int header = st->header;
    if (header < 0) {
        /* all row-0 cells textual = header; so is ONE textual cell over a column the
         * data sniffed numeric/temporal (a year-pivot header like "id,2024,2025") —
         * a demotion the data alone would never produce */
        int all_str = 1, demote = 0;
        for (int64_t j = 0; j < nc; j++) {
            ct_t d0 = csv_detect((const char*)ray_data(hf[j]), (size_t)ray_len(hf[j]));
            if (d0 != CT_STR) all_str = 0;
            else if (st->f_ct[j] != CT_UNKNOWN && st->f_ct[j] != CT_STR) demote = 1;
        }
        header = all_str || demote;
    }
    if (!header) {                                 /* row 0 is data after all: fold it into the sniff */
        int64_t cnt = 0;
        bad = csv_row_each(st, (const char*)ray_data(rows[0]), (size_t)ray_len(rows[0]), csv_cell_detect, &cnt);
        if (!bad && cnt != nc) bad = q_err(QE_CSV);
        if (bad) return bad;
    }
    for (int64_t j = 0; j < nc; j++) {
        int64_t hl = header ? ray_len(hf[j]) : 0;
        if (hl > 0) st->names[j] = ray_sym_intern_runtime((const char*)ray_data(hf[j]), (size_t)hl);
        else {                                     /* headerless (or an empty header cell): x, x1, x2… */
            char b[24];
            int bl = j ? snprintf(b, sizeof b, "x%lld", (long long)j) : (b[0] = 'x', 1);
            st->names[j] = ray_sym_intern_runtime(b, (size_t)bl);
        }
    }
    for (int64_t j = 0; j < nc; j++)
        for (int64_t i = 0; i < j; i++)
            if (st->names[i] == st->names[j]) return q_err(QE_CSV);   /* duplicate column name */
    int64_t first = header ? 1 : 0;
    for (int64_t j = 0; j < nc; j++) st->ctypes[j] = csv_resolve(st->f_ct[j], &st->f_card[j], st->info_only);
    if (st->has_tgt) {
        /* the target's schema outranks the sniff: by NAME with a header (a CSV column
         * the target lacks is projected to ' ' — the subsetting rider), positionally
         * and projection-free without one — position MEANS the target column, so the
         * batch takes its name too and the name-matching insert seam still lands it */
        for (int64_t j = 0; j < nc; j++) {
            int64_t k = csv_tgt_find(st, st->names[j]);
            if (header) st->ctypes[j] = k >= 0 ? st->tgt_chars[k] : ' ';
            else if (j < st->tgt_n) {
                st->ctypes[j] = st->tgt_chars[j];
                st->names[j] = st->tgt_names[j];
            }
        }
    }
    bad = csv_types_apply(st);
    if (bad) return bad;
    if (st->has_tgt && st->types_arg) {
        /* explicit types must AGREE with the target where both speak: any change
         * types_apply made to a target-mapped column is a conflict — 'mismatch
         * EARLY, before a single cell parses.  An explicit ' ' drop passes: the
         * missing column is then the insert seam's own contract to refuse. */
        for (int64_t j = 0; j < nc; j++) {
            int64_t k = header ? csv_tgt_find(st, st->names[j]) : (j < st->tgt_n ? j : -1);
            if (k >= 0 && st->ctypes[j] != ' ' && st->ctypes[j] != st->tgt_chars[k])
                return q_err(QE_MISMATCH);
        }
    }
    if (st->has_tgt && header) {                   /* the rider's report: what never landed */
        st->ign = (int64_t*)malloc((size_t)nc * sizeof(int64_t));
        if (!st->ign) return q_err(QE_WSFULL);
        for (int64_t j = 0; j < nc; j++)
            if (st->ctypes[j] == ' ' && csv_tgt_find(st, st->names[j]) < 0)
                st->ign[st->nign++] = st->names[j];
    }
    st->nkept = 0;
    for (int64_t j = 0; j < nc; j++) {
        if (st->ctypes[j] == ' ') { st->kidx[j] = -1; continue; }
        st->kidx[j] = st->nkept;
        st->knames[st->nkept] = st->names[j];
        st->kchars[st->nkept] = st->ctypes[j];
        st->nkept++;
    }
    if (st->nkept == 0) return q_err(QE_DOMAIN);   /* a schema of nothing but skips */
    st->acc = (ray_t**)calloc((size_t)st->nkept, sizeof(ray_t*));
    if (!st->acc) return q_err(QE_WSFULL);
    for (int64_t k = 0; k < st->nkept; k++) {
        st->acc[k] = ray_list_new(64);
        if (RAY_IS_ERR(st->acc[k])) {
            ray_t* e = st->acc[k];
            st->acc[k] = NULL;
            return e;
        }
    }
    st->frozen = 1;
    if (!st->info_only)
        for (int64_t i = first; i < m; i++) {
            bad = csv_parse_row(st, (const char*)ray_data(rows[i]), (size_t)ray_len(rows[i]));
            if (bad) return bad;
        }
    ray_release(st->sniff);
    st->sniff = NULL;
    ray_release(st->f_hdr);
    st->f_hdr = NULL;
    free(st->f_ct);
    st->f_ct = NULL;
    free(st->f_card);
    st->f_card = NULL;
    return NULL;
}

/* ---- the carry: bytes in, complete rows out -------------------------------- */

static ray_t* csv_line(csv_st* st, const char* p, size_t n) {
    if (n == 0) return NULL;                       /* empty lines are skipped */
    if (st->frozen) return st->info_only ? NULL : csv_parse_row(st, p, n);
    if (!st->sniff) {
        st->sniff = ray_list_new(64);
        if (RAY_IS_ERR(st->sniff)) {
            ray_t* e = st->sniff;
            st->sniff = NULL;
            return e;
        }
    }
    ray_t* r = ray_charv(p, (int64_t)n);
    if (RAY_IS_ERR(r)) return r;
    st->sniff = ray_list_append(st->sniff, r);
    ray_release(r);
    if (RAY_IS_ERR(st->sniff)) {
        ray_t* e = st->sniff;
        st->sniff = NULL;
        return e;
    }
    return ray_len(st->sniff) >= st->sample ? csv_freeze(st) : NULL;
}

static ray_t* csv_feed(csv_st* st, const char* p, int64_t n) {
    if (st->clen + (size_t)n > st->ccap) {
        size_t cap = st->ccap ? st->ccap : 4096;
        while (cap < st->clen + (size_t)n) cap *= 2;
        char* nb = (char*)realloc(st->carry, cap);
        if (!nb) return q_err(QE_WSFULL);
        st->carry = nb;
        st->ccap = cap;
    }
    memcpy(st->carry + st->clen, p, (size_t)n);
    st->clen += (size_t)n;
    size_t rs = 0;
    for (size_t i = st->scan; i < st->clen; i++) {
        char c = st->carry[i];
        if (c == '"') st->in_quote = !st->in_quote;
        else if ((c == '\n' || c == '\r') && !st->in_quote) {
            /* LF, CRLF and bare CR all terminate a row: CR ends it here and a
             * following LF makes an empty row, which the empty-line skip absorbs */
            ray_t* bad = csv_line(st, st->carry + rs, i - rs);
            if (bad) return bad;
            rs = i + 1;
        }
    }
    memmove(st->carry, st->carry + rs, st->clen - rs);
    st->clen -= rs;
    st->scan = st->clen;
    return NULL;
}

static ray_t* csv_finish(csv_st* st) {
    if (st->clen) {
        if (st->in_quote) return q_err(QE_CSV);    /* unterminated quote at EOF */
        ray_t* bad = csv_line(st, st->carry, st->clen);
        if (bad) return bad;
        st->clen = st->scan = 0;
    }
    return st->frozen ? NULL : csv_freeze(st);
}

/* ---- batches and sinks ----------------------------------------------------- */

static ray_t* csv_typed_empty(char c) {
    /* '*' is the string COLUMN's list container — cell-vs-container is CSV
     * knowledge; everything else is the tag<->char owner's composition */
    return c == '*' ? ray_list_new(1) : q_type_empty(q_type_of_char(c));
}

/* the pending rows as an OWNED table; accumulators reset for the next batch */
static ray_t* csv_flush_tbl(csv_st* st) {
    ray_t* tbl = ray_table_new(st->nkept);
    if (RAY_IS_ERR(tbl)) return tbl;
    for (int64_t k = 0; k < st->nkept; k++) {
        ray_t* col = ray_len(st->acc[k]) ? q_list_collapse(st->acc[k]) : csv_typed_empty(st->kchars[k]);
        if (!col || RAY_IS_ERR(col)) {
            ray_release(tbl);
            return col ? col : q_err(QE_OOM);
        }
        tbl = ray_table_add_col(tbl, st->knames[k], col);
        ray_release(col);
        if (RAY_IS_ERR(tbl)) return tbl;
        ray_release(st->acc[k]);
        st->acc[k] = ray_list_new(64);
        if (RAY_IS_ERR(st->acc[k])) {
            ray_t* e = st->acc[k];
            st->acc[k] = NULL;
            ray_release(tbl);
            return e;
        }
    }
    st->pend = 0;
    st->batches++;
    return tbl;
}

static ray_t* csv_emit(csv_st* st) {
    ray_t* tbl = csv_flush_tbl(st);
    if (RAY_IS_ERR(tbl)) return tbl;
    ray_t* r;
    if (st->sink_kind == 1) r = st->use_upsert ? q_upsert_wrap(st->sink, tbl) : q_insert_wrap(st->sink, tbl);
    else {
        ray_t* ed = ray_list_new(0);               /* errData: () until the rejects PR */
        if (RAY_IS_ERR(ed)) { ray_release(tbl); return ed; }
        ray_t* cargs[3] = { tbl, ed, RAY_NULL_OBJ };
        r = q_eval_apply_value(st->sink, cargs, 3);
        ray_release(ed);
    }
    ray_release(tbl);
    if (!r) return q_err(QE_OOM);
    if (RAY_IS_ERR(r)) return r;
    ray_release(r);
    return NULL;
}

/* ---- options + arguments --------------------------------------------------- */

static int csv_sym_is(int64_t sym, const char* name) {
    ray_t* s = ray_sym_str(sym);                   /* borrowed */
    if (!s) return 0;
    size_t n = ray_str_len(s);
    return n == strlen(name) && !memcmp(ray_str_ptr(s), name, n);
}

static ray_t* csv_opts(csv_st* st, ray_t* opts) {
    if (!opts || opts->type == RAY_NULL) return NULL;
    if (opts->type != RAY_DICT) return q_err(QE_TYPE);
    ray_t* ks = ray_dict_keys(opts);
    ray_t* vs = ray_dict_vals(opts);
    int64_t n = ks ? ray_len(ks) : 0;
    for (int64_t i = 0; i < n; i++) {
        ray_t* ia = ray_i64(i);
        ray_t* k = ray_at_fn(ks, ia);
        ray_t* v = ray_at_fn(vs, ia);
        ray_release(ia);
        ray_t* bad = NULL;
        if (!k || !v || RAY_IS_ERR(k) || RAY_IS_ERR(v)) bad = q_err(QE_OOM);
        else if (k->type != -RAY_SYM) bad = q_err(QE_TYPE);
        else if (csv_sym_is(k->i64, "")) {
            /* the empty-sym key is padding, value unread — legalizes the short-dict
             * idiom ``delim!(::;";") without weakening the never-ignored law */
        } else if (csv_sym_is(k->i64, "delim")) {
            if (v->type != -RAY_CHARV) bad = q_err(QE_TYPE);
            else if (v->u8 == '"' || v->u8 == '\n' || v->u8 == '\r')
                bad = q_err(QE_DOMAIN);            /* the grammar's own metacharacters cannot delimit */
            else st->delim = (char)v->u8;
        } else if (csv_sym_is(k->i64, "header")) {
            if (v->type == -RAY_BOOL) st->header = v->u8 != 0;
            else bad = q_err(QE_TYPE);
        } else if (csv_sym_is(k->i64, "sample_size") || csv_sym_is(k->i64, "buffer_size")) {
            int64_t x;
            if (!q_type_strict_i64(v, &x)) bad = q_err(QE_TYPE);
            else if (x <= 0) bad = q_err(QE_DOMAIN);
            else if (csv_sym_is(k->i64, "sample_size")) st->sample = x;
            else st->bufsz = x;
        } else {
            /* skip/comment/nullstr/all_varchar/store_rejects/rejects_table are ratified-but-later;
             * they and unknown keys signal alike — an option is NEVER silently ignored */
            bad = q_err(QE_OPTION);
        }
        if (k && !RAY_IS_ERR(k)) ray_release(k);
        if (v && !RAY_IS_ERR(v)) ray_release(v);
        if (bad) return bad;
    }
    return NULL;
}

static void csv_free(csv_st* st) {
    free(st->carry);
    free(st->names);
    free(st->ctypes);
    free(st->kidx);
    free(st->knames);
    free(st->kchars);
    free(st->f_ct);
    free(st->f_card);
    free(st->tgt_names);
    free(st->tgt_chars);
    free(st->ign);
    if (st->sniff) ray_release(st->sniff);
    if (st->f_hdr) ray_release(st->f_hdr);
    if (st->acc) {
        for (int64_t k = 0; k < st->nkept; k++)
            if (st->acc[k]) ray_release(st->acc[k]);
        free(st->acc);
    }
}

static ray_t* csv_run(csv_st* st, ray_t* file) {
    ray_t* path = q_io_file_path(file);
    if (!path) return q_err(QE_TYPE);
    int64_t off = 0;
    ray_t* bad = NULL;
    for (;;) {
        ray_t* b = q_io_read_slice(path, off, st->bufsz, NULL);
        if (!b) { bad = q_err(QE_OOM); break; }
        if (RAY_IS_ERR(b)) { bad = b; break; }
        int64_t got = ray_len(b);
        if (got > 0) bad = csv_feed(st, (const char*)ray_data(b), got);
        ray_release(b);
        if (bad) break;
        if (st->frozen && st->sink_kind && st->pend > 0) {
            bad = csv_emit(st);
            if (bad) break;
        }
        off += got;
        if (got < st->bufsz || (st->info_only && st->frozen)) break;
    }
    if (!bad) bad = csv_finish(st);
    ray_release(path);
    return bad;
}

/* ---- results --------------------------------------------------------------- */

static ray_t* csv_types_dict(const int64_t* names, const char* chars, int64_t n) {
    ray_t* kl = ray_list_new(n);
    if (RAY_IS_ERR(kl)) return kl;
    for (int64_t i = 0; i < n; i++) {
        ray_t* s = ray_sym(names[i]);
        kl = ray_list_append(kl, s);
        ray_release(s);
        if (RAY_IS_ERR(kl)) return kl;
    }
    ray_t* keys = q_list_collapse(kl);
    ray_release(kl);
    if (!keys || RAY_IS_ERR(keys)) return keys ? keys : q_err(QE_OOM);
    ray_t* vals = ray_charv(chars, n);
    if (RAY_IS_ERR(vals)) { ray_release(keys); return vals; }
    return ray_dict_new(keys, vals);               /* consumes both */
}

/* the rider-dropped CSV column names as a sym vector (typed-empty when none) */
static ray_t* csv_ignored_syms(const csv_st* st) {
    if (!st->nign) return ray_sym_vec_new(RAY_SYM_W64, 1);
    ray_t* l = ray_list_new(st->nign);
    if (RAY_IS_ERR(l)) return l;
    for (int64_t i = 0; i < st->nign; i++) {
        ray_t* s = ray_sym(st->ign[i]);
        l = ray_list_append(l, s);
        ray_release(s);
        if (RAY_IS_ERR(l)) return l;
    }
    ray_t* v = q_list_collapse(l);
    ray_release(l);
    return v ? v : q_err(QE_OOM);
}

static ray_t* csv_summary(csv_st* st) {
    ray_t* td = csv_types_dict(st->knames, st->kchars, st->nkept);
    if (!td || RAY_IS_ERR(td)) return td ? td : q_err(QE_OOM);
    ray_t* iv = csv_ignored_syms(st);
    if (RAY_IS_ERR(iv)) { ray_release(td); return iv; }
    static const char* const kn[] = { "rows", "rejected", "chunks", "ignored", "types" };
    ray_t* kl = ray_list_new(5);
    ray_t* vl = ray_list_new(5);
    if (RAY_IS_ERR(kl) || RAY_IS_ERR(vl)) {
        if (!RAY_IS_ERR(kl)) ray_release(kl);
        if (!RAY_IS_ERR(vl)) ray_release(vl);
        ray_release(td);
        ray_release(iv);
        return q_err(QE_OOM);
    }
    ray_t* vs[5] = { ray_i64(st->rows_total), ray_i64(0), ray_i64(st->batches), iv, td };
    for (int i = 0; i < 5 && !RAY_IS_ERR(kl) && !RAY_IS_ERR(vl); i++) {
        ray_t* s = ray_sym(ray_sym_intern_runtime(kn[i], strlen(kn[i])));
        kl = ray_list_append(kl, s);
        ray_release(s);
        vl = ray_list_append(vl, vs[i]);
    }
    for (int i = 0; i < 5; i++) ray_release(vs[i]);
    if (RAY_IS_ERR(kl) || RAY_IS_ERR(vl)) {
        ray_t* e = RAY_IS_ERR(kl) ? kl : vl;
        if (!RAY_IS_ERR(kl)) ray_release(kl);
        if (!RAY_IS_ERR(vl)) ray_release(vl);
        return e;
    }
    ray_t* keys = q_list_collapse(kl);
    ray_release(kl);
    if (!keys || RAY_IS_ERR(keys)) { ray_release(vl); return keys ? keys : q_err(QE_OOM); }
    return ray_dict_new(keys, vl);
}

/* ---- the natives ----------------------------------------------------------- */

static void csv_init(csv_st* st) {
    memset(st, 0, sizeof *st);
    st->delim = ',';
    st->header = -1;
    st->sample = CSV_DEF_SAMPLE;
    st->bufsz = CSV_DEF_BUFFER;
}

static ray_t* csv_arg_val(ray_t* x) {
    return (x && x->type != RAY_NULL) ? x : NULL;  /* :: and elided read as default */
}

/* .csv.i.read[file;target;types;opts] */
static ray_t* csv_read_fn(ray_t** args, int64_t n) {
    if (n != 4) return q_err(QE_RANK);
    csv_st st;
    csv_init(&st);
    ray_t* target = csv_arg_val(args[1]);
    if (!target) st.sink_kind = 0;
    else if (target->type == -RAY_SYM) { st.sink_kind = 1; st.sink = target; }
    else if (q_eval_apply_is_fn(target)) {
        if (q_eval_apply_rank(target) != 3) return q_err(QE_RANK);
        st.sink_kind = 2;
        st.sink = target;
    } else return q_err(QE_TYPE);
    ray_t* ty = csv_arg_val(args[2]);
    if (ty && ty->type != RAY_CHARV && ty->type != -RAY_CHARV && ty->type != RAY_DICT)
        return q_err(QE_TYPE);
    st.types_arg = ty;
    ray_t* bad = NULL;
    if (st.sink_kind == 1) {
        ray_t* g = q_env_get(target->i64);         /* the same resolution insert itself uses */
        if (g && (g->type == RAY_TABLE || q_type_is_keyed(g)))
            bad = csv_target_schema(&st, g);
    }
    if (!bad) bad = csv_opts(&st, csv_arg_val(args[3]));
    if (!bad) bad = csv_run(&st, args[0]);
    ray_t* result = NULL;
    if (!bad) {
        if (st.sink_kind == 0) result = csv_flush_tbl(&st);
        else {
            /* flush the tail — and a zero-row file still delivers its schema once */
            if (st.pend > 0 || st.batches == 0) bad = csv_emit(&st);
            if (!bad) result = csv_summary(&st);
        }
    }
    csv_free(&st);
    return bad ? bad : result;
}

/* .csv.i.info[file;opts] */
static ray_t* csv_info_fn(ray_t** args, int64_t n) {
    if (n != 2) return q_err(QE_RANK);
    csv_st st;
    csv_init(&st);
    st.info_only = 1;
    ray_t* bad = csv_opts(&st, csv_arg_val(args[1]));
    if (!bad) bad = csv_run(&st, args[0]);
    ray_t* result = bad ? bad : csv_types_dict(st.names, st.ctypes, st.ncols);
    csv_free(&st);
    return result;
}

static void csv_bind_fn(const char* name, ray_vary_fn fn) {
    ray_t* f = ray_fn_vary(name, RAY_FN_NONE, fn);
    q_env_bind(ray_sym_intern(name, strlen(name)), f);
    ray_release(f);
}

void q_csv_register(void) {
    csv_bind_fn(".csv.i.read", csv_read_fn);
    csv_bind_fn(".csv.i.info", csv_info_fn);
}
