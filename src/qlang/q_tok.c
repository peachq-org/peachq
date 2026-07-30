/* q_tok.c — THE single string->value scanner home (contract: q_tok.h).
 * Section 1: the literal-magnitude scanner the code parser calls.
 * Section 2: the `$` Tok whole-string scanners.  Both sit on q_calendar.c. */
#include "qlang/q_tok.h"
#include "qlang/q_err.h"
#include "qlang/q_calendar.h"  /* q_calendar_days_from_civil, q_calendar_date_valid, q_calendar_ts_compose(_checked) */
#include "core/numparse.h"     /* ray_parse_f64/i64 — float twin + numeric Tok */
#include "lang/internal.h"      /* ray_typed_null, ray_guid, ray_error — q_tok() values */
#include "qlang/q_parse_internal.h"  /* MAX_VEC — one literal cap for both scanners */
#include <limits.h>
#include <math.h>
#include <string.h>

static int tok_digit(char c) { return c >= '0' && c <= '9'; }

static int tok_dig_run(const char *s, int p) {
    int n = 0;
    while (tok_digit(s[p + n])) n++;
    return n;
}

/* ===== 1. literal magnitudes (the code parser's temporal arm) ===== */
int q_tok_temporal(const char* src, int* p, q_tok_el* out, const char** err) {
    /* Date literal magnitude: strictly yyyy.mm.dd (4-2-2 digits — every
     * published-doc spelling is zero-padded), next byte neither digit nor
     * another dot.  Checked BEFORE the float peek, which would otherwise eat
     * `2000.01` and strand `.01` (the pre-date 'type failure).  Exactly ONE
     * dot stays a float: kdb's bare `2000.01` IS the float 2000.01 (a month
     * literal needs the `m` suffix, and month has no engine type).  A leading
     * sign the SCANNER already classified as glued (neg_sign / vector
     * elements) negates the day count: kdb `-2012.01.01` is 1988.01.01.
     * Invalid civil dates (2000.13.01, 2000.02.30, 0000.01.01) die rather
     * than fall back to the float-strand mess. */
    {
        int q = *p;
        int neg = (src[q] == '-');
        if (neg) q++;
        if (tok_dig_run(src, q) == 4 && src[q + 4] == '.' &&
            tok_dig_run(src, q + 5) == 2 && src[q + 7] == '.' &&
            tok_dig_run(src, q + 8) == 2 &&
            !(tok_digit(src[q + 10])) && src[q + 10] != '.') {
            int64_t y = (src[q]     - '0') * 1000 + (src[q + 1] - '0') * 100
                      + (src[q + 2] - '0') * 10   + (src[q + 3] - '0');
            int64_t mo = (src[q + 5] - '0') * 10 + (src[q + 6] - '0');
            int64_t d  = (src[q + 8] - '0') * 10 + (src[q + 9] - '0');
            if (!q_calendar_date_valid(y, mo, d)) { *err = "bad date"; return -1; }
            if (src[q + 10] == 'D') {
                /* Timestamp literal: dateDtimespan (datatypes.md row 12).
                 * Full clock HH:MM:SS required (cast.md pins both the
                 * fraction-less 2015.10.28D03:55:58 and the 9-digit
                 * 2014.11.22D17:43:40.123456789); a fraction of 1..9 digits
                 * right-pads to nanoseconds.  The part after D is a TIMESPAN
                 * (no 24h cap — hours normalize through the ns count), so
                 * only mm/ss >= 60 die, mirroring the time-literal arm.
                 * Shorter tod forms (bare D / D12 / D12:00) are deferred; an
                 * invalid tod after D dies rather than half-matching a date
                 * and stranding the tail (the invalid-civil-date rule). */
                int r = q + 11;
                if (!(tok_dig_run(src, r) == 2 && src[r + 2] == ':' &&
                      tok_dig_run(src, r + 3) == 2 && src[r + 5] == ':' &&
                      tok_dig_run(src, r + 6) == 2))
                    { *err = "bad timestamp"; return -1; }
                int64_t h  = (src[r]     - '0') * 10 + (src[r + 1] - '0');
                int64_t mi = (src[r + 3] - '0') * 10 + (src[r + 4] - '0');
                int64_t s  = (src[r + 6] - '0') * 10 + (src[r + 7] - '0');
                if (mi >= 60 || s >= 60) { *err = "bad timestamp"; return -1; }
                int64_t frac = 0;
                int end = r + 8;
                if (src[end] == '.') {
                    int fd = tok_dig_run(src, end + 1);
                    if (fd < 1 || fd > 9) { *err = "bad timestamp"; return -1; }
                    for (int k = 0; k < fd; k++)
                        frac = frac * 10 + (src[end + 1 + k] - '0');
                    for (int k = fd; k < 9; k++) frac *= 10;
                    end += 1 + fd;
                }
                int64_t tod = (h * 3600 + mi * 60 + s) * 1000000000LL + frac;
                out->kind = Q_TOK_EL_TS;
                out->i = q_calendar_ts_compose(q_calendar_days_from_civil(y, mo, d), tod);
                if (neg) out->i = -out->i;
                *p = end;
                return 1;
            }
            if (src[q + 10] == 'T') {
                /* Datetime literal: dateTtime (datatypes.md row 15, q type
                 * 15).  Full clock HH:MM:SS required (cast.md:172 pins the
                 * fraction-less 2017.08.23T23:50:12); a fraction of 1..3
                 * digits right-pads to MILLISECONDS (the time-literal rule —
                 * tok.md:227 pins the .123 form; display is always ms).
                 * Unlike the D timestamp arm the clock is a TIME OF DAY, so
                 * hours >= 24 die alongside mm/ss >= 60.  Payload = f64 days
                 * since 2000.01.01, fraction = tod/86400000ms. */
                int r = q + 11;
                if (!(tok_dig_run(src, r) == 2 && src[r + 2] == ':' &&
                      tok_dig_run(src, r + 3) == 2 && src[r + 5] == ':' &&
                      tok_dig_run(src, r + 6) == 2))
                    { *err = "bad datetime"; return -1; }
                int64_t h  = (src[r]     - '0') * 10 + (src[r + 1] - '0');
                int64_t mi = (src[r + 3] - '0') * 10 + (src[r + 4] - '0');
                int64_t sec = (src[r + 6] - '0') * 10 + (src[r + 7] - '0');
                if (h >= 24 || mi >= 60 || sec >= 60) { *err = "bad datetime"; return -1; }
                int64_t ms = 0;
                int end = r + 8;
                if (src[end] == '.') {
                    int fd = tok_dig_run(src, end + 1);
                    if (fd < 1 || fd > 3) { *err = "bad datetime"; return -1; }
                    for (int k = 0; k < fd; k++)
                        ms = ms * 10 + (src[end + 1 + k] - '0');
                    for (int k = fd; k < 3; k++) ms *= 10;
                    end += 1 + fd;
                }
                double tod = ((double)(h * 3600 + mi * 60 + sec) * 1000.0 +
                              (double)ms) / 86400000.0;
                out->kind = Q_TOK_EL_DT;
                out->f = (double)q_calendar_days_from_civil(y, mo, d) + tod;
                if (neg) out->f = -out->f;   /* glued sign negates the payload
                                              * (the kdb date-literal rule;
                                              * derived for the T form) */
                *p = end;
                return 1;
            }
            out->kind = Q_TOK_EL_DATE;
            out->i = q_calendar_days_from_civil(y, mo, d);
            if (neg) out->i = -out->i;
            *p = q + 10;
            return 1;
        }
    }

    /* Month-SHAPED magnitude: yyyy.mm (4-2 digits), terminator neither digit
     * nor dot nor an exponent continuation.  UNLIKE date, the month shape IS
     * a valid float spelling (kdb bare `2000.01` is the float 2000.01; only
     * the trailing `m` letter makes it a month), so this arm cannot commit:
     * it records BOTH the month payload (.i = months since 2000.01) and the
     * float twin (.f) with forces_float=1 — the `m` context in
     * scan_num_literal reads .i, every other context reverts to the float via
     * el_to_float's Q_TOK_EL_MONTH arm.  A glued sign negates the payload (the
     * kdb date-literal rule).  An invalid civil month (2000.13 / 2000.00)
     * stays a float — EXCEPT when the very next byte is the `m` letter
     * (2000.13m), which can only be a malformed month literal: die, mirroring
     * the date arm's invalid-civil rule.  Year 0000 is out of the kdb domain
     * and stays a float. */
    {
        int q = *p;
        int neg = (src[q] == '-');
        if (neg) q++;
        if (tok_dig_run(src, q) == 4 && src[q + 4] == '.' &&
            tok_dig_run(src, q + 5) == 2 &&
            src[q + 7] != '.' &&
            !((src[q + 7] == 'e' || src[q + 7] == 'E') &&
              (src[q + 8] == '+' || src[q + 8] == '-' ||
               (src[q + 8] >= '0' && src[q + 8] <= '9')))) {
            int64_t y  = (src[q]     - '0') * 1000 + (src[q + 1] - '0') * 100
                       + (src[q + 2] - '0') * 10   + (src[q + 3] - '0');
            int64_t mo = (src[q + 5] - '0') * 10 + (src[q + 6] - '0');
            int valid = (y >= 1 && mo >= 1 && mo <= 12);
            if (!valid && src[q + 7] == 'm') { *err = "bad number"; return -1; }
            if (valid) {
                size_t rem2 = strlen(src + *p);
                double fv; size_t u = ray_parse_f64(src + *p, rem2, &fv);
                if (u == (size_t)(q + 7 - *p)) {   /* float twin spans yyyy.mm */
                    out->kind = Q_TOK_EL_MONTH;
                    out->i = (y - 2000) * 12 + (mo - 1);
                    if (neg) out->i = -out->i;
                    out->f = fv;
                    out->forces_float = 1;
                    *p = q + 7;
                    return 1;
                }
            }
        }
    }

    /* Time literal magnitude: HH:MM:SS.f (2-2-2 clock digits + a dot + 1..3
     * fractional digits, padded to milliseconds).  Checked before the float
     * peek for the same reason as date.  The 1..3-digit gate is THE
     * disambiguation from the adjacent temporal shapes (basics/syntax.md):
     * timespan `00:00:00.000000000` has 9 fractional digits (>=4 -> this shape
     * fails -> falls through to today's name-error, deferred); second
     * `00:00:00` and minute `00:00` have no `.f` and also stay name-errors
     * (minute/second/timespan have no engine type yet).  kdb accepts 1..3
     * fractional digits and pads to ms (`.1`->100, `.11`->110, `.111`->111);
     * time always DISPLAYS 3 fractional digits.  kdb time == i32 milliseconds
     * of day (the base RAY_TIME payload).  A leading sign already glued by the
     * scanner negates the ms count.  m>=60 / s>=60 die rather than fall to the
     * float mess. */
    {
        int q = *p;
        int neg = (src[q] == '-');
        if (neg) q++;
        /* The clock-digit / ':' / '.' checks short-circuit BEFORE reading the
         * fractional run, so tok_dig_run(q+9) is only reached once src[q+8]=='.' is
         * confirmed in-bounds (else a short input overruns the buffer). */
        if (tok_dig_run(src, q) == 2 && src[q + 2] == ':' &&
            tok_dig_run(src, q + 3) == 2 && src[q + 5] == ':' &&
            tok_dig_run(src, q + 6) == 2 && src[q + 8] == '.') {
            int fd = tok_dig_run(src, q + 9);         /* fractional-digit run length */
            if (fd >= 1 && fd <= 3) {
                int64_t h  = (src[q]     - '0') * 10 + (src[q + 1] - '0');
                int64_t mi = (src[q + 3] - '0') * 10 + (src[q + 4] - '0');
                int64_t s  = (src[q + 6] - '0') * 10 + (src[q + 7] - '0');
                int64_t ms = 0;                   /* fractional -> milliseconds */
                for (int k = 0; k < fd; k++) ms = ms * 10 + (src[q + 9 + k] - '0');
                for (int k = fd; k < 3; k++) ms *= 10; /* right-pad to 3 digits */
                if (mi >= 60 || s >= 60) { *err = "bad time"; return -1; }
                out->kind = Q_TOK_EL_TIME;
                out->i = h * 3600000 + mi * 60000 + s * 1000 + ms;
                if (neg) out->i = -out->i;
                *p = q + 9 + fd;
                return 1;
            }
            if (fd >= 4 && fd <= 9) {
                /* Timespan clock form: HH:MM:SS. + 4..9 fractional digits,
                 * right-padded to nanoseconds (the pinned spelling is the
                 * 9-digit 12:00:00.000000000, datatypes.md:134; 4..8 derived
                 * — mirrors the timestamp arm's 1..9 pad). */
                int64_t h  = (src[q]     - '0') * 10 + (src[q + 1] - '0');
                int64_t mi = (src[q + 3] - '0') * 10 + (src[q + 4] - '0');
                int64_t s  = (src[q + 6] - '0') * 10 + (src[q + 7] - '0');
                int64_t ns = 0;
                for (int k = 0; k < fd; k++) ns = ns * 10 + (src[q + 9 + k] - '0');
                for (int k = fd; k < 9; k++) ns *= 10;
                if (mi >= 60 || s >= 60) { *err = "bad timespan"; return -1; }
                out->kind = Q_TOK_EL_TIMESPAN;
                out->i = (h * 3600 + mi * 60 + s) * 1000000000LL + ns;
                if (neg) out->i = -out->i;
                *p = q + 9 + fd;
                return 1;
            }
        }
    }

    /* Second literal magnitude: HH:MM:SS with the terminator neither '.'
     * (time / timespan clock-frac shapes above) nor ':' nor a digit
     * (basics/syntax.md:90).  The three clock shapes are mutually
     * exclusive by terminator, so ordering here is not load-bearing. */
    {
        int q = *p;
        int neg = (src[q] == '-');
        if (neg) q++;
        if (tok_dig_run(src, q) == 2 && src[q + 2] == ':' &&
            tok_dig_run(src, q + 3) == 2 && src[q + 5] == ':' &&
            tok_dig_run(src, q + 6) == 2 &&
            src[q + 8] != '.' && src[q + 8] != ':' &&
            !(tok_digit(src[q + 8]))) {
            int64_t h  = (src[q]     - '0') * 10 + (src[q + 1] - '0');
            int64_t mi = (src[q + 3] - '0') * 10 + (src[q + 4] - '0');
            int64_t s  = (src[q + 6] - '0') * 10 + (src[q + 7] - '0');
            if (mi >= 60 || s >= 60) { *err = "bad second"; return -1; }
            out->kind = Q_TOK_EL_SECOND;
            out->i = h * 3600 + mi * 60 + s;
            if (neg) out->i = -out->i;
            *p = q + 8;
            return 1;
        }
    }

    /* Minute literal magnitude: HH:MM with the terminator neither ':'
     * (second/time shapes) nor '.' nor a digit (basics/syntax.md:89). */
    {
        int q = *p;
        int neg = (src[q] == '-');
        if (neg) q++;
        if (tok_dig_run(src, q) == 2 && src[q + 2] == ':' &&
            tok_dig_run(src, q + 3) == 2 &&
            src[q + 5] != ':' && src[q + 5] != '.' &&
            !(tok_digit(src[q + 5]))) {
            int64_t h  = (src[q]     - '0') * 10 + (src[q + 1] - '0');
            int64_t mm = (src[q + 3] - '0') * 10 + (src[q + 4] - '0');
            if (mm >= 60) { *err = "bad minute"; return -1; }
            out->kind = Q_TOK_EL_MINUTE;
            out->i = h * 60 + mm;
            if (neg) out->i = -out->i;
            *p = q + 5;
            return 1;
        }
    }

    /* Timespan D-form: digits 'D' [HH[:MM[:SS[.f{1,9}]]]] (interfaces
     * usage 0D00:05 / 0D00:00:10; day-count payload derived).  Only
     * matches when 'D' is followed by exactly-2 clock digits whose next
     * byte does not continue a name, or by a byte that cannot continue a
     * name at all — `1D45x` stays a name juxtaposition and `0Dabc` stays
     * `0` + `Dabc` (the no-churn rule).  Hour overflow normalizes through
     * the ns count (`123D45` -> 124D21:…, the timestamp-arm D24 rule).
     * The date arm ran first, so `2000.01.01D…` never reaches here. */
    {
        int q = *p;
        int neg = (src[q] == '-');
        if (neg) q++;
        int dd = tok_dig_run(src, q);
        if (dd >= 1 && src[q + dd] == 'D') {
            int r = q + dd + 1;
            int hd = tok_dig_run(src, r);
            int matched = 0;
            int64_t days = 0, tod_s = 0, ns = 0;
            for (int k = 0; k < dd; k++) days = days * 10 + (src[q + k] - '0');
            if (hd == 2) {
                int64_t h = (src[r] - '0') * 10 + (src[r + 1] - '0');
                int e = r + 2;
                int64_t mi = 0, ss = 0;
                if (src[e] == ':' && tok_dig_run(src, e + 1) == 2) {
                    mi = (src[e + 1] - '0') * 10 + (src[e + 2] - '0');
                    e += 3;
                    if (src[e] == ':' && tok_dig_run(src, e + 1) == 2) {
                        ss = (src[e + 1] - '0') * 10 + (src[e + 2] - '0');
                        e += 3;
                        if (src[e] == '.') {
                            int fd = tok_dig_run(src, e + 1);
                            if (fd < 1 || fd > 9) { *err = "bad timespan"; return -1; }
                            for (int k = 0; k < fd; k++)
                                ns = ns * 10 + (src[e + 1 + k] - '0');
                            for (int k = fd; k < 9; k++) ns *= 10;
                            e += 1 + fd;
                        }
                    }
                }
                if (mi >= 60 || ss >= 60) { *err = "bad timespan"; return -1; }
                /* A name byte right after the clock digits means this was
                 * a name after all (e.g. 1D45x) — not a timespan. */
                if (!(tok_digit(src[e])) &&
                    !((src[e] >= 'a' && src[e] <= 'z') ||
                      (src[e] >= 'A' && src[e] <= 'Z') || src[e] == '_')) {
                    tod_s = h * 3600 + mi * 60 + ss;
                    out->kind = Q_TOK_EL_TIMESPAN;
                    out->i = (days * 86400 + tod_s) * 1000000000LL + ns;
                    if (neg) out->i = -out->i;
                    *p = e;
                    matched = 1;
                }
            } else if (hd == 0 &&
                       !((src[r] >= 'a' && src[r] <= 'z') ||
                         (src[r] >= 'A' && src[r] <= 'Z') ||
                         src[r] == '_' || src[r] == '.' || src[r] == ':')) {
                /* Bare dD day count (kdb 1D; derived — no doc example uses
                 * a bare form as input, PR-noted). */
                out->kind = Q_TOK_EL_TIMESPAN;
                out->i = days * 86400000000000LL;
                if (neg) out->i = -out->i;
                *p = q + dd + 1;
                matched = 1;
            }
            if (matched) return 1;
        }
    }
    return 0;
}

