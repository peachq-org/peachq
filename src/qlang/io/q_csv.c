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
 * closing quote and the delimiter — is 'csv too, under the DEFAULTS.  THE TOLERANCE LAW (csv PR-3): the
 * DuckDB-named levers ignore_errors / null_padding / strict_mode=0b / store_rejects govern whether the load
 * SURVIVES a bad row, NEVER whether it is counted — every bad row (skipped, padded or salvaged) leaves one
 * reject record (line, column, class, raw text; csv_reject_note is the ONE construction home) and bumps the
 * summary's `rejected` unconditionally.  Duplicate header names dedupe DuckDB-style (a, b, a_1), never an
 * abort and never a silent merge.  An option key is NEVER silently ignored: unknown or not-yet-implemented
 * -> 'option. */
#include "qlang/q_registry_internal.h" /* q_insert_wrap (the by-name row-append), q_list_collapse via q_prim */
#include "qlang/base/q_calendar.h" /* the ONE civil-calendar home: date validity + day/ts composition */
#include "qlang/base/q_err.h"
#include "qlang/eval/q_eval.h"  /* q_eval_apply_value/_is_fn/_rank — the lambda-target seam */
#include "qlang/io/q_csv.h"
#include "qlang/io/q_io.h"      /* the byte core: paths + the slice read */
#include "qlang/parse/q_tok.h"  /* the Tok scanners — THE spelling owners the sniffer delegates to */
#include "qlang/q_env.h"        /* q_env_bind — the .csv.i.* bindings */
#include "core/numparse.h"      /* ray_parse_i64/f64 */
#include "lang/env.h"           /* ray_fn_vary */
#include "lang/eval.h"          /* RAY_FN_NONE, ray_at_fn */
#include "ops/hash.h"           /* ray_hash_bytes — the cardinality sample */
#include "table/sym.h"          /* ray_sym_intern_runtime / ray_sym_str */
#include <rayforce.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CSV_DEF_SAMPLE 20480          /* sniff rows (header included) */
#define CSV_DEF_BUFFER (1 << 20)      /* read-chunk bytes */
#define CSV_CARD_CAP   100            /* distinct-sample cap per column */

/* reject classes, DuckDB's reject_errors error_type vocabulary folded to syms */
#define CSV_RJ_CAST     "cast"
#define CSV_RJ_TOOFEW   "toofewcolumns"
#define CSV_RJ_TOOMANY  "toomanycolumns"
#define CSV_RJ_UNQUOTED "unquotedvalue"
#define CSV_RJ_UNTERM   "unterminatedquote"
#define CSV_RJ_PADDED   "padded"      /* a null_padding row: salvaged INTO the data, still audited */

/* sniff lattice — TWO ordered runs are load-bearing: BOOL < I64 < F64 (numeric
 * promotion) and MINUTE < SECOND < TIME < TIMESPAN (the duration lattice:
 * the widest observed WRITTEN form wins, never truncation — kdb-formats law) */
typedef enum { CT_UNKNOWN = 0, CT_BOOL, CT_I64, CT_F64, CT_DATE, CT_MONTH, CT_TS,
               CT_MINUTE, CT_SECOND, CT_TIME, CT_TIMESPAN, CT_STR } ct_t;

typedef struct {
    uint32_t h[CSV_CARD_CAP];
    uint16_t l[CSV_CARD_CAP];
    uint16_t distinct;
    uint32_t non_null;
} card_t;

#define CSV_FMT_MAX 64

typedef struct { const char* p; size_t n; char* dyn; } csv_fld;

static ray_t* csv_typed_empty(char c);

typedef struct csv_st {
    char     delim;
    int      delim_explicit;
    char     quote, esc, cmt;     /* the dialect options (csv PR-4): explicit only, NEVER sniffed;
                                   * esc defaults to the quote char (RFC doubling); cmt 0 = disabled */
    int      quote_off;           /* quote="" — quotes are ordinary bytes */
    int      no_delim;            /* THE NARROWED FALLBACK: a one-column-shaped file loads whole lines */
    int      row_cut;             /* csv_field hit an unquoted comment char: the row ends here */
    int      scan_ct;             /* carry scan is inside a comment tail — opaque until the newline */
    char     datefmt[CSV_FMT_MAX];  /* dateformat / timestampformat options; empty = the default */
    char     tsfmt[CSV_FMT_MAX];    /* writer's-forms grammar, set = the format REPLACES it */
    int      header;              /* -1 = sniff, else forced 0/1 */
    int      info_only;
    int      frozen;
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
    int      ignore_err, null_pad, lenient, store_rej;   /* the tolerance levers; lenient = strict_mode 0b */
    int64_t  rej_name;            /* rejects_table sym; 0 = the reject_errors default */
    int64_t  line_cur;            /* 1-based physical line of the row in hand */
    int64_t  row_line;            /* the line the NEXT emitted row starts on */
    int64_t* sniff_lines;         /* per sniff row: its physical line */
    int      scan_fs;             /* carry scan: byte at `scan` sits at a field start */
    size_t   scan_probe;          /* quote-region probe resume point (keeps the scan O(n)) */
    csv_fld* fields;              /* the split of the row in hand */
    int64_t  nfields, fcap;
    ray_t**  rowvals;             /* per kept column: the row's staged atom (commit is all-or-nothing) */
    ray_t*   rj[4];               /* reject accumulators: line/column/error/csvLine */
    int64_t  rej_total, rej_flushed;
    const char* rej_class;        /* set at the failure site; non-NULL = the fault is rejectable */
    const char* salv_class;       /* lenient: the row's first quote salvage, recorded on commit */
} csv_st;

/* ---- sniffing (adapted from src/io/csv.c detect_type / promote_csv_type) --- */

/* the DEFAULT null is the EMPTY field only — DuckDB's default; richer
 * vocabularies (NA/NULL/…) arrive later behind the nullstr option */
static int csv_null_tok(const char* f, size_t n) {
    (void)f;
    return n == 0;
}

/* ---- temporal cell parsers — the writer's text forms define the grammar (owner
 * 2026-08-22).  Shape gates live here; SPELLING validity and payload delegate to
 * the ONE Tok home (q_tok.c), so a form never grows a second parser.  The same
 * helpers serve the sniffer and the frozen parsers: a cell can never detect as a
 * type its parser refuses ------------------------------------------------------ */

/* q_tok's date/ts/month/clock scanners cover the payloads; csv owns only what
 * the writer adds on top: the duration SIGN (Tok defers signs), the month `m`
 * suffix, the float 0w/-0w specials, and the trailing tz offset. */

/* true/false in any case (the `B` domain is case-insensitive; DuckDB agrees):
 * 1/0 truth value, or -1 = not a written bool */
static int csv_bool_tok(const char* f, size_t n) {
    static const char* const w[2] = { "false", "true" };
    for (int v = 0; v < 2; v++) {
        size_t wn = 5 - (size_t)v;
        if (n != wn) continue;
        size_t i = 0;
        while (i < wn && (f[i] | 0x20) == w[v][i]) i++;
        if (i == wn) return v;
    }
    return -1;
}

static int csv_date_cell(const char* f, size_t n, int32_t* out) {
    int64_t y, m, d;
    if (!q_tok_date(f, n, &y, &m, &d) || !q_calendar_date_valid(y, m, d)) return 0;
    *out = (int32_t)q_calendar_days_from_civil(y, m, d);
    return 1;
}

static int csv_month_cell(const char* f, size_t n, int64_t* months) {
    if (n && f[n - 1] == 'm') n--;                 /* the writer's suffix; Tok owns the payload */
    return q_tok_month(f, n, months);
}

/* Z | ±HH[[:]MM] at the head -> the bytes it consumed, 0 = no offset there.  The ONE tz-offset home,
 * shape and all, and it is reached only through %z: kdb parses no timezone, so the default grammar
 * never types an offset cell (owner 2026-08-23) */
