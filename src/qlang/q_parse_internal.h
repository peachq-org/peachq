/* q_parse_internal.h — INTERNAL shared surface for the q_parse.c split (2026-07-14).
 * These symbols were file-local statics before the file split; they are
 * exported ONLY so the sibling .c files of the split can keep calling them.
 * NOT public API — do not include outside src/qlang. */
#ifndef Q_PARSE_INTERNAL_H
#define Q_PARSE_INTERNAL_H
#include "qlang/q_parse.h"
#include "qlang/q_registry.h"
#include <setjmp.h>

/* ATTR_QUOTED: flag on a -RAY_SYM atom.  SET = literal symbol; CLEAR
 * (default) = name reference, resolved at eval.  Mirrors the definition in
 * src/lang/eval.h (0x20) — kept local so the parser needs no eval header. */
#define Q_ATTR_QUOTED 0x20

/* Q_ATTR_HOLE: flag on the `::` sym of an ELIDED bracket-call slot (`f[a;;b]`)
 * — a projection hole, as distinct from an explicit `::` (a real generic-null
 * VALUE, e.g. the whole-value amend index `@[v;::;f]` or a trap fx).  The two
 * spell identically (`::`), so this flag is the only signal that lets the
 * `@`/`.` lowering tell an elision (project) from an explicit `::` (amend/trap
 * data).  0x40 is unused on a -RAY_SYM atom (it is RAY_FN_COMPILED/Q_LOWER on
 * fn/lambda values only), and the marked node never survives lowering. */
#define Q_ATTR_HOLE   0x40

#define MAX_VEC  4096
#define MAX_NAME 256

/* ---- defined in q_parse.c ---- */
ray_t *q_verb(char c);
ray_t *q_verb_name(const char *s, int len);
ray_t *q_marker(const char *s);
ray_t *q_embed(ray_t *sym, q_valence_t val);
ray_t *q_null(void);
ray_t *q_symvec_append(ray_t *vec, const char *s, int len);
extern const char VERB_CHARS[];
extern const char *ADVERB_NAMES[];
int q_symvec_contains_id(ray_t *v, int64_t id);

#endif