/* ---- whole-literal construction --------------------------------------------
 * A literal is a space-separated run of magnitudes plus at most one trailing
 * type letter (qlang.g4): the magnitudes scan uniformly, the letter fixes the
 * type (none => long, or float if any magnitude was fractional).  Nulls and
 * integer infinities are Specials that widen to the chosen type's sentinel.
 * Every TEMPORAL type then builds identically — reject the foreign element
 * kinds, atom-or-vector over one payload width — so the eight of them are ONE
 * table walked by one builder (they were eight hand-copied arms until
 * 2026-07-30; each new type meant editing the other seven reject lists).
 * Doc pins: basics/datatypes.md rows 12-19. */

#define LIT_ERR(M) do { *err = (M); return NULL; } while (0)

static int lit_ws(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

/* q Specials: 0N/0n (null), 0W/0w (+inf), -0W/-0w (-inf).  Lowercase forces a
 * float context.  Returns bytes consumed (0 = not a Special). */
static int lit_special(const char *s, int p, q_tok_el *out) {
    int neg = (s[p] == '-');
    int q = p + (neg ? 1 : 0);
    if (s[q] != '0') return 0;
    char k = s[q + 1];
    if (k != 'N' && k != 'W' && k != 'n' && k != 'w') return 0;
    int is_null = (k == 'N' || k == 'n');
    if (neg && is_null) return 0;            /* -0N is not a literal */
    out->forces_float = (k == 'n' || k == 'w');
    out->i = 0; out->f = 0.0;
    out->kind = is_null ? Q_TOK_EL_NULL : (neg ? Q_TOK_EL_NINF : Q_TOK_EL_PINF);
    return (q + 2) - p;
}

/* Scan one magnitude at src[*p]: 1 on success, 0 on no match, -1 with *err on a
 * malformed temporal shape (an invalid civil date never falls back to a float). */
static int lit_magnitude(const char *src, int *p, q_tok_el *out, const char **err) {
    out->forces_float = 0;
    int used = lit_special(src, *p, out);
    if (used) { *p += used; return 1; }

    int tm = q_tok_temporal(src, p, out, err);
    if (tm) return tm;

    /* Decide float vs int: a float magnitude contains '.' or an exponent among
     * its own bytes (before the next whitespace / letter).  Peek the digit run. */
    int q = *p;
    if (src[q] == '-' || src[q] == '+') q++;
    int is_float = 0, saw_digit = 0;
    for (int r = q; ; r++) {
        char c = src[r];
        if (c >= '0' && c <= '9') { saw_digit = 1; continue; }
        if (c == '.') { is_float = 1; continue; }
        if ((c == 'e' || c == 'E') && saw_digit &&
            (src[r + 1] == '+' || src[r + 1] == '-' ||
             (src[r + 1] >= '0' && src[r + 1] <= '9'))) { is_float = 1; continue; }
        break;
    }
    if (!saw_digit) return 0;

    size_t rem = strlen(src + *p);
    if (is_float) {
        double v; size_t u = ray_parse_f64(src + *p, rem, &v);
        if (u == 0) return 0;
        *p += (int)u; out->kind = Q_TOK_EL_FLOAT; out->f = v; out->forces_float = 1;
        return 1;
    }
    int64_t v; size_t u = ray_parse_i64(src + *p, rem, &v);
    if (u == 0) return 0;
    *p += (int)u; out->kind = Q_TOK_EL_INT; out->i = v;
    return 1;
}

/* Widen the long sentinels/inf to a narrow int width (2 or 4 bytes). */
static int64_t lit_narrow_special(q_tok_el_kind k, int width) {
    int64_t vmin = (width == 2) ? INT16_MIN : (width == 4) ? INT32_MIN : INT64_MIN;
    int64_t vmax = (width == 2) ? INT16_MAX : (width == 4) ? INT32_MAX : INT64_MAX;
    if (k == Q_TOK_EL_NULL) return vmin;
    if (k == Q_TOK_EL_PINF) return vmax;
    return -vmax;   /* Q_TOK_EL_NINF */
}

/* Resolve one element to an int64 in the given integer width.  Every temporal
 * kind carries its own payload in .i (reachable only in its own context). */
static int64_t lit_int(const q_tok_el *e, int width) {
    if (e->kind == Q_TOK_EL_FLOAT) return (int64_t)e->f;
    if (e->kind == Q_TOK_EL_NULL || e->kind == Q_TOK_EL_PINF || e->kind == Q_TOK_EL_NINF)
        return lit_narrow_special(e->kind, width);
    return e->i;
}

/* Resolve one element to a double.  Q_TOK_EL_MONTH must return its float TWIN
 * (.f), not the month payload — a bare `2000.01` is the float 2000.01 (review
 * C1) — and Q_TOK_EL_DT keeps its f64 day count there too.  ±inf are LIVE
 * values, never nulls (live-infinity model 2026-07-28). */
static double lit_float(const q_tok_el *e) {
    if (e->kind == Q_TOK_EL_NULL) return NULL_F64;
    if (e->kind == Q_TOK_EL_PINF) return INFINITY;
    if (e->kind == Q_TOK_EL_NINF) return -INFINITY;
    if (e->kind == Q_TOK_EL_FLOAT || e->kind == Q_TOK_EL_MONTH ||
        e->kind == Q_TOK_EL_DT) return e->f;
    return (double)e->i;
}

static ray_t *lit_mark_nulls(ray_t *vec, const q_tok_el *buf, int m) {
    if (vec && !RAY_IS_ERR(vec))
        for (int i = 0; i < m; i++)
            if (buf[i].kind == Q_TOK_EL_NULL) ray_vec_set_null(vec, i, true);
    return vec;
}

/* One temporal literal context.  `bare` = a magnitude of that shape commits to
 * the type on its own; month is the exception (bare `2000.01` is the float —
 * only the `m` letter reads the month payload).  `width` 0 = f64 (datetime). */
typedef struct {
    char           letter;
    q_tok_el_kind  kind;
    int8_t         type;
    uint8_t        width;
    uint8_t        bare;
    ray_t        *(*atom)(int64_t);
} lit_ctx;

static const lit_ctx LIT_CTX[] = {
    /* timestamp FIRST: a mixed date+timestamp strand must promote days->ns
     * rather than truncate ns into an i32 date. */
    { 'p', Q_TOK_EL_TS,       RAY_TIMESTAMP, 8, 1, ray_timestamp },
    { 'd', Q_TOK_EL_DATE,     RAY_DATE,      4, 1, ray_date      },
    { 't', Q_TOK_EL_TIME,     RAY_TIME,      4, 1, ray_time      },
    { 'm', Q_TOK_EL_MONTH,    RAY_MONTH,     4, 0, ray_month     },
    { 'u', Q_TOK_EL_MINUTE,   RAY_MINUTE,    4, 1, ray_minute    },
    { 'v', Q_TOK_EL_SECOND,   RAY_SECOND,    4, 1, ray_second    },
    { 'n', Q_TOK_EL_TIMESPAN, RAY_TIMESPAN,  8, 1, ray_timespan  },
    { 'z', Q_TOK_EL_DT,       RAY_DATETIME,  0, 1, NULL          },
};

/* An element belongs to context c iff it is c's own shape, a Special, or a
 * plain int (a raw payload count), plus the date->timestamp promotion.  A
 * float-forcing element (a fraction, or a lowercase `0w`) may only be c's own
 * shape or the null — `0nd` is the K-ism spelling of `0Nd`, `0wd` is not a
 * literal. */
static int lit_el_ok(const q_tok_el *e, const lit_ctx *c) {
    if (e->forces_float && e->kind != Q_TOK_EL_NULL && e->kind != c->kind) return 0;
    if (e->kind == c->kind || e->kind == Q_TOK_EL_INT) return 1;
    if (e->kind == Q_TOK_EL_NULL || e->kind == Q_TOK_EL_PINF ||
        e->kind == Q_TOK_EL_NINF) return 1;
    return c->kind == Q_TOK_EL_TS && e->kind == Q_TOK_EL_DATE;
}

static int64_t lit_payload(const lit_ctx *c, const q_tok_el *e) {
    if (c->kind == Q_TOK_EL_TS && e->kind == Q_TOK_EL_DATE)
        return q_calendar_ts_compose(e->i, 0);      /* days -> ns */
    return lit_int(e, c->width);
}

static ray_t *lit_temporal(const lit_ctx *c, const q_tok_el *buf, int m) {
    if (c->width == 0) {                            /* datetime: f64 payload */
        if (m == 1) {
            if (buf[0].kind == Q_TOK_EL_DT) return ray_datetime(buf[0].f);
            if (buf[0].kind == Q_TOK_EL_PINF) return ray_datetime(INFINITY);
            if (buf[0].kind == Q_TOK_EL_NINF) return ray_datetime(-INFINITY);
            return ray_typed_null(-RAY_DATETIME);   /* incl. a bare int: see PLAN.md */
        }
        double t[MAX_VEC];
        for (int i = 0; i < m; i++) t[i] = lit_float(&buf[i]);
        return lit_mark_nulls(ray_vec_from_raw(RAY_DATETIME, t, m), buf, m);
    }
    if (m == 1)
        return buf[0].kind == Q_TOK_EL_NULL ? ray_typed_null((int8_t)-c->type)
                                            : c->atom(lit_payload(c, &buf[0]));
    if (c->width == 4) {
        int32_t t[MAX_VEC];
        for (int i = 0; i < m; i++) t[i] = (int32_t)lit_payload(c, &buf[i]);
        return lit_mark_nulls(ray_vec_from_raw(c->type, t, m), buf, m);
    }
    int64_t t[MAX_VEC];
    for (int i = 0; i < m; i++) t[i] = lit_payload(c, &buf[i]);
    return lit_mark_nulls(ray_vec_from_raw(c->type, t, m), buf, m);
}

/* Read an optional trailing type letter at src[*p].  b/h/i/j/e/f are always
 * available; every TEMPORAL letter is gated on the preceding magnitude being
 * that type's own shape or a Special, so corpus tokens like `3d` / `3t` keep
 * parsing as `3` juxtaposed with the name `d` / `t` (no parse-display churn).
 * `g` (guid) is null-only — guid has no infinity and no other literal
 * (basics/datatypes.md §Guid). */
/* 0N / 0W / -0W: TYPELESS, so a Special takes its type from its own letter. */
static int lit_el_special(const q_tok_el *e) {
    return e->kind == Q_TOK_EL_NULL || e->kind == Q_TOK_EL_PINF ||
           e->kind == Q_TOK_EL_NINF;
}

static int lit_type_letter(const char *src, int *p, char *letter,
                           const q_tok_el *last, const char **err) {
    char c = src[*p];
    if (!c || !last) return 1;
    int special = lit_el_special(last);
    int ok = strchr("bhijef", c) != NULL || (c == 'g' && last->kind == Q_TOK_EL_NULL);
    for (size_t k = 0; !ok && k < sizeof LIT_CTX / sizeof *LIT_CTX; k++)
        ok = (c == LIT_CTX[k].letter && (special || last->kind == LIT_CTX[k].kind));
    if (!ok) return 1;
    if (*letter && *letter != c) { *err = "inconsistent numeric type suffix"; return 0; }
    *letter = c;
    (*p)++;
    return 1;
}

/* ---- byte literals (q type 4, char x) --------------------------------------
 * Glued `0x` enters byte-literal mode: consume the maximal hex-digit run.
 * Doc pins (CLEAN ROOM, qdocs/): basics/datatypes.md row 4 (`0x00`);
 * ref/sv.md `0x0 sv …` (single digit = atom); ref/read1.md `0#0x` (bare `0x`
 * = EMPTY byte vector); ref/sv.md `0x0102010201` (multi-digit = vector).
 * Derived (no doc pin, most defensible reading): an odd digit count left-pads
 * one zero nibble (generalizes the pinned `0x0` -> 0x00); uppercase hex
 * digits are accepted (display is always lowercase); a run terminated by a
 * letter / '_' / '.' (`0xzz`, `0x0az`, `0x1.5`) is a malformed constant.
 * Bytes have NO null / infinity / type letter (datatypes.md blank columns). */
static int lit_hex_digit(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
           (c >= 'A' && c <= 'F');
}
static int lit_hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return c - 'A' + 10;
}
int q_tok_byte_lit_starts(const char *src, int p) {
    return src[p] == '0' && src[p + 1] == 'x';
}
static ray_t *lit_byte(const char *src, int *p, const char **err) {
    int q = *p + 2;                               /* past "0x" */
    int d0 = q;
    while (lit_hex_digit(src[q])) q++;
    int nd = q - d0;
    char t = src[q];                              /* run terminator */
    if ((t >= 'a' && t <= 'z') || (t >= 'A' && t <= 'Z') || t == '_' || t == '.')
        LIT_ERR("bad number");                    /* 0xzz / 0x0az / 0x1.5 */
    if (nd > 2 * MAX_VEC) LIT_ERR("numeric literal too long");
    uint8_t bytes[MAX_VEC]; int nb = 0;
    int i = d0;
    if (nd & 1) bytes[nb++] = (uint8_t)lit_hex_val(src[i++]);   /* left-pad nibble */
    for (; i < q; i += 2)
        bytes[nb++] = (uint8_t)((lit_hex_val(src[i]) << 4) | lit_hex_val(src[i + 1]));
    *p = q;
    if (nb == 1) return ray_u8(bytes[0]);
    return ray_vec_from_raw(RAY_BYTE_ONLY, bytes, nb);   /* nb==0: empty byte vec */
}