static size_t csv_tz_ns(const char* f, size_t n, int64_t* out) {
    if (!n) return 0;
    if (f[0] == 'Z' || f[0] == 'z') { *out = 0; return 1; }
    int sign = f[0] == '+' ? 1 : f[0] == '-' ? -1 : 0;
    if (!sign || n < 3 || (unsigned)(f[1] - '0') > 9 || (unsigned)(f[2] - '0') > 9) return 0;
    int hh = (f[1] - '0') * 10 + (f[2] - '0');
    int mm = 0;
    size_t i = 3;
    int colon = i < n && f[i] == ':';
    if (colon) i++;
    if (i + 1 < n && (unsigned)(f[i] - '0') <= 9 && (unsigned)(f[i + 1] - '0') <= 9) {
        mm = (f[i] - '0') * 10 + (f[i + 1] - '0');
        i += 2;
    } else if (colon) return 0;                    /* a colon promises minutes */
    if (hh > 23 || mm > 59) return 0;
    *out = (int64_t)sign * ((int64_t)hh * 3600 + (int64_t)mm * 60) * 1000000000LL;
    return i;
}

/* One clock cell -> its lattice class + (secs, frac ns, sign).  The COLON GRID
 * and the fraction width are the class law (the writer's forms: h…h:mm minute,
 * h…h:mm:ss second, .f{1,3} time, .f{4,9} the literal grammar's bare-clock
 * timespan — the hour field is >= 2 digits and uncapped, matching the duration
 * display); validity and the split payload are q_tok_clock's.  A leading '-'
 * is the writer's duration sign.  A value past its class's payload domain is
 * not that written form.  CT_STR = not a clock cell. */
static ct_t csv_clock_class(const char* f, size_t n, int64_t* secs, int64_t* frac_ns, int* neg) {
    *neg = n && f[0] == '-';
    if (*neg) { f++; n--; }
    size_t k = 0;
    while (k < n && (unsigned)(f[k] - '0') <= 9) k++;
    if (k < 2 || k >= n || f[k] != ':') return CT_STR;
    ct_t t;
    if (n == k + 3) t = CT_MINUTE;
    else if (n == k + 6 && f[k + 3] == ':') t = CT_SECOND;
    else if (n > k + 7 && f[k + 3] == ':' && f[k + 6] == '.') t = n <= k + 10 ? CT_TIME : CT_TIMESPAN;
    else return CT_STR;
    if (!q_tok_clock(f, n, secs, frac_ns)) return CT_STR;
    if (t == CT_MINUTE && *secs / 60 > INT32_MAX) return CT_STR;
    if (t == CT_SECOND && *secs > INT32_MAX) return CT_STR;
    if (t == CT_TIME &&
        (*secs > INT32_MAX / 1000 + 1 || *secs * 1000 + *frac_ns / 1000000 > INT32_MAX)) return CT_STR;
    if (t == CT_TIMESPAN) {
        int64_t ns;
        if (__builtin_mul_overflow(*secs, 1000000000LL, &ns) ||
            __builtin_add_overflow(ns, *frac_ns, &ns)) return CT_STR;
    }
    return t;
}

/* the D-separated (or, under a frozen 'n', any Tok-accepted clock) timespan */
static int csv_timespan_cell(const char* f, size_t n, int64_t* ns) {
    int neg = n && f[0] == '-';
    if (neg) { f++; n--; }
    if (!q_tok_timespan_ns(f, n, ns)) return 0;
    if (neg) *ns = -*ns;
    return 1;
}

/* ---- the dateformat/timestampformat options (DuckDB strptime vocabulary, the
 * implemented subset): %Y %y %m %d %H %M %S %f %z %% plus literal bytes.  A format
 * REPLACES the writer's-forms grammar for its type; validation at option time
 * refuses unknown specifiers and a dateless format ('option — never silent).
 * %z is DuckDB's own semantics: apply the row's own numeric offset, store UTC.
 * Zone NAMES (%Z) need tz data we do not ship, so they stay unknown --- */

/* ok = 1; want_time lets a dateformat refuse clock specifiers */
static int csv_fmt_valid(const char* s, size_t n, int want_time) {
    int y = 0, mo = 0, d = 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i] != '%') continue;
        if (++i >= n) return 0;
        switch (s[i]) {
            case 'Y': case 'y': y = 1; break;
            case 'm': mo = 1; break;
            case 'd': d = 1; break;
            case 'H': case 'M': case 'S': case 'f': case 'z':
                if (!want_time) return 0;
                break;
            case '%': break;
            default: return 0;
        }
    }
    return y && mo && d;
}

/* one whole cell under the format -> (days, ns-of-day); 0 = not this format */
static int csv_fmt_cell(const char* fmt, const char* f, size_t n, int64_t* days, int64_t* ns) {
    const char* p = f;
    const char* end = f + n;
    int64_t v[8] = { 0 };
    enum { FY, FM, FD, FH, FMIN, FS, FF, FZ };
    for (size_t i = 0; fmt[i]; i++) {
        if (fmt[i] != '%') {
            if (p >= end || *p != fmt[i]) return 0;
            p++;
            continue;
        }
        char c = fmt[++i];
        if (c == '%') {
            if (p >= end || *p != '%') return 0;
            p++;
            continue;
        }
        if (c == 'z') {
            size_t nz = csv_tz_ns(p, (size_t)(end - p), &v[FZ]);
            if (!nz) return 0;
            p += nz;
            continue;
        }
        /* %Y is exactly 4 digits: greedy 1-4 would let "1112020" under %d%m%Y
         * end as year 20 - a silently wrong century instead of a refusal */
        int lo = c == 'Y' ? 4 : c == 'y' ? 2 : 1, hi = c == 'Y' ? 4 : c == 'f' ? 9 : 2;
        int k = 0;
        int64_t x = 0;
        while (k < hi && p < end && (unsigned)(*p - '0') <= 9) { x = x * 10 + (*p - '0'); p++; k++; }
        if (k < lo) return 0;
        switch (c) {
            case 'Y': v[FY] = x; break;
            case 'y': v[FY] = x <= 68 ? 2000 + x : 1900 + x; break;   /* the POSIX two-digit pivot */
            case 'm': v[FM] = x; break;
            case 'd': v[FD] = x; break;
            case 'H': v[FH] = x; break;
            case 'M': v[FMIN] = x; break;
            case 'S': v[FS] = x; break;
            case 'f': while (k++ < 9) x *= 10; v[FF] = x; break;
            default: return 0;
        }
    }
    if (p != end) return 0;
    if (!q_calendar_date_valid(v[FY], v[FM], v[FD])) return 0;
    if (v[FH] > 23 || v[FMIN] > 59 || v[FS] > 59) return 0;
    *days = q_calendar_days_from_civil(v[FY], v[FM], v[FD]);
    *ns = ((v[FH] * 3600 + v[FMIN] * 60 + v[FS]) * 1000000000LL) + v[FF] - v[FZ];
    /* an offset is under a day, so the shift crosses at most one midnight either way */
    if (*ns < 0) { *ns += 86400000000000LL; (*days)--; }
    else if (*ns >= 86400000000000LL) { *ns -= 86400000000000LL; (*days)++; }
    return 1;
}

/* the writer's float specials: q prints ±inf as 0w/-0w, which ray_parse_f64
 * (inf/nan only) does not read */
static int csv_f64_special(const char* f, size_t n, double* v) {
    int neg = n && f[0] == '-';
    if (neg) { f++; n--; }
    if (n != 2 || f[0] != '0' || f[1] != 'w') return 0;
    *v = neg ? -INFINITY : INFINITY;
    return 1;
}

