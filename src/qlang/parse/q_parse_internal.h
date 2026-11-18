/* q_parse_internal.h — INTERNAL shared surface for the q_parse.c split (2026-07-14).
 * These symbols were file-local statics before the file split; they are
 * exported ONLY so the sibling .c files of the split can keep calling them.
 * NOT public API — do not include outside src/qlang. */
#ifndef Q_PARSE_INTERNAL_H
#define Q_PARSE_INTERNAL_H
#include "qlang/parse/q_parse.h"

/* ATTR_QUOTED: flag on a -RAY_SYM atom.  SET = literal symbol; CLEAR
 * (default) = name reference, resolved at eval.  Mirrors the definition in
 * src/lang/eval.h (0x20) — kept local so the parser needs no eval header. */
#define Q_ATTR_QUOTED 0x20

/* Q_ATTR_POSTFIX: flag on the call node of a postfix elision (`2+`, `-15!`, `count@`): its trailing hole was never
 * WRITTEN, so the walker applies the head to the bound operand alone and the projection is `+[2]`'s (qPython's md5
 * fixture is two items) — parse still shows kdb's (+;2;::).  On a `g::` train tail (::;g) it makes a function g the
 * composition '[::;g].  0x04 is unclaimed on a general list. */
#define Q_ATTR_POSTFIX 0x04

/* A leading '_' is the drop/cut verb: no name, and no first item of a symbol run, begins with it (basics/syntax.md:601). */
static inline int q_parse_drop_lead(char c) { return c == '_'; }

#define MAX_NAME 256

typedef enum {
    T_EOF,
    T_NOUN,
    T_VERB,
    T_ADVERB,
    T_LPAREN, T_RPAREN,
    T_LBRACE, T_RBRACE,
    T_LBRACK, T_RBRACK,
    T_SEMI,
    T_COMMENT   /* q_parse_scan's alone: scan() skips comments */
} TKind;

/* ---- defined in q_parse.c ---- */
extern const char *ADVERB_NAMES[];

/* The scanner's tokens and the comments between them as spans, without their values, for the syntax tree
 * (q_syntax.c); an infix keyword is a T_VERB, as the parser reads it.  -1 for a cold registry or no memory, else 0
 * with tok to free, and when a scan error stopped it, err (owned) at err_at, err_len bytes or -1 (an unclosed string
 * or literal runs to the end). */
enum { Q_NOUN_NONE, Q_NOUN_NAME, Q_NOUN_NUM, Q_NOUN_STR, Q_NOUN_SYM };
typedef struct { TKind kind; int noun, start, len; } q_scan_tok_t;
typedef struct { q_scan_tok_t *tok; int n; ray_t *err; int err_at, err_len; } q_scan_t;
int q_parse_scan(const char *src, q_scan_t *s);
const char *q_parse_die_msg(void);   /* why the last parse or scan died a bare 'parse, else NULL */
int q_parse_lacked_memory(ray_t *err);   /* whether err, the last refusal, was no memory and no verdict on the text */

#endif