ray_t* q_tok_literal(const char *src, int *p, const char **err) {
    *err = NULL;
    if (q_tok_byte_lit_starts(src, *p)) return lit_byte(src, p, err);
    int start = *p;
    q_tok_el buf[MAX_VEC]; int m = 0;
    char letter = 0;
    if (lit_magnitude(src, p, &buf[m++], err) != 1)
        LIT_ERR(*err ? *err : "bad number");
    if (!lit_type_letter(src, p, &letter, &buf[m - 1], err)) return NULL;
    int closed = letter && !lit_el_special(&buf[m - 1]);
    for (;;) {
        int sp = *p;
        while (lit_ws(src[sp])) sp++;
        if (sp == *p) break;                     /* no space => run ended */
        if (q_tok_byte_lit_starts(src, sp)) break;   /* byte literal: own noun */
        q_tok_el e; int q = sp;
        int got = lit_magnitude(src, &q, &e, err);
        if (got < 0) return NULL;
        if (!got) break;                         /* not another magnitude */
        /* A letter on a MAGNITUDE closes the literal: q prints exactly one
         * trailing suffix for the whole vector (basics/datatypes.md:254-259),
         * so `1h 2h` is not a q spelling (owner ruling 2026-07-30) — `1 2h` is.
         * A letter on a SPECIAL is that element's own type (0N/0W carry no
         * type of their own), which is why `0Nu 0Wu 09:30` stays a literal. */
        if (closed) LIT_ERR("type suffix must end the literal");
        if (m >= MAX_VEC) LIT_ERR("numeric literal too long");
        *p = q; buf[m++] = e;
        if (!lit_type_letter(src, p, &letter, &buf[m - 1], err)) return NULL;
        closed = letter && !lit_el_special(&buf[m - 1]);
    }

    /* Booleans: a 0/1 run ending in 'b' (spaces flattened). */
    if (letter == 'b') {
        uint8_t bits[MAX_VEC]; int nb = 0;
        for (int i = start; i < *p - 1; i++) {
            if (src[i] == ' ' || src[i] == '\t') continue;
            if (src[i] != '0' && src[i] != '1') LIT_ERR("bad boolean literal");
            if (nb >= MAX_VEC) LIT_ERR("boolean literal too long");
            bits[nb++] = (uint8_t)(src[i] - '0');
        }
        if (nb == 0) LIT_ERR("bad boolean literal");
        if (nb == 1) return ray_bool(bits[0]);
        return ray_vec_from_raw(RAY_BOOL, bits, nb);
    }

    /* Guid: the null is guid's ONLY literal (basics/datatypes.md §Guid), so
     * every element must be 0N; a multi-0N run builds an all-null guid vector
     * by the same strand-suffix rule as `1 2h` (derived). */
    if (letter == 'g') {
        for (int i = 0; i < m; i++)
            if (buf[i].kind != Q_TOK_EL_NULL) LIT_ERR("bad number");
        if (m == 1) return ray_typed_null(-RAY_GUID);
        ray_t *vec = ray_vec_new(RAY_GUID, m);
        if (vec && !RAY_IS_ERR(vec)) {
            vec->len = m;
            memset(ray_data(vec), 0, (size_t)m * 16);   /* all-null guids */
        }
        return vec;
    }

    for (size_t k = 0; k < sizeof LIT_CTX / sizeof *LIT_CTX; k++) {
        const lit_ctx *c = &LIT_CTX[k];
        int hit = (letter == c->letter);
        for (int i = 0; i < m && !hit; i++)
            if (c->bare && buf[i].kind == c->kind) hit = 1;
        if (!hit) continue;
        for (int i = 0; i < m; i++)
            if (!lit_el_ok(&buf[i], c)) LIT_ERR("bad number");
        return lit_temporal(c, buf, m);
    }

    /* Float context: explicit e/f letter, or any fractional/lowercase-special. */
    int is_float = (letter == 'e' || letter == 'f');
    for (int i = 0; i < m && !is_float; i++)
        if (buf[i].forces_float) is_float = 1;
    if (is_float) {
        int f32 = (letter == 'e');
        if (m == 1) {
            double v = lit_float(&buf[0]);
            return f32 ? ray_f32((float)v) : ray_f64(v);
        }
        if (f32) {
            float t[MAX_VEC];
            for (int i = 0; i < m; i++) t[i] = (float)lit_float(&buf[i]);
            return lit_mark_nulls(ray_vec_from_raw(RAY_F32, t, m), buf, m);
        }
        double t[MAX_VEC];
        for (int i = 0; i < m; i++) t[i] = lit_float(&buf[i]);
        return lit_mark_nulls(ray_vec_from_raw(RAY_F64, t, m), buf, m);
    }

    /* Integer context: h=i16, i=i32, j/none=i64.  Without HAS_NULLS a
     * reduction cannot tell 0Ni/0Nh from data. */
    int width = (letter == 'h') ? 2 : (letter == 'i') ? 4 : 8;
    if (m == 1) {
        int64_t v = lit_int(&buf[0], width);
        if (width == 2) return ray_i16((int16_t)v);
        if (width == 4) return ray_i32((int32_t)v);
        return ray_i64(v);
    }
    if (width == 8) {
        int64_t t[MAX_VEC];
        for (int i = 0; i < m; i++) t[i] = lit_int(&buf[i], 8);
        return lit_mark_nulls(ray_vec_from_raw(RAY_I64, t, m), buf, m);
    }
    if (width == 4) {
        int32_t t[MAX_VEC];
        for (int i = 0; i < m; i++) t[i] = (int32_t)lit_int(&buf[i], 4);
        return lit_mark_nulls(ray_vec_from_raw(RAY_I32, t, m), buf, m);
    }
    int16_t t[MAX_VEC];
    for (int i = 0; i < m; i++) t[i] = (int16_t)lit_int(&buf[i], 2);
    return lit_mark_nulls(ray_vec_from_raw(RAY_I16, t, m), buf, m);
}