static ct_t csv_detect(const csv_st* st, const char* f, size_t n) {
    if (csv_null_tok(f, n)) return CT_UNKNOWN;
    /* typed cells tolerate surrounding blanks (" 567" is a written long — kdb's
     * casts and DuckDB both read it); blanks-only is text, NEVER a null */
    while (n && f[0] == ' ') { f++; n--; }
    while (n && f[n - 1] == ' ') n--;
    if (n == 0) return CT_STR;
    if (st->datefmt[0] || st->tsfmt[0]) {          /* an explicit format outranks every default claim */
        int64_t dd, nn;
        if (st->datefmt[0] && csv_fmt_cell(st->datefmt, f, n, &dd, &nn)) return CT_DATE;
        if (st->tsfmt[0] && csv_fmt_cell(st->tsfmt, f, n, &dd, &nn)) return CT_TS;
    }
    if (n == 3 && ((f[0] == 'n' || f[0] == 'N') ? ((f[1] == 'a' || f[1] == 'A') && (f[2] == 'n' || f[2] == 'N'))
                                                : ((f[0] == 'i' || f[0] == 'I') && (f[1] == 'n' || f[1] == 'N') &&
                                                   (f[2] == 'f' || f[2] == 'F'))))
        return CT_F64;
    if (n == 4 && (f[0] == '+' || f[0] == '-') &&
        (f[1] == 'i' || f[1] == 'I') && (f[2] == 'n' || f[2] == 'N') && (f[3] == 'f' || f[3] == 'F'))
        return CT_F64;
    if (csv_bool_tok(f, n) >= 0) return CT_BOOL;
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
        const char* d0 = (*f == '-' || *f == '+') ? f + 1 : f;
        if (*d0 == '0' && p - d0 > 1) return CT_STR;   /* 007 is an identifier, not a number (DuckDB agrees) */
        int64_t iv;
        size_t u = ray_parse_i64(f, n, &iv);
        return u == n ? CT_I64 : CT_F64;           /* past the long domain it reads as float, like "F"$ */
    }
    double fv;
    if (csv_f64_special(f, n, &fv)) return CT_F64;
    /* the temporal shapes go through the SAME parsers the frozen phase uses:
     * whatever this function types, csv_cell_atom must accept — a near-miss must
     * sniff as text.  The numeric scan above already claimed every bare digit
     * run, so packed dates / unix-seconds stay longs (the never-guess law). */
    if (n >= 10 && (f[4] == '.' || f[4] == '-' || f[4] == '/')) {
        int32_t d;
        int64_t v;
        if (n == 10 && csv_date_cell(f, n, &d))
            return st->datefmt[0] ? CT_STR : CT_DATE;   /* an explicit dateformat OWNS the date shapes */
        if (!st->tsfmt[0] && q_tok_ts(f, n, &v)) return CT_TS;
        return CT_STR;                             /* a date-shaped head with a bad tail */
    }
    if (n >= 7 && f[n - 1] == 'm') {
        int64_t mo;
        if (csv_month_cell(f, n, &mo)) return CT_MONTH;
    }
    {
        int64_t secs, frac;
        int neg;
        ct_t ct = csv_clock_class(f, n, &secs, &frac, &neg);
        if (ct != CT_STR) return ct;
    }
    {   /* routing gate: only the D form reaches timespan from detect — bare
         * clocks classify through the lattice above, never through the 'n' cell */
        size_t i = f[0] == '-' ? 1 : 0;
        size_t d0 = i;
        while (i < n && (unsigned)(f[i] - '0') <= 9) i++;
        int64_t v;
        if (i > d0 && i < n && f[i] == 'D' && csv_timespan_cell(f, n, &v)) return CT_TIMESPAN;
    }
    return CT_STR;
}

static ct_t csv_promote(ct_t cur, ct_t obs) {
    if (cur == CT_UNKNOWN) return obs;
    if (obs == CT_UNKNOWN || cur == obs) return cur;
    if (cur == CT_STR || obs == CT_STR) return CT_STR;
    if ((cur == CT_DATE && obs == CT_TS) || (cur == CT_TS && obs == CT_DATE)) return CT_TS;
    if (cur <= CT_F64 && obs <= CT_F64) return cur > obs ? cur : obs;
    if (cur >= CT_MINUTE && cur <= CT_TIMESPAN && obs >= CT_MINUTE && obs <= CT_TIMESPAN)
        return cur > obs ? cur : obs;              /* the duration lattice: widest form wins */
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
        case CT_BOOL:     return 'b';
        case CT_I64:      return 'j';
        case CT_F64:      return 'f';
        case CT_DATE:     return 'd';
        case CT_MONTH:    return 'm';
        case CT_TS:       return 'p';
        case CT_MINUTE:   return 'u';
        case CT_SECOND:   return 'v';
        case CT_TIME:     return 't';
        case CT_TIMESPAN: return 'n';
        case CT_STR:
            if (!advise) return '*';
            return (c->non_null >= 64 && (uint32_t)c->distinct * 100u >= c->non_null * 80u) ? '*' : 's';
        default:      return advise ? 's' : '*';   /* nothing observed */
    }
}

/* ---- the frozen-type cell parser ------------------------------------------- */

/* one cell -> an OWNED atom under its FROZEN type char, or 'csv.  The clock
 * columns accept their own class and every NARROWER one (the promote lattice
 * widened to the frozen char during the sample); a WIDER cell after the sample
 * is the frozen-type-miss law — 'csv, never a truncation. */
static ray_t* csv_cell_atom(const csv_st* st, char c, const char* f, size_t n) {
    if (c == 's') return ray_sym(ray_sym_intern_runtime(f, n));
    if (c == '*') return ray_charv(f, (int64_t)n);
    if (csv_null_tok(f, n)) {
        if (c == 'b') return ray_bool(0);          /* q booleans have no null — "B"$"" is 0b */
        return ray_typed_null((int8_t)-q_type_of_char(c));
    }
    while (n && f[0] == ' ') { f++; n--; }         /* the same blank tolerance the sniffer applies */
    while (n && f[n - 1] == ' ') n--;
    if (n == 0) return q_err(QE_CSV);              /* blanks-only under a typed column is the miss law */
    switch (c) {
        case 'b': {
            int v = csv_bool_tok(f, n);
            if (v < 0 && n == 1 && (f[0] == '0' || f[0] == '1')) v = f[0] == '1';
            return v < 0 ? q_err(QE_CSV) : ray_bool(v);
        }
        case 'j': {
            int64_t v;
            size_t u = ray_parse_i64(f, n, &v);
            return (u && u == n) ? ray_i64(v) : q_err(QE_CSV);
        }
        case 'f': {
            double v;
            if (csv_f64_special(f, n, &v)) return ray_f64(v);
            size_t u = ray_parse_f64(f, n, &v);
            if (!u || u != n) return q_err(QE_CSV);
            return v != v ? ray_typed_null(-RAY_F64) : ray_f64(v);
        }
        case 'd': {
            if (st->datefmt[0]) {
                int64_t dd, nn;
                if (!csv_fmt_cell(st->datefmt, f, n, &dd, &nn)) return q_err(QE_CSV);
                return ray_date((int32_t)dd);
            }
            int32_t d;
            if (!csv_date_cell(f, n, &d)) return q_err(QE_CSV);
            return ray_date(d);
        }
        case 'm': {
            int64_t mo;
            if (!csv_month_cell(f, n, &mo)) return q_err(QE_CSV);
            return ray_month(mo);
        }
        case 'u': case 'v': case 't': {
            int64_t secs, frac;
            int neg;
            ct_t got = csv_clock_class(f, n, &secs, &frac, &neg);
            ct_t cap = c == 'u' ? CT_MINUTE : c == 'v' ? CT_SECOND : CT_TIME;
            if (got == CT_STR || got > cap) return q_err(QE_CSV);   /* a WIDER class is the miss law */
            int64_t v = c == 'u' ? secs / 60 : c == 'v' ? secs : secs * 1000 + frac / 1000000;
            if (neg) v = -v;
            /* a narrower-class value the PROMOTED char's i32 payload cannot
             * hold (a wide minute under 't') is the miss law too */
            if (v > INT32_MAX || v < -INT32_MAX) return q_err(QE_CSV);
            if (c == 'u') return ray_minute(v);
            if (c == 'v') return ray_second(v);
            return ray_time(v);                    /* RAY_TIME is ms since midnight */
        }
        case 'n': {
            int64_t ns;
            if (!csv_timespan_cell(f, n, &ns)) return q_err(QE_CSV);
            return ray_timespan(ns);
        }
        case 'p': {
            int64_t v;
            if (st->tsfmt[0] || st->datefmt[0]) {  /* a format date under 'p' reads as its midnight */
                int64_t dd, nn;
                if (st->tsfmt[0] && csv_fmt_cell(st->tsfmt, f, n, &dd, &nn))
                    return ray_timestamp(dd * 86400000000000LL + nn);
                if (st->datefmt[0] && csv_fmt_cell(st->datefmt, f, n, &dd, &nn))
                    return ray_timestamp(dd * 86400000000000LL);
                if (st->tsfmt[0]) return q_err(QE_CSV);
            }
            if (!q_tok_ts(f, n, &v)) return q_err(QE_CSV);
            return ray_timestamp(v);
        }
        default: return q_err(QE_CSV);
    }
}

