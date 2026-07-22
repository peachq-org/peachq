/* q_tok — THE single string->value scanner home (temporal + guid).  Both
 * text entrances share it: the code parser's literal magnitudes
 * (q_tok_temporal, called from q_parse.c's scan_one_num) and `$` Tok's
 * whole-string parses (q_dollar_tok).  The two grammars stay DISTINCT — Tok
 * accepts separator/packed forms the literal syntax refuses — but the code
 * lives in one file over one calendar (q_calendar.c) so a format never grows
 * a second copy. */
#ifndef Q_TOK_H
#define Q_TOK_H

#include <stddef.h>
#include <stdint.h>

/* One scanned literal magnitude (the parser's element).  Q_TOK_EL_MONTH carries
 * BOTH the month payload (.i) and its float twin (.f): bare `2000.01` is the
 * float, only a trailing `m` reads the payload. */
typedef enum { Q_TOK_EL_INT, Q_TOK_EL_FLOAT, Q_TOK_EL_NULL, Q_TOK_EL_PINF, Q_TOK_EL_NINF, Q_TOK_EL_DATE, Q_TOK_EL_TIME,
               Q_TOK_EL_TS, Q_TOK_EL_MONTH, Q_TOK_EL_MINUTE, Q_TOK_EL_SECOND, Q_TOK_EL_TIMESPAN,
               Q_TOK_EL_DT /* datetime: f64 days in .f */ } q_tok_el_kind;
typedef struct { q_tok_el_kind kind; int64_t i; double f; int forces_float; } q_tok_el;

/* Scan one TEMPORAL literal magnitude at src[*p] (date / timestamp / datetime
 * / month-shaped / time / timespan / second / minute).  Returns 1 and
 * advances *p on a match, 0 on no match (caller falls through to the numeric
 * scan), -1 on a malformed shape with *err set to the parse-error text (the
 * caller dies — invalid civil dates/clocks never fall back to floats). */
int q_tok_temporal(const char* src, int* p, q_tok_el* out, const char** err);

/* Tok string scanners (ref/tok.md), shared by "D"$/"M"$/"T"$/"U"$/"V"$/"N"$/
 * "P"$/"Z"$/"G"$: each returns 1 and fills its payload on a shape match, 0
 * otherwise (the Tok caller yields the typed null — never an error).
 * Grammars and derivations are documented at each definition. */
int q_tok_date(const char* p, size_t len, int64_t* y, int64_t* m, int64_t* d);
int q_tok_month(const char* p, size_t len, int64_t* months);   /* months since 2000.01 */
int q_tok_time(const char* p, size_t len, int32_t* ms);
int q_tok_clock_ns(const char* p, size_t len, int64_t* ns);
int q_tok_timespan_ns(const char* p, size_t len, int64_t* ns);
int q_tok_ts(const char* p, size_t len, int64_t* out);
int q_tok_uuid(const char* p, size_t len, uint8_t out[16]);

#endif /* Q_TOK_H */