#undef LIT_ERR

/* ===== 2. `$` Tok whole-string scanners ===== */

/* "D"$ date-string scan (ref/tok.md date formats).  Supported subset:
 * yyyymmdd (8 digits, the doc's [yy]yymmdd with an unambiguous 4-digit year)
 * and yyyy.mm.dd / yyyy-mm-dd / yyyy/mm/dd (the doc's separator variants;
 * "D"$"2000-12-12" is letter-pinned).  Two-digit years and MMM month names
 * are deferred.  Returns 1 and fills y/m/d on a shape match; civil validity
 * is the caller's q_calendar_date_valid check. */
int q_tok_date(const char* p, size_t len,
                       int64_t* y, int64_t* m, int64_t* d) {
    if (len == 8) {
        for (int i = 0; i < 8; i++)
            if (p[i] < '0' || p[i] > '9') return 0;
        *y = (p[0]-'0')*1000 + (p[1]-'0')*100 + (p[2]-'0')*10 + (p[3]-'0');
        *m = (p[4]-'0')*10 + (p[5]-'0');
        *d = (p[6]-'0')*10 + (p[7]-'0');
        return 1;
    }
    if (len == 10 && (p[4] == '.' || p[4] == '-' || p[4] == '/') && p[7] == p[4]) {
        for (int i = 0; i < 10; i++) {
            if (i == 4 || i == 7) continue;
            if (p[i] < '0' || p[i] > '9') return 0;
        }
        *y = (p[0]-'0')*1000 + (p[1]-'0')*100 + (p[2]-'0')*10 + (p[3]-'0');
        *m = (p[5]-'0')*10 + (p[6]-'0');
        *d = (p[8]-'0')*10 + (p[9]-'0');
        return 1;
    }
    return 0;
}