/* ---- one row -> fields ----------------------------------------------------- */

/* THE ONE quote-region walk for whole-row memory (csv_field and the dialect counter; the carry scan keeps
 * its own suspension machine over the same law).  *pp at the opening quote; on success *pp lands on the
 * follower (delimiter / comment char / row end), content bounds in s/e, esc_seen when un-escaping is due.
 * 0 = the region cannot parse and *cls says which fault.  delim is an int so -1 means "none" (the
 * one-column fallback) without colliding with any byte.  esc != quote: the escape consumes its follower,
 * whatever it is; a close is any unescaped quote.  esc == quote: the RFC twin law. */
static int csv_region_scan(const csv_st* st, int delim, const char** pp, const char* end,
                           const char** s, const char** e, int* esc_seen, const char** cls) {
    const char* p = *pp + 1;
    *s = p;
    *esc_seen = 0;
    char qc = st->quote, ec = st->esc;
    while (p < end) {
        if (ec != qc && *p == ec) {
            *esc_seen = 1;
            p += p + 1 < end ? 2 : 1;              /* a trailing bare esc leaves the region open */
            continue;
        }
        if (*p != qc) { p++; continue; }
        if (ec == qc && p + 1 < end && p[1] == qc) { *esc_seen = 1; p += 2; continue; }
        break;
    }
    if (p >= end) { *cls = CSV_RJ_UNTERM; return 0; }
    *e = p;
    p++;
    if (p < end && (unsigned char)*p != delim && !(st->cmt && *p == st->cmt)) {
        *cls = CSV_RJ_UNQUOTED;
        return 0;
    }
    *pp = p;
    return 1;
}

/* One field at *pp.  Quoted fields un-escape into a malloc'd *dyn; bytes between a closing quote and the
 * delimiter, and a quote inside an unquoted field, are 'csv — st->rej_class carries which fault.  THE
 * LENIENT-QUOTE LAW (strict_mode 0b): a field whose quotes cannot parse is read as LITERAL bytes to the
 * next delimiter, opening quote included, and st->salv_class notes the salvage (the carry scan applied the
 * same law, so the row bounds already agree).  An unquoted comment char sets st->row_cut: the row ends
 * here, the tail unread.  *ate says a delimiter was consumed, so a trailing one yields the empty final
 * field. */
static ray_t* csv_field(csv_st* st, const char** pp, const char* end,
                        const char** out, size_t* outn, char** dyn, int* ate) {
    const char* p = *pp;
    int delim = st->no_delim ? -1 : (unsigned char)st->delim;
    *ate = 0;
    if (!st->quote_off && p < end && *p == st->quote) {
        const char* q0 = p;
        const char *s, *e, *cls;
        int esc_seen;
        if (!csv_region_scan(st, delim, &p, end, &s, &e, &esc_seen, &cls)) {
            if (!st->lenient) { st->rej_class = cls; return q_err(QE_CSV); }
            if (!st->salv_class) st->salv_class = cls;
            p = q0;
            goto literal;
        }
        if (esc_seen) {
            size_t raw = (size_t)(e - s);
            char* d = (char*)malloc(raw ? raw : 1);
            if (!d) return q_err(QE_WSFULL);
            *dyn = d;
            size_t o = 0;
            for (const char* q = s; q < e; q++) {
                if (*q == st->esc && q + 1 < e) q++;   /* the escape drops; its follower is literal */
                d[o++] = *q;
            }
            *out = d;
            *outn = o;
        } else {
            *out = s;
            *outn = (size_t)(e - s);
        }
    } else {
literal:;
        const char* s = p;
        while (p < end && (unsigned char)*p != delim && !(st->cmt && *p == st->cmt)) {
            if (!st->quote_off && *p == st->quote && p > s) {   /* mid-field: the carry scan counted it too */
                if (!st->lenient) { st->rej_class = CSV_RJ_UNQUOTED; return q_err(QE_CSV); }
                if (!st->salv_class) st->salv_class = CSV_RJ_UNQUOTED;
            }
            p++;
        }
        *out = s;
        *outn = (size_t)(p - s);
    }
    if (p < end) {
        if (st->cmt && *p == st->cmt) st->row_cut = 1;
        else { p++; *ate = 1; }
    }
    *pp = p;
    return NULL;
}

static void csv_fields_free(csv_st* st) {
    for (int64_t i = 0; i < st->nfields; i++) {
        free(st->fields[i].dyn);
        st->fields[i].dyn = NULL;
    }
    st->nfields = 0;
}

/* split one row into st->fields — the ONE field walk every phase (header, detect,
 * parse) consumes, so count is known before anything is committed */
static ray_t* csv_row_split(csv_st* st, const char* p, size_t n, int64_t* cnt) {
    const char* end = p + n;
    st->nfields = 0;
    st->row_cut = 0;
    for (;;) {
        if (st->nfields == st->fcap) {
            int64_t cap = st->fcap ? st->fcap * 2 : 16;
            csv_fld* nf = (csv_fld*)realloc(st->fields, (size_t)cap * sizeof(csv_fld));
            if (!nf) return q_err(QE_WSFULL);
            st->fields = nf;
            st->fcap = cap;
        }
        csv_fld* f = &st->fields[st->nfields];
        f->dyn = NULL;
        int ate = 0;
        ray_t* bad = csv_field(st, &p, end, &f->p, &f->n, &f->dyn, &ate);
        if (bad) { csv_fields_free(st); return bad; }
        st->nfields++;
        if (!ate) break;
    }
    *cnt = st->nfields;
    return NULL;
}

/* ---- the reject channel: ONE construction home ------------------------------ */

/* the audit row a bad csv row leaves behind: line / column / class / raw text.
 * col 0 = a row-level fault (the null sym). */
static ray_t* csv_reject_note(csv_st* st, int64_t line, int64_t col, const char* cls,
                              const char* p, size_t n) {
    for (int i = 0; i < 4; i++)
        if (!st->rj[i]) {
            st->rj[i] = ray_list_new(8);
            if (RAY_IS_ERR(st->rj[i])) {
                ray_t* e = st->rj[i];
                st->rj[i] = NULL;
                return e;
            }
        }
    ray_t* vs[4] = { ray_i64(line), ray_sym(col ? col : ray_sym_intern_runtime("", 0)),
                     ray_sym(ray_sym_intern_runtime(cls, strlen(cls))), ray_charv(p, (int64_t)n) };
    ray_t* bad = NULL;
    for (int i = 0; i < 4; i++) {
        if (!vs[i] || RAY_IS_ERR(vs[i])) { if (!bad) bad = vs[i] ? vs[i] : q_err(QE_OOM); continue; }
        if (!bad) {
            st->rj[i] = ray_list_append(st->rj[i], vs[i]);
            if (RAY_IS_ERR(st->rj[i])) {
                bad = st->rj[i];
                st->rj[i] = NULL;
            }
        }
        ray_release(vs[i]);
    }
    if (bad) return bad;
    st->rej_total++;
    return NULL;
}

/* the reject records from index `from` on, as an OWNED table (empty = the schema) */
static ray_t* csv_rejects_tbl(csv_st* st, int64_t from) {
    static const char* const cn[4] = { "line", "column", "error", "csvLine" };
    static const char ct[4] = { 'j', 's', 's', '*' };
    ray_t* tbl = ray_table_new(4);
    if (RAY_IS_ERR(tbl)) return tbl;
    for (int c = 0; c < 4; c++) {
        ray_t* col;
        if (!st->rj[c] || ray_len(st->rj[c]) <= from) col = csv_typed_empty(ct[c]);
        else {
            int64_t n = ray_len(st->rj[c]);
            ray_t** it = (ray_t**)ray_data(st->rj[c]);
            ray_t* l = ray_list_new(n - from);
            for (int64_t i = from; i < n && !RAY_IS_ERR(l); i++) l = ray_list_append(l, it[i]);
            col = RAY_IS_ERR(l) ? l : q_list_collapse(l);
            if (!RAY_IS_ERR(l)) ray_release(l);
        }
        if (!col || RAY_IS_ERR(col)) {
            ray_release(tbl);
            return col ? col : q_err(QE_OOM);
        }
        tbl = ray_table_add_col(tbl, ray_sym_intern_runtime(cn[c], strlen(cn[c])), col);
        ray_release(col);
        if (RAY_IS_ERR(tbl)) return tbl;
    }
    return tbl;
}

