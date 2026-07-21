/* q_fmt — format an evaluated q value as the q console would print it.
 *
 * For now this is a thin seam that delegates to rayforce's formatter; it is the
 * single place where q-correct output (`1b`, `42h`, `` `sym ``, typed nulls,
 * …) will be implemented, replacing the fallback case by case. */
#ifndef Q_FMT_H
#define Q_FMT_H

#include <rayforce.h>
#include <stddef.h>

/* Render val into buf (NUL-terminated, bounded by bufsz), UNCLIPPED.  The
 * round-trippable paths (`string`, `-3!`/`.Q.s1`, CSV) and every recursive
 * cell/element render use this. */
void q_fmt(ray_t* val, char* buf, size_t bufsz);

/* Render val for the CONSOLE, honouring the live `\c rows cols` DISPLAY clip
 * when armed (q_sys.c q_con_display): a line longer than cols-1 chars keeps
 * its first cols-3 chars + `..` at columns cols-2,cols-1 (fixed-column,
 * type-blind); a display taller than rows-2 lines shows rows-3 lines + a bare
 * `..` row.  Applied IN-RENDER by q_fmt.c's emitter, so a huge value never
 * renders in full.  The DISPLAY seam — REPL auto-echo, `show`, `.Q.s` route
 * through here; unarmed (or for a parse tree — rule 2) it equals q_fmt. */
void q_fmt_console(ray_t* val, char* buf, size_t bufsz);

/* ---- `\P` display precision -----------------------------------------------
 * Significant digits shown when a float is converted to a string (kdb `\P`,
 * default 7, range [0,17]; 0 = maximum = 17).  q_sys.c's `\P` handler is the
 * sole writer; q_fmt's float renderer is the sole reader.  q_runtime_create
 * resets it to the default per runtime. */
void q_fmt_set_prec(int p);
int  q_fmt_prec(void);

/* THE q float->text leaf (`\P`-honouring; NaN -> 0n/0Ne; wholes within the \P
 * horizon print integral, past it exponent-form).  f32=1 renders DISPLAY reals
 * (`e` suffix, 0Ne null); q_string_fn passes f32=0 — string "results contain
 * none of the special notation that distinguishes types" (q1.txt:620) — and 0:
 * Prepare Text inherits through it.  RULE: anything composing text above
 * string/0: must inherit, never re-format floats itself.  out (non-null,
 * n>0) is always NUL-terminated; over-long magnitudes are truncated. */
void q_float_tok(double v, int f32, char* out, size_t n);

/* ---- q console sink -------------------------------------------------------
 * `show` (and, in principle, `0N!`) print a value's q display as a SIDE EFFECT
 * and return generic null.  Since qdoc compares only the row's rendered output
 * and the REPL prints per line, the side-effect text is buffered here: the
 * host (qdoc / repl) drains it before/instead of the result and resets it once
 * per example / line. */
void        q_console_show(ray_t* val);   /* append q_fmt(val) + '\n' */
const char* q_console_str(void);          /* buffered text ("" if empty) */
void        q_console_reset(void);        /* clear the buffer */

/* Single-line k-repr (kdb `0N!x` / .Q.s1 style): generic lists inline as
 * (a;b;c), enlist as ,x, strings quoted-escaped (a len-1 string renders
 * `,"c"` — the string-model conflation rule); vectors/atoms via q_fmt. */
void q_fmt_krepr(ray_t* val, char* buf, size_t bufsz);
void q_console_show_krepr(ray_t* val);    /* append q_fmt_krepr(val) + '\n' */
void q_console_write(const char* s, size_t n);  /* raw bytes (kdb 1/-1 handles) */

#endif /* Q_FMT_H */