static int tok_hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Parse a CANONICAL 36-char UUID (8-4-4-4-12, hyphens at 8/13/18/23, hex
 * elsewhere, case-insensitive) into out[16].  Returns 1 on success, 0 on any
 * shape/char mismatch.  kdb "G"$ ALSO accepts IPv4/IPv6 address forms
 * (tok.md #ip-address) — those fall through to tok_ip_guid below. */
int q_tok_uuid(const char* p, size_t len, uint8_t out[16]) {
    if (len != 36) return 0;
    int bi = 0;
    for (size_t i = 0; i < 36; ) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (p[i] != '-') return 0;
            i++;
            continue;
        }
        int h = tok_hexval(p[i]);
        int l = tok_hexval(p[i + 1]);
        if (h < 0 || l < 0) return 0;
        out[bi++] = (uint8_t)((h << 4) | l);
        i += 2;
    }
    return bi == 16;
}

/* Parse p[0..len) as a dotted-quad IPv4 (four decimal octets 0..255, three
 * dots, nothing else).  Returns 1 + oct[0..3], 0 on any shape/range mismatch. */
static int tok_ipv4_octets(const char* p, size_t len, uint8_t oct[4]) {
    size_t i = 0;
    for (int n = 0; n < 4; n++) {
        if (i >= len || p[i] < '0' || p[i] > '9') return 0;
        int v = 0, dig = 0;
        while (i < len && p[i] >= '0' && p[i] <= '9') {
            v = v * 10 + (p[i] - '0'); i++;
            if (++dig > 3 || v > 255) return 0;
        }
        oct[n] = (uint8_t)v;
        if (n < 3) { if (i >= len || p[i] != '.') return 0; i++; }
    }
    return i == len;
}