/* ---- the frozen-phase row parser ------------------------------------------- */

/* One row, all-or-nothing: cells stage into rowvals and commit together, so a
 * rejected row leaves no partial column behind.  A fault whose rej_class is set
 * (structural or cast) becomes a reject record under a continue-mode option
 * (ignore_errors / store_rejects); anything else propagates.  A padded or
 * quote-salvaged row commits AND leaves its audit record. */
static ray_t* csv_parse_row(csv_st* st, const char* p, size_t n) {
    int64_t cnt = 0, badcol = 0;
    st->rej_class = NULL;
    st->salv_class = NULL;
    ray_t* bad = csv_row_split(st, p, n, &cnt);
    if (!bad && cnt != st->ncols && !(cnt < st->ncols && st->null_pad)) {
        st->rej_class = cnt > st->ncols ? CSV_RJ_TOOMANY : CSV_RJ_TOOFEW;
        bad = q_err(QE_CSV);
    }
    if (!bad) {
        for (int64_t j = 0; j < st->ncols && !bad; j++) {
            int64_t k = st->kidx[j];
            if (k < 0) continue;
            ray_t* a = j < cnt ? csv_cell_atom(st, st->kchars[k], st->fields[j].p, st->fields[j].n)
                               : csv_cell_atom(st, st->kchars[k], "", 0);   /* the pad: the empty-field null */
            if (!a) bad = q_err(QE_OOM);
            else if (RAY_IS_ERR(a)) {
                st->rej_class = CSV_RJ_CAST;
                badcol = st->knames[k];
                bad = a;
            } else st->rowvals[k] = a;
        }
    }
    if (!bad && (cnt < st->ncols || st->salv_class))
        bad = csv_reject_note(st, st->line_cur, 0, cnt < st->ncols ? CSV_RJ_PADDED : st->salv_class, p, n);
    if (!bad) {
        for (int64_t k = 0; k < st->nkept && !bad; k++) {
            st->acc[k] = ray_list_append(st->acc[k], st->rowvals[k]);
            ray_release(st->rowvals[k]);
            st->rowvals[k] = NULL;
            if (RAY_IS_ERR(st->acc[k])) {
                bad = st->acc[k];
                st->acc[k] = NULL;
            }
        }
        if (!bad) {
            st->rows_total++;
            st->pend++;
        }
    }
    csv_fields_free(st);
    for (int64_t k = 0; k < st->nkept; k++)
        if (st->rowvals[k]) {
            ray_release(st->rowvals[k]);
            st->rowvals[k] = NULL;
        }
    if (bad && st->rej_class && (st->ignore_err || st->store_rej)) {
        ray_release(bad);
        return csv_reject_note(st, st->line_cur, badcol, st->rej_class, p, n);
    }
    return bad;
}

/* ---- freeze: header + types from the sniff sample -------------------------- */

/* one sniff row into the type/cardinality sample.  Under a tolerance lever a row
 * the parse phase will pad keeps its cells' evidence, and a row it will REJECT is
 * skipped whole — a skipped row's cells must not poison the frozen types. */
static ray_t* csv_row_detect(csv_st* st, const char* p, size_t n) {
    int64_t cnt = 0;
    st->rej_class = NULL;
    st->salv_class = NULL;
    ray_t* bad = csv_row_split(st, p, n, &cnt);
    int tol = st->ignore_err || st->store_rej;
    if (bad) {
        if (!(st->rej_class && tol)) return bad;
        ray_release(bad);
        return NULL;
    }
    if (cnt != st->ncols && !(cnt < st->ncols && st->null_pad)) {
        csv_fields_free(st);
        return tol ? NULL : q_err(QE_CSV);
    }
    for (int64_t j = 0; j < (cnt < st->ncols ? cnt : st->ncols); j++) {
        st->f_ct[j] = csv_promote(st->f_ct[j], csv_detect(st, st->fields[j].p, st->fields[j].n));
        csv_card_note(&st->f_card[j], st->fields[j].p, st->fields[j].n);
    }
    csv_fields_free(st);
    return NULL;
}