/* IPv4 dotted-quad -> big-endian 32-bit ("I"$"192.168.1.34" -> -1062731486i,
 * tok.md #ipv4-address-as-int).  Returns 1 + *out, 0 on mismatch. */
int q_tok_ipv4(const char* p, size_t len, uint32_t* out) {
    uint8_t o[4];
    if (!tok_ipv4_octets(p, len, o)) return 0;
    *out = ((uint32_t)o[0] << 24) | ((uint32_t)o[1] << 16) |
           ((uint32_t)o[2] << 8) | (uint32_t)o[3];
    return 1;
}

/* IPv6 text -> 16 bytes (tok.md #ipv6-address-as-guid).  Handles the full
 * 8-group form, `::` zero-compression (at most one), and a trailing dotted-quad
 * (v4-mapped, e.g. "::FFFF:192.0.2.1").  Returns 1 + out, 0 on any mismatch. */
static int tok_ipv6(const char* p, size_t len, uint8_t out[16]) {
    if (len == 0) return 0;
    uint8_t head[16], tail[16];
    int hn = 0, tn = 0, seen_gap = 0;
    uint8_t* cur = head; int* cn = &hn;
    size_t i = 0;
    if (len >= 2 && p[0] == ':' && p[1] == ':') { seen_gap = 1; cur = tail; cn = &tn; i = 2; }
    else if (p[0] == ':') return 0;
    while (i < len) {
        size_t start = i;
        while (i < len && tok_hexval(p[i]) >= 0) i++;
        if (i < len && p[i] == '.') {                 /* trailing dotted-quad */
            uint8_t o4[4];
            if (!tok_ipv4_octets(p + start, len - start, o4) || *cn + 4 > 16) return 0;
            memcpy(cur + *cn, o4, 4); *cn += 4;
            i = len; break;
        }
        size_t glen = i - start;
        if (glen == 0 || glen > 4 || *cn + 2 > 16) return 0;
        uint16_t g = 0;
        for (size_t k = start; k < i; k++) g = (uint16_t)((g << 4) | tok_hexval(p[k]));
        cur[(*cn)++] = (uint8_t)(g >> 8);
        cur[(*cn)++] = (uint8_t)(g & 0xff);
        if (i == len) break;
        if (p[i] != ':') return 0;
        i++;
        if (i < len && p[i] == ':') {                 /* the "::" gap */
            if (seen_gap) return 0;
            seen_gap = 1; cur = tail; cn = &tn; i++;
        } else if (i == len) return 0;                /* trailing single colon */
    }
    if (seen_gap) {
        if (hn + tn >= 16) return 0;              /* `::` must compress >=1 group */
        memset(out, 0, 16);
        memcpy(out, head, (size_t)hn);
        memcpy(out + 16 - tn, tail, (size_t)tn);
    } else {
        if (hn != 16) return 0;
        memcpy(out, head, 16);
    }
    return 1;
}

/* "G"$ IP-address forms -> 16-byte guid: a bare dotted-quad maps to ::ffff:v4
 * ("192.0.2.1" -> 00000000-0000-0000-0000-ffffc0000201), else IPv6.  Returns
 * 1 + out, 0 on mismatch (caller yields 0Ng). */
static int tok_ip_guid(const char* p, size_t len, uint8_t out[16]) {
    uint8_t o4[4];
    if (tok_ipv4_octets(p, len, o4)) {
        memset(out, 0, 16);
        out[10] = 0xff; out[11] = 0xff;
        out[12] = o4[0]; out[13] = o4[1]; out[14] = o4[2]; out[15] = o4[3];
        return 1;
    }
    return tok_ipv6(p, len, out);
}

/* "T"$ time-string scan (ref/tok.md).  Two forms, both -> i32 ms of day:
 *   - PACKED digits HHMMSSmmm (doc-pinned): "T"$"123456789" -> 12:34:56.789,
 *     "T"$"123456123987654" -> 12:34:56.123 (>=6 digits: HH MM SS then up to 3
 *     fractional; extra fractional digits ignored).
 *   - COLON HH:MM:SS[.f…] (derived — the natural literal spelling): the `.`
 *     fractional is optional; only its first 3 digits (millis) are used.
 * mm/ss must be < 60, else out-of-domain.  Returns 1 and fills *ms on success,
 * 0 on any shape/range mismatch (caller -> typed null 0Nt). */
static int tok_all_digits(const char* p, size_t len) {
    if (len == 0) return 0;
    for (size_t i = 0; i < len; i++)
        if (p[i] < '0' || p[i] > '9') return 0;
    return 1;
}
int q_tok_time(const char* p, size_t len, int32_t* ms) {
    int64_t h, mi, s, frac = 0;
    int has_colon = 0;
    for (size_t i = 0; i < len; i++) if (p[i] == ':') { has_colon = 1; break; }
    /* colon form: H[H]:MM:SS[.f…] */
    if (has_colon) {
        size_t i = 0;
        int64_t hv = 0;
        while (i < len && p[i] >= '0' && p[i] <= '9') { hv = hv * 10 + (p[i] - '0'); i++; }
        if (i == 0 || i > 2 || i >= len || p[i] != ':') return 0;
        i++;
        if (i + 2 > len || !tok_all_digits(p + i, 2) || i + 2 >= len || p[i + 2] != ':')
            return 0;
        mi = (p[i] - '0') * 10 + (p[i + 1] - '0');
        i += 3;
        if (i + 2 > len || !tok_all_digits(p + i, 2)) return 0;
        s = (p[i] - '0') * 10 + (p[i + 1] - '0');
        i += 2;
        if (i < len) {                        /* optional .fractional */
            if (p[i] != '.') return 0;
            i++;
            int64_t scale = 100;
            size_t seen = 0;
            while (i < len && p[i] >= '0' && p[i] <= '9') {
                if (seen < 3) { frac += (p[i] - '0') * scale; scale /= 10; seen++; }
                i++;
            }
            if (i != len) return 0;           /* trailing junk */
        }
        h = hv;
        if (mi >= 60 || s >= 60) return 0;
        *ms = (int32_t)(h * 3600000 + mi * 60000 + s * 1000 + frac);
        return 1;
    }
    /* packed HHMMSSmmm: >=6 digits, first 6 = HHMMSS, next up to 3 = millis */
    if (len >= 6 && tok_all_digits(p, len)) {
        h  = (p[0] - '0') * 10 + (p[1] - '0');
        mi = (p[2] - '0') * 10 + (p[3] - '0');
        s  = (p[4] - '0') * 10 + (p[5] - '0');
        int64_t scale = 100;
        for (size_t i = 6; i < len && i < 9; i++) { frac += (p[i] - '0') * scale; scale /= 10; }
        if (mi >= 60 || s >= 60) return 0;
        *ms = (int32_t)(h * 3600000 + mi * 60000 + s * 1000 + frac);
        return 1;
    }
    return 0;
}