/* 'b'jfsdtpmuvn / '*' / ' ', upper case (the `0:` spelling) folded down; 0 = unknown */
static char csv_type_canon(char c) {
    if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    return strchr("bjfsdtpmuvn* ", c) && c ? c : 0;
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

/* field count of one row under a candidate delimiter; -1 = malformed under it
 * (csv_field's grammar via the shared region walk; an unquoted comment char ends the count) */
static int64_t csv_dialect_cols(const csv_st* st, const char* p, size_t n, int delim) {
    const char* end = p + n;
    int64_t col = 0;
    for (;;) {
        col++;
        if (!st->quote_off && p < end && *p == st->quote) {
            const char *s, *e, *cls;
            int es;
            if (!csv_region_scan(st, delim, &p, end, &s, &e, &es, &cls)) return -1;
        } else
            while (p < end && (unsigned char)*p != delim) {
                if (st->cmt && *p == st->cmt) return col;
                if (!st->quote_off && *p == st->quote) return -1;
                p++;
            }
        if (p >= end || (st->cmt && *p == st->cmt)) return col;
        p++;
    }
}

/* THE DELIMITER-SNIFF LAW (structure-only, DuckDB-style consistency): a
 * candidate from , ; \t | QUALIFIES only if every one of the first
 * CSV_DIALECT_ROWS sample rows parses to the SAME field count, above one
 * column, none malformed; the qualifying candidate with the most columns wins,
 * a tie keeping candidate order (comma first); none qualifying either falls
 * back to one column (below) or keeps comma.
 * Data statistics play no part, and an explicit delim option means no sniff.
 * Under a ragged-row lever (ignore_errors / null_padding / store_rejects) the
 * uniformity clause relaxes to the MODAL count — the lever's whole point is
 * rows off the common width, which must not defeat the delimiter (oracle-
 * verified: DuckDB picks ';' on "a;b/1;2/3" under either lever); malformed
 * rows still disqualify.
 * THE NARROWED FALLBACK (csv PR-4): when no candidate qualifies, the file loads
 * as ONE column — but only when it is one-column-SHAPED: for every candidate a
 * strict majority of sample rows have field count 1 and none is malformed.  A
 * merely POLLUTED candidate (modal width above one — a comma file with a stray
 * junk line) keeps the strict refusal, and bad quotes stay strict_mode's
 * business, so the strict law's loudness survives the fallback. */
#define CSV_DIALECT_ROWS 128
static void csv_delim_sniff(csv_st* st) {
    static const char cand[] = { ',', ';', '\t', '|' };
    int64_t m = ray_len(st->sniff);
    if (m > CSV_DIALECT_ROWS) m = CSV_DIALECT_ROWS;
    ray_t** rows = (ray_t**)ray_data(st->sniff);
    int tol = st->ignore_err || st->store_rej || st->null_pad;
    int64_t best_cols = 0;
    int onecol = 1;
    for (int64_t i = 0; i < m && onecol; i++)      /* NUL-laden rows are binary, not one-column text */
        if (memchr(ray_data(rows[i]), 0, (size_t)ray_len(rows[i]))) onecol = 0;
    for (size_t c = 0; c < sizeof cand; c++) {
        if ((st->cmt && cand[c] == st->cmt) || (!st->quote_off && cand[c] == st->quote)) continue;
        int64_t k[CSV_DIALECT_ROWS], ones = 0, cols = 0;
        int malformed = 0, ragged = 0;
        for (int64_t i = 0; i < m; i++) {
            k[i] = csv_dialect_cols(st, (const char*)ray_data(rows[i]), (size_t)ray_len(rows[i]),
                                    (unsigned char)cand[c]);
            malformed |= k[i] < 0;
            ones += k[i] == 1;
            ragged |= i && k[i] != k[0];
        }
        if (malformed || ones * 2 <= m) onecol = 0;
        if (malformed) continue;
        if (!tol) {
            if (ragged || k[0] < 2) continue;
            cols = k[0];
        } else
            for (int64_t i = 0; i < m; i++) {      /* modal count; a frequency tie takes the wider */
                int64_t f = 0;
                for (int64_t j = 0; j < m; j++) f += k[j] == k[i];
                int64_t bf = 0;
                for (int64_t j = 0; j < m; j++) bf += k[j] == cols;
                if (f > bf || (f == bf && k[i] > cols)) cols = k[i];
            }
        if (cols < 2) continue;                    /* a one-column mode is no delimiter evidence */
        if (cols > best_cols) {
            best_cols = cols;
            st->delim = cand[c];
        }
    }
    /* the fallback is a DEFAULTS-side law: a ragged-row lever declares the data delimited, and duck
     * agrees (a,b/1/2/3 under ignore_errors keeps comma and skips the short rows — oracle-verified) */
    if (!tol && !best_cols && onecol) st->no_delim = 1;
}

static ray_t* csv_freeze(csv_st* st) {
    int64_t m = st->sniff ? ray_len(st->sniff) : 0;
    if (m == 0) return q_err(QE_CSV);
    ray_t** rows = (ray_t**)ray_data(st->sniff);
    if (!st->delim_explicit) csv_delim_sniff(st);
    st->f_hdr = ray_list_new(8);
    if (RAY_IS_ERR(st->f_hdr)) {
        ray_t* e = st->f_hdr;
        st->f_hdr = NULL;
        return e;
    }
    int64_t nc = 0;
    st->rej_class = NULL;
    st->salv_class = NULL;
    ray_t* bad = csv_row_split(st, (const char*)ray_data(rows[0]), (size_t)ray_len(rows[0]), &nc);
    if (bad) return bad;                           /* row 0 must parse: no schema, no tolerance */
    const char* hdr_salv = st->salv_class;         /* a salvaged HEADER is audited too (row 0 as
                                                    * data records itself through the replay) */
    for (int64_t j = 0; j < nc; j++) {
        ray_t* c = ray_charv(st->fields[j].p, (int64_t)st->fields[j].n);
        if (!RAY_IS_ERR(c)) {
            st->f_hdr = ray_list_append(st->f_hdr, c);
            ray_release(c);
        }
        if (RAY_IS_ERR(c) || RAY_IS_ERR(st->f_hdr)) {
            ray_t* e = RAY_IS_ERR(c) ? c : st->f_hdr;
            if (!RAY_IS_ERR(c)) st->f_hdr = NULL;
            csv_fields_free(st);
            return e;
        }
    }
    csv_fields_free(st);
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
        bad = csv_row_detect(st, (const char*)ray_data(rows[i]), (size_t)ray_len(rows[i]));
        if (bad) return bad;
    }
    int header = st->header;
    if (header < 0) {
        /* all row-0 cells textual = header — an EMPTY cell counts as textual (a
         * header may carry blank names; DuckDB agrees); so is ONE textual cell
         * over a column the data sniffed numeric/temporal (a year-pivot header
         * like "id,2024,2025") or left ALL-EMPTY (a name over nothing but nulls,
         * DuckDB's rule) — demotions the data alone would never produce */
        int all_str = 1, demote = 0;
        for (int64_t j = 0; j < nc; j++) {
            ct_t d0 = csv_detect(st, (const char*)ray_data(hf[j]), (size_t)ray_len(hf[j]));
            if (d0 != CT_STR && d0 != CT_UNKNOWN) all_str = 0;
            else if (d0 == CT_STR && st->f_ct[j] != CT_STR && (st->f_ct[j] != CT_UNKNOWN || m > 1))
                demote = 1;
        }
        header = all_str || demote;
    }
    if (!header) {                                 /* row 0 is data after all: fold it into the sniff */
        bad = csv_row_detect(st, (const char*)ray_data(rows[0]), (size_t)ray_len(rows[0]));
        if (bad) return bad;
    } else if (hdr_salv) {
        bad = csv_reject_note(st, st->sniff_lines[0], 0, hdr_salv,
                              (const char*)ray_data(rows[0]), (size_t)ray_len(rows[0]));
        if (bad) return bad;
    }
    for (int64_t j = 0; j < nc; j++) {
        const char* hp = (const char*)ray_data(hf[j]);
        int64_t hl = header ? ray_len(hf[j]) : 0;
        while (hl && (hp[0] == ' ' || hp[0] == '\t')) { hp++; hl--; }   /* names trim, like DuckDB */
        while (hl && (hp[hl - 1] == ' ' || hp[hl - 1] == '\t')) hl--;
        if (hl > 0) st->names[j] = ray_sym_intern_runtime(hp, (size_t)hl);
        else {                                     /* headerless (or a blank header cell): x, x1, x2… */
            char b[24];
            int bl = j ? snprintf(b, sizeof b, "x%lld", (long long)j) : (b[0] = 'x', 1);
            st->names[j] = ray_sym_intern_runtime(b, (size_t)bl);
        }
    }
    for (int64_t j = 0; j < nc; j++) {             /* duplicate header names dedupe DuckDB-style:
                                                    * the later twin takes _1, _2… (first free) */
        int64_t dup = 0;
        for (int64_t i = 0; i < j; i++) dup |= st->names[i] == st->names[j];
        if (!dup) continue;
        ray_t* s = ray_sym_str(st->names[j]);      /* borrowed */
        size_t bl = ray_str_len(s);
        char* b = (char*)malloc(bl + 24);
        if (!b) return q_err(QE_WSFULL);
        memcpy(b, ray_str_ptr(s), bl);
        for (int suf = 1;; suf++) {
            int sl = snprintf(b + bl, 24, "_%d", suf);
            int64_t cand = ray_sym_intern_runtime(b, bl + (size_t)sl);
            int64_t i = 0;
            while (i < j && st->names[i] != cand) i++;
            if (i == j) { st->names[j] = cand; break; }
        }
        free(b);
    }
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
    st->rowvals = (ray_t**)calloc((size_t)st->nkept, sizeof(ray_t*));
    if (!st->acc || !st->rowvals) return q_err(QE_WSFULL);
    for (int64_t k = 0; k < st->nkept; k++) {
        st->acc[k] = ray_list_new(64);
        if (RAY_IS_ERR(st->acc[k])) {
            ray_t* e = st->acc[k];
            st->acc[k] = NULL;
            return e;
        }
    }
    st->frozen = 1;
    if (!st->info_only) {
        int64_t live_line = st->line_cur;          /* the replay borrows the counter */
        for (int64_t i = first; i < m; i++) {
            st->line_cur = st->sniff_lines[i];
            bad = csv_parse_row(st, (const char*)ray_data(rows[i]), (size_t)ray_len(rows[i]));
            if (bad) return bad;
        }
        st->line_cur = live_line;
    }
    ray_release(st->sniff);
    st->sniff = NULL;
    ray_release(st->f_hdr);
    st->f_hdr = NULL;
    free(st->f_ct);
    st->f_ct = NULL;
    free(st->f_card);
    st->f_card = NULL;
    free(st->sniff_lines);
    st->sniff_lines = NULL;
    return NULL;
}

/* ---- the carry: bytes in, complete rows out -------------------------------- */

static ray_t* csv_line(csv_st* st, const char* p, size_t n) {
    if (st->cmt && n && p[0] == st->cmt) return NULL;   /* a FIRST-byte comment line: requested dialect,
                                                         * skipped whole — never counted, never rejected */
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
    int64_t m = ray_len(st->sniff);
    int64_t* nl = (int64_t*)realloc(st->sniff_lines, (size_t)m * sizeof(int64_t));
    if (!nl) return q_err(QE_WSFULL);
    st->sniff_lines = nl;
    nl[m - 1] = st->line_cur;                      /* the replay rejects with real line numbers */
    return m >= st->sample ? csv_freeze(st) : NULL;
}

/* The scan's delimiter predicate: the exact delimiter once it is KNOWN (frozen
 * or explicit); before the sniff resolves it, any candidate counts — a quoted
 * field closing onto ';' or '|' must span its newlines even though the dialect
 * is still comma-default (new_line_string.csv: the close's follower is only
 * meaningful relative to the delimiter, which does not exist yet). */
static int csv_scan_delim(const csv_st* st, char c) {
    if (st->frozen || st->delim_explicit) return !st->no_delim && c == st->delim;
    return c == ',' || c == ';' || c == '\t' || c == '|';
}

/* physical line breaks inside one row's text (a quoted field's newlines) */
static int64_t csv_text_lines(const char* p, size_t n) {
    int64_t k = 0;
    for (size_t i = 0; i < n; i++)
        if (p[i] == '\n' || (p[i] == '\r' && (i + 1 == n || p[i + 1] != '\n'))) k++;
    return k;
}

/* THE ONE carry scan (strict and lenient row bounds are the same law): a
 * field-start quote opens a region that must close onto a delimiter/terminator/
 * EOF; anything else means the quotes cannot parse, so the row bounds read the
 * quote as a literal byte — exactly csv_field's grammar, which then signals
 * (strict) or salvages (lenient) per PHYSICAL row.  A region (or a bare CR) the
 * carry cannot yet resolve suspends the scan until more bytes arrive — the
 * probe resume point keeps that O(n) — and at_eof resolves everything. */
static ray_t* csv_drain(csv_st* st, int at_eof) {
    size_t rs = 0, i = st->scan;
    int fs = st->scan_fs, ct = st->scan_ct;
    ray_t* bad = NULL;
    while (i < st->clen && !bad) {
        char c = st->carry[i];
        if (c == '\n' || c == '\r') {
            if (c == '\r' && i + 1 == st->clen && !at_eof) break;  /* CRLF may straddle the feed */
            st->line_cur = st->row_line;
            bad = csv_line(st, st->carry + rs, i - rs);
            st->row_line += 1 + csv_text_lines(st->carry + rs, i - rs);
            if (c == '\r' && i + 1 < st->clen && st->carry[i + 1] == '\n') i++;
            rs = ++i;
            fs = 1;
            ct = 0;
            continue;
        }
        if (ct) { i++; continue; }                 /* a comment tail binds nothing until its newline */
        if (st->cmt && c == st->cmt) { ct = 1; i++; continue; }
        if (!st->quote_off && c == st->quote && fs) {
            size_t q0 = i;
            size_t p = q0 + 1 > st->scan_probe ? q0 + 1 : st->scan_probe;
            int resolved = 0, ok = 0;
            while (p < st->clen) {
                char b = st->carry[p];
                if (st->esc != st->quote && b == st->esc) {
                    if (p + 1 == st->clen && !at_eof) break;       /* the escape's follower unknown */
                    p += p + 1 < st->clen ? 2 : 1; /* a trailing bare esc at EOF leaves the region open */
                    continue;
                }
                if (b != st->quote) { p++; continue; }
                if (st->esc == st->quote && p + 1 < st->clen && st->carry[p + 1] == st->quote) {
                    p += 2;
                    continue;
                }
                if (p + 1 == st->clen && !at_eof) break;           /* twin or follower unknown */
                resolved = 1;
                /* lenient refuses EOF as a close's follower (oracle: DuckDB bounds
                 * evil_nullpadding at its newlines); strict keeps RFC's EOF close */
                ok = p + 1 == st->clen
                         ? !st->lenient
                         : csv_scan_delim(st, st->carry[p + 1]) ||
                               st->carry[p + 1] == '\n' || st->carry[p + 1] == '\r' ||
                               (st->cmt && st->carry[p + 1] == st->cmt);
                break;
            }
            if (!resolved && !at_eof) {                            /* suspend at the quote */
                st->scan_probe = p;
                i = q0;
                fs = 1;
                break;
            }
            st->scan_probe = 0;
            i = resolved && ok ? p + 1 : q0 + 1;                   /* one field, or literal bytes */
            fs = 0;
            continue;
        }
        if (csv_scan_delim(st, c)) { fs = 1; i++; continue; }
        fs = 0;
        i++;
    }
    st->scan_fs = fs;
    st->scan_ct = ct;
    if (rs) memmove(st->carry, st->carry + rs, st->clen - rs);
    st->clen -= rs;
    st->scan = i - rs;
    st->scan_probe = st->scan_probe > rs ? st->scan_probe - rs : 0;
    return bad;
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
    return csv_drain(st, 0);
}