/* Clock scan for the duration Toks "U"$/"V"$/"N"$ -> ns.  Two forms
 * (the q_tok_time scheme generalised to ns):
 *   - PACKED digits HHMMSS + up to 9 fractional digits right-padded
 *     (doc-pinned for "N": tok.md:200 "N"$"123456123987654" ->
 *     0D12:34:56.123987654); >=4 digits HHMM accepted with SS=0 (derived).
 *   - COLON H[H]:MM[:SS[.f{1..9}]] (derived — the literal spellings).
 * mm/ss must be < 60.  Returns 1 and fills *ns, else 0 (caller -> null). */
int q_tok_clock_ns(const char* p, size_t len, int64_t* ns) {
    int64_t h = 0, mi = 0, s = 0, frac = 0;
    int has_colon = 0;
    for (size_t i = 0; i < len; i++) if (p[i] == ':') { has_colon = 1; break; }
    if (has_colon) {
        size_t i = 0;
        while (i < len && p[i] >= '0' && p[i] <= '9') { h = h * 10 + (p[i] - '0'); i++; }
        if (i == 0 || i > 2 || i >= len || p[i] != ':') return 0;
        i++;
        if (i + 2 > len || !tok_all_digits(p + i, 2)) return 0;
        mi = (p[i] - '0') * 10 + (p[i + 1] - '0');
        i += 2;
        if (i < len) {                        /* optional :SS[.f…] */
            if (p[i] != ':') return 0;
            i++;
            if (i + 2 > len || !tok_all_digits(p + i, 2)) return 0;
            s = (p[i] - '0') * 10 + (p[i + 1] - '0');
            i += 2;
            if (i < len) {
                if (p[i] != '.' || i + 1 == len) return 0;
                i++;
                size_t fd = len - i;
                if (fd > 9 || !tok_all_digits(p + i, fd)) return 0;
                for (size_t k = 0; k < fd; k++) frac = frac * 10 + (p[i + k] - '0');
                for (size_t k = fd; k < 9; k++) frac *= 10;
            }
        }
    } else if (len >= 4 && tok_all_digits(p, len)) {
        h  = (p[0] - '0') * 10 + (p[1] - '0');
        mi = (p[2] - '0') * 10 + (p[3] - '0');
        if (len >= 6) {
            s = (p[4] - '0') * 10 + (p[5] - '0');
            size_t fd = len - 6;
            if (fd > 9) return 0;
            for (size_t k = 0; k < fd; k++) frac = frac * 10 + (p[6 + k] - '0');
            for (size_t k = fd; k < 9; k++) frac *= 10;
        } else if (len != 4) return 0;
    } else return 0;
    if (mi >= 60 || s >= 60) return 0;
    *ns = (h * 3600 + mi * 60 + s) * 1000000000LL + frac;
    return 1;
}

/* "N"$ timespan scan: an optional `<days>D` prefix (1D02:03:04.005006007)
 * then the q_tok_clock_ns clock/packed form; days*86400e9 + tod via the
 * checked compose home.  Bare clock = 0 days (tok.md:200).  Sign/`dD…` forms
 * deferred like the clock scan -> caller yields 0Nn.  Returns 1 + *ns else 0. */
int q_tok_timespan_ns(const char* p, size_t len, int64_t* ns) {
    size_t i = 0;
    while (i < len && p[i] >= '0' && p[i] <= '9') i++;
    if (i > 0 && i < len && p[i] == 'D') {
        int64_t days = 0, tod;
        for (size_t k = 0; k < i; k++)                /* checked: a long run must not UB-overflow */
            if (__builtin_mul_overflow(days, (int64_t)10, &days) ||
                __builtin_add_overflow(days, (int64_t)(p[k] - '0'), &days))
                return 0;
        if (!q_tok_clock_ns(p + i + 1, len - i - 1, &tod)) return 0;
        return q_calendar_ts_compose_checked(days, tod, ns);
    }
    return q_tok_clock_ns(p, len, ns);
}

/* tod scan for "P"$: HH:MM:SS[.f{1..9}] -> ns of day (colon form only; the
 * packed date form is split off by the caller).  Returns 1/0. */
static int tok_tod_ns(const char* p, size_t len, int64_t* ns) {
    if (len < 8 || !tok_all_digits(p, 2) || p[2] != ':' ||
        !tok_all_digits(p + 3, 2) || p[5] != ':' || !tok_all_digits(p + 6, 2))
        return 0;
    int64_t h  = (p[0]-'0')*10 + (p[1]-'0');
    int64_t mi = (p[3]-'0')*10 + (p[4]-'0');
    int64_t s  = (p[6]-'0')*10 + (p[7]-'0');
    if (mi >= 60 || s >= 60) return 0;
    int64_t frac = 0;
    if (len > 8) {
        if (p[8] != '.' || len == 9) return 0;
        size_t fd = len - 9;
        if (fd > 9) return 0;
        for (size_t k = 0; k < fd; k++) {
            if (p[9 + k] < '0' || p[9 + k] > '9') return 0;
            frac = frac * 10 + (p[9 + k] - '0');
        }
        for (size_t k = fd; k < 9; k++) frac *= 10;
    }
    *ns = (h * 3600 + mi * 60 + s) * 1000000000LL + frac;
    return 1;
}

/* "P"$ timestamp-string scan (ref/tok.md Â§Timestamps).  Subset:
 *   - Unix seconds, 9..11 digits [+ . fraction] (doc-pinned:
 *     "P"$"10129708800" -> 2290.12.31D00:00:00.000000000,
 *     "P"$"10129708800.123456789" -> ...D00:00:00.123456789);
 *   - date part (q_tok_date separator forms or packed yyyymmdd) + one of
 *     "DT- " + colon tod (pins: "PZ"$\:"20191122-11:11:11.123");
 *   - date-only -> midnight (derived).
 * MMM months / 2-digit years / timezone forms deferred.  Returns 1 + payload
 * ns on success; 0 -> caller yields 0Np (tok.md out-of-domain contract —
 * CHECKED compose, never the cast path's saturating +-0Wp). */