static ray_t* csv_finish(csv_st* st) {
    ray_t* bad = csv_drain(st, 1);
    if (bad) return bad;
    if (st->clen) {                                /* the last row needs no terminator */
        st->line_cur = st->row_line;
        bad = csv_line(st, st->carry, st->clen);
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

/* misc: the lambda target's extensible side-channel — 0-based chunk index + rows so far */
static ray_t* csv_misc_dict(int64_t chunk, int64_t rows) {
    static const char* const kn[2] = { "chunk", "rows" };
    ray_t* kl = ray_list_new(2);
    ray_t* vl = ray_list_new(2);
    int64_t v[2] = { chunk, rows };
    for (int i = 0; i < 2 && !RAY_IS_ERR(kl) && !RAY_IS_ERR(vl); i++) {
        ray_t* k = ray_sym(ray_sym_intern_runtime(kn[i], strlen(kn[i])));
        ray_t* x = ray_i64(v[i]);
        kl = ray_list_append(kl, k);
        vl = ray_list_append(vl, x);
        ray_release(k);
        ray_release(x);
    }
    if (RAY_IS_ERR(kl) || RAY_IS_ERR(vl)) {
        ray_t* e = RAY_IS_ERR(kl) ? kl : vl;
        if (!RAY_IS_ERR(kl)) ray_release(kl);
        if (!RAY_IS_ERR(vl)) ray_release(vl);
        return e;
    }
    ray_t* keys = q_list_collapse(kl);
    ray_release(kl);
    if (!keys || RAY_IS_ERR(keys)) { ray_release(vl); return keys ? keys : q_err(QE_OOM); }
    ray_t* vals = q_list_collapse(vl);
    ray_release(vl);
    if (!vals || RAY_IS_ERR(vals)) { ray_release(keys); return vals ? vals : q_err(QE_OOM); }
    return ray_dict_new(keys, vals);               /* consumes both */
}

static ray_t* csv_emit(csv_st* st) {
    ray_t* tbl = csv_flush_tbl(st);
    if (RAY_IS_ERR(tbl)) return tbl;
    ray_t* r;
    if (st->sink_kind == 1) r = st->use_upsert ? q_upsert_wrap(st->sink, tbl) : q_insert_wrap(st->sink, tbl);
    else {
        ray_t* ed = csv_rejects_tbl(st, st->rej_flushed);   /* THIS batch's records */
        if (!ed || RAY_IS_ERR(ed)) { ray_release(tbl); return ed ? ed : q_err(QE_OOM); }
        st->rej_flushed = st->rej_total;
        ray_t* misc = csv_misc_dict(st->batches - 1, st->rows_total);
        if (!misc || RAY_IS_ERR(misc)) {
            ray_release(tbl);
            ray_release(ed);
            return misc ? misc : q_err(QE_OOM);
        }
        ray_t* cargs[3] = { tbl, ed, misc };
        r = q_eval_apply_value(st->sink, cargs, 3);
        ray_release(ed);
        ray_release(misc);
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

/* the dialect-char option shape (quote/escape/comment): a char atom or a 0/1-char string.
 * *out: the byte, or -2 for the empty string.  1 ok, 0 'type, -1 'domain. */
static int csv_char_opt(ray_t* v, int* out) {
    if (v->type == -RAY_CHARV) *out = (unsigned char)v->u8;
    else if (v->type == RAY_CHARV) {
        int64_t l = ray_len(v);
        if (l > 1) return -1;
        *out = l ? (unsigned char)*(const char*)ray_data(v) : -2;
    } else return 0;
    return (*out == '\n' || *out == '\r') ? -1 : 1;
}

static ray_t* csv_opts(csv_st* st, ray_t* opts) {
    if (!opts || opts->type == RAY_NULL) return NULL;
    if (opts->type != RAY_DICT) return q_err(QE_TYPE);
    ray_t* ks = ray_dict_keys(opts);
    ray_t* vs = ray_dict_vals(opts);
    int64_t n = ks ? ray_len(ks) : 0;
    int q_ch = -1, e_ch = -1, c_ch = -1;           /* dialect chars collect first, commit after the loop
                                                    * — dict key order must not decide the cross-checks */
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
            else if (v->u8 == '\n' || v->u8 == '\r')
                bad = q_err(QE_DOMAIN);            /* the terminators cannot delimit; the quote/comment
                                                    * collisions are the cross-checks below */
            else { st->delim = (char)v->u8; st->delim_explicit = 1; }
        } else if (csv_sym_is(k->i64, "quote") || csv_sym_is(k->i64, "escape") ||
                   csv_sym_is(k->i64, "comment")) {
            int ch, r = csv_char_opt(v, &ch);
            if (r <= 0) bad = q_err(r ? QE_DOMAIN : QE_TYPE);
            else if (csv_sym_is(k->i64, "quote")) q_ch = ch;
            else if (csv_sym_is(k->i64, "escape")) e_ch = ch;
            else c_ch = ch;
        } else if (csv_sym_is(k->i64, "header")) {
            if (v->type == -RAY_BOOL) st->header = v->u8 != 0;
            else bad = q_err(QE_TYPE);
        } else if (csv_sym_is(k->i64, "dateformat") || csv_sym_is(k->i64, "timestampformat")) {
            int is_ts = csv_sym_is(k->i64, "timestampformat");
            if (v->type != RAY_CHARV && v->type != -RAY_CHARV) bad = q_err(QE_TYPE);
            else {
                int64_t fl = v->type == RAY_CHARV ? ray_len(v) : 1;
                const char* fp = v->type == RAY_CHARV ? (const char*)ray_data(v) : (const char*)&v->u8;
                if (fl <= 0 || fl >= CSV_FMT_MAX || !csv_fmt_valid(fp, (size_t)fl, is_ts))
                    bad = q_err(QE_OPTION);        /* outside the implemented strptime subset */
                else {
                    char* dst = is_ts ? st->tsfmt : st->datefmt;
                    memcpy(dst, fp, (size_t)fl);
                    dst[fl] = 0;
                }
            }
        } else if (csv_sym_is(k->i64, "sample_size") || csv_sym_is(k->i64, "buffer_size")) {
            int64_t x;
            if (!q_type_strict_i64(v, &x)) bad = q_err(QE_TYPE);
            else if (x <= 0) bad = q_err(QE_DOMAIN);
            else if (csv_sym_is(k->i64, "sample_size")) st->sample = x;
            else st->bufsz = x;
        } else if (csv_sym_is(k->i64, "ignore_errors") || csv_sym_is(k->i64, "null_padding") ||
                   csv_sym_is(k->i64, "strict_mode") || csv_sym_is(k->i64, "store_rejects")) {
            if (v->type != -RAY_BOOL) bad = q_err(QE_TYPE);
            else if (csv_sym_is(k->i64, "ignore_errors")) st->ignore_err = v->u8 != 0;
            else if (csv_sym_is(k->i64, "null_padding")) st->null_pad = v->u8 != 0;
            else if (csv_sym_is(k->i64, "strict_mode")) st->lenient = v->u8 == 0;
            else st->store_rej = v->u8 != 0;
        } else if (csv_sym_is(k->i64, "rejects_table")) {
            if (v->type != -RAY_SYM) bad = q_err(QE_TYPE);
            else {
                st->rej_name = v->i64;
                st->store_rej = 1;                 /* naming the table opts in, like DuckDB */
            }
        } else {
            /* skip/nullstr/all_varchar are ratified-but-later; they and
             * unknown keys signal alike — an option is NEVER silently ignored */
            bad = q_err(QE_OPTION);
        }
        if (k && !RAY_IS_ERR(k)) ray_release(k);
        if (v && !RAY_IS_ERR(v)) ray_release(v);
        if (bad) return bad;
    }
    if (q_ch == -2) st->quote_off = 1;
    else if (q_ch >= 0) st->quote = (char)q_ch;
    if (e_ch >= 0) st->esc = (char)e_ch;           /* escape="" reads as the default: the quote itself */
    else st->esc = st->quote;
    if (c_ch >= 0) st->cmt = (char)c_ch;           /* comment="" stays disabled — duck's own default */
    /* one byte cannot serve two grammar roles.  delim collisions only bind an EXPLICIT delim —
     * a sniffed delimiter simply excludes the quote/comment bytes from its candidates */
    if (st->delim_explicit && ((!st->quote_off && st->delim == st->quote) ||
                               (st->cmt && st->cmt == st->delim)))
        return q_err(QE_DOMAIN);
    if (st->cmt && !st->quote_off && st->cmt == st->quote) return q_err(QE_DOMAIN);
    return NULL;
}

static void csv_free(csv_st* st) {
    csv_fields_free(st);
    free(st->fields);
    free(st->carry);
    free(st->names);
    free(st->ctypes);
    free(st->kidx);
    free(st->knames);
    free(st->kchars);
    free(st->f_ct);
    free(st->f_card);
    free(st->sniff_lines);
    free(st->tgt_names);
    free(st->tgt_chars);
    free(st->ign);
    if (st->sniff) ray_release(st->sniff);
    if (st->f_hdr) ray_release(st->f_hdr);
    for (int i = 0; i < 4; i++)
        if (st->rj[i]) ray_release(st->rj[i]);
    if (st->rowvals) {
        for (int64_t k = 0; k < st->nkept; k++)
            if (st->rowvals[k]) ray_release(st->rowvals[k]);
        free(st->rowvals);
    }
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
        /* the first read is at least the BOM's 3 bytes, so it can never straddle a tiny buffer */
        int64_t want = (off == 0 && st->bufsz < 3) ? 3 : st->bufsz;
        ray_t* b = q_io_read_slice(path, off, want, NULL);
        if (!b) { bad = q_err(QE_OOM); break; }
        if (RAY_IS_ERR(b)) { bad = b; break; }
        int64_t got = ray_len(b);
        const char* p = (const char*)ray_data(b);
        int64_t skip = 0;
        if (off == 0 && got >= 3 && !memcmp(p, "\xef\xbb\xbf", 3)) skip = 3;   /* a leading UTF-8 BOM */
        if (got > skip) bad = csv_feed(st, p + skip, got - skip);
        ray_release(b);
        if (bad) break;
        if (st->frozen && st->sink_kind && st->pend > 0) {
            bad = csv_emit(st);
            if (bad) break;
        }
        off += got;
        if (got < want || (st->info_only && st->frozen)) break;
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
    ray_t* vs[5] = { ray_i64(st->rows_total), ray_i64(st->rej_total), ray_i64(st->batches), iv, td };
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
    st->quote = st->esc = '"';
    st->header = -1;
    st->sample = CSV_DEF_SAMPLE;
    st->bufsz = CSV_DEF_BUFFER;
    st->scan_fs = 1;                               /* byte 0 of the file IS a field start (codex P1) */
    st->row_line = 1;
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
            /* flush the tail — a zero-row file still delivers its schema once, and a
             * lambda still receives a trailing all-rejected batch's errData */
            if (st.pend > 0 || st.batches == 0 ||
                (st.sink_kind == 2 && st.rej_total > st.rej_flushed)) bad = csv_emit(&st);
            if (!bad) result = csv_summary(&st);
        }
    }
    if (!bad && result && !RAY_IS_ERR(result) && st.store_rej) {
        /* one load, one audit: the stored table is REPLACED, empty on a clean load;
         * the name lands like any q assignment (q_env_set re-roots through `\d`) */
        ray_t* rt = csv_rejects_tbl(&st, 0);
        if (!rt || RAY_IS_ERR(rt)) bad = rt ? rt : q_err(QE_OOM);
        else {
            int64_t nm = st.rej_name ? st.rej_name : ray_sym_intern_runtime("reject_errors", 13);
            ray_err_t e = q_env_set(nm, rt);
            ray_release(rt);
            if (e) bad = q_env_err(e);
        }
        if (bad) {
            ray_release(result);
            result = NULL;
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