int q_tok_ts(const char* p, size_t len, int64_t* out) {
    /* unix-seconds: 9..11 digits, optionally . + 1..9 fraction digits */
    size_t dot = len;
    for (size_t i = 0; i < len; i++) if (p[i] == '.') { dot = i; break; }
    if (dot >= 9 && dot <= 11 && tok_all_digits(p, dot) &&
        (dot == len || (len > dot + 1 && len <= dot + 10 &&
                        tok_all_digits(p + dot + 1, len - dot - 1)))) {
        int64_t secs = 0;
        for (size_t i = 0; i < dot; i++) secs = secs * 10 + (p[i] - '0');
        secs -= 946684800LL;                  /* unix epoch -> 2000.01.01 */
        int64_t ns;
        if (__builtin_mul_overflow(secs, 1000000000LL, &ns)) return 0;
        int64_t frac = 0;
        size_t fd = (dot == len) ? 0 : len - dot - 1;
        for (size_t k = 0; k < fd; k++) frac = frac * 10 + (p[dot + 1 + k] - '0');
        for (size_t k = fd; k < 9; k++) frac *= 10;
        if (__builtin_add_overflow(ns, frac, &ns)) return 0;
        *out = ns;
        return 1;
    }
    /* date [sep tod] */
    size_t dl = 0;
    if (len >= 10 && (p[4] == '.' || p[4] == '-' || p[4] == '/')) dl = 10;
    else if (len >= 8 && tok_all_digits(p, 8)) dl = 8;
    if (dl == 0 || len < dl) return 0;
    int64_t y, mo, d;
    if (!q_tok_date(p, dl, &y, &mo, &d) || !q_calendar_date_valid(y, mo, d)) return 0;
    int64_t tod = 0;
    if (len > dl) {
        char sep = p[dl];
        if (!(sep == 'D' || sep == 'T' || sep == '-' || sep == ' ')) return 0;
        if (!tok_tod_ns(p + dl + 1, len - dl - 1, &tod)) return 0;
    }
    return q_calendar_ts_compose_checked(q_calendar_days_from_civil(y, mo, d), tod, out);
}

/* "M"$str -> month payload (ref/tok.md designator table: month | -13 M).
 * Subset: "yyyy.mm" / "yyyy-mm" / "yyyy/mm" / packed yyyymm; the civil month
 * must be 01..12 and the year in the date domain [1,9999]. */
int q_tok_month(const char* p, size_t len, int64_t* months) {
    int64_t y = 0, mo = 0;
    int ok = 0;
    if (len == 7 && tok_all_digits(p, 4) &&
        (p[4] == '.' || p[4] == '-' || p[4] == '/') &&
        tok_all_digits(p + 5, 2)) {
        y  = (p[0]-'0')*1000 + (p[1]-'0')*100 + (p[2]-'0')*10 + (p[3]-'0');
        mo = (p[5]-'0')*10 + (p[6]-'0');
        ok = 1;
    } else if (len == 6 && tok_all_digits(p, 6)) {   /* packed yyyymm */
        y  = (p[0]-'0')*1000 + (p[1]-'0')*100 + (p[2]-'0')*10 + (p[3]-'0');
        mo = (p[4]-'0')*10 + (p[5]-'0');
        ok = 1;
    }
    if (!ok || mo < 1 || mo > 12 || y < 1 || y > 9999) return 0;
    *months = (y - 2000) * 12 + (mo - 1);
    return 1;
}

/* ===== 3. the Tok entry — contract in q_tok.h, stated once ===== */
ray_t* q_tok(int8_t tag, const char* p, size_t len) {
    while (len && *p == ' ') { p++; len--; }
    while (len && p[len - 1] == ' ') len--;
    switch (tag) {
    case RAY_SYM:
        return ray_sym(ray_sym_intern(len ? p : "", len));
    case RAY_BOOL:   /* truthy set pinned by ref/tok.md: "txyTXY1" */
        return ray_bool(len == 1 && strchr("1TtXxYy", p[0]) != NULL);
    case RAY_F64: case RAY_F32: {
        double v = 0;
        size_t used = len ? ray_parse_f64(p, len, &v) : 0;
        if (used != len || len == 0) return ray_typed_null((int8_t)-tag);
        return tag == RAY_F64 ? ray_f64(v) : ray_f32((float)v);
    }
    case RAY_I64: case RAY_I32: case RAY_I16: {
        uint32_t ip;                          /* "I"$ dotted-quad -> IPv4 int */
        if (tag == RAY_I32 && q_tok_ipv4(p, len, &ip)) return ray_i32((int32_t)ip);
        int64_t v = 0;
        size_t used = len ? ray_parse_i64(p, len, &v) : 0;
        if (used != len || len == 0) return ray_typed_null((int8_t)-tag);
        /* Out-of-domain bounds are ±INT*_MAX, NOT INT*_MIN: the exact minimum
         * IS the null sentinel (0N/0Ni/0Nh) and must never round-trip as an
         * accepted value. */
        if (tag == RAY_I64)
            return (v == INT64_MIN)
                 ? ray_typed_null(-RAY_I64) : ray_i64(v);
        if (tag == RAY_I32)
            return (v > INT32_MAX || v < -INT32_MAX)
                 ? ray_typed_null(-RAY_I32) : ray_i32((int32_t)v);
        return (v > INT16_MAX || v < -INT16_MAX)
             ? ray_typed_null(-RAY_I16) : ray_i16((int16_t)v);
    }
    case RAY_DATE: {
        /* invalid civil date included (tok.md pins "D"$"2147483648" -> 0Nd) */
        int64_t y, mo, d;
        if (!q_tok_date(p, len, &y, &mo, &d) || !q_calendar_date_valid(y, mo, d))
            return ray_typed_null(-RAY_DATE);
        return ray_date(q_calendar_days_from_civil(y, mo, d));
    }
    case RAY_MONTH: {
        int64_t mo;
        if (!q_tok_month(p, len, &mo))
            return ray_typed_null(-RAY_MONTH);
        return ray_month(mo);
    }
    case RAY_TIME: {
        int32_t ms;
        if (!q_tok_time(p, len, &ms))
            return ray_typed_null(-RAY_TIME);
        return ray_time(ms);
    }
    case RAY_TIMESTAMP: {
        int64_t ns;
        if (!q_tok_ts(p, len, &ns))
            return ray_typed_null(-RAY_TIMESTAMP);
        return ray_timestamp(ns);
    }
    case RAY_DATETIME: {
        /* tok.md:222-227 pins "PZ"$\: over ONE input: Z shares P's accepted
         * shapes at ms display precision — reuse q_tok_ts (the single P
         * parser) and convert ns -> fractional days. */
        int64_t ns;
        if (!q_tok_ts(p, len, &ns))
            return ray_typed_null(-RAY_DATETIME);
        return ray_datetime((double)ns / 86400000000000.0);
    }
    case RAY_BYTE_ONLY: {
        /* "X"$ reads HEX ("X"$"42" -> 0x42, ref/tok.md).  Unparseable or
         * > 0xff -> 0x00 (derived): byte HAS no null (basics/datatypes.md),
         * so its zero value stands in for the typed null. */
        uint64_t v = 0;
        size_t i = 0;
        for (; i < len; i++) {
            char c = p[i];
            int d = (c >= '0' && c <= '9') ? c - '0'
                  : (c >= 'a' && c <= 'f') ? c - 'a' + 10
                  : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
            if (d < 0) break;
            v = (v << 4) | (uint64_t)d;
            if (v > 0xff) break;
        }
        if (len == 0 || i != len || v > 0xff) return ray_u8(0);
        return ray_u8((uint8_t)v);
    }
    case RAY_GUID: {
        /* Canonical 36-char UUID, else an IPv4/IPv6 address (tok.md #ip-address);
         * base ray_cast_fn "GUID" ERRORS on bad input, so parse here. */
        uint8_t bytes[16];
        if (!q_tok_uuid(p, len, bytes) && !tok_ip_guid(p, len, bytes))
            return ray_typed_null(-RAY_GUID);
        return ray_guid(bytes);
    }
    case RAY_MINUTE: {
        /* FLOOR to the containing minute (ref/tok.md:61 "U"$"12:13:14" ->
         * 12:13; cast.md:168-170 truncation rule). */
        int64_t ns;
        if (!q_tok_clock_ns(p, len, &ns))
            return ray_typed_null(-RAY_MINUTE);
        return ray_minute(ns / 60000000000LL);
    }
    case RAY_SECOND: {
        int64_t ns;
        if (!q_tok_clock_ns(p, len, &ns))
            return ray_typed_null(-RAY_SECOND);
        return ray_second(ns / 1000000000LL);
    }
    case RAY_TIMESPAN: {
        int64_t ns;
        if (!q_tok_timespan_ns(p, len, &ns))
            return ray_typed_null(-RAY_TIMESPAN);
        return ray_timespan(ns);
    }
    default:
        return q_err(QE_NYI);
    }
}
