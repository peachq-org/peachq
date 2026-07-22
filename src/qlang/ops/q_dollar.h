/* q_dollar — the single C home for the q `$` verb: one generic entry matching
 * the q public API plus typed specializations for reuse (the q_bang pattern).
 *
 * Reuse mandate (THE q-layer conversion entry points): q-semantics work that
 * needs a conversion MUST call q_dollar_cast with a rayfall type tag rather
 * than growing its own conversion — the only sanctioned exception is a
 * profiled hot path with a specialized SIMD/vectorized kernel (base arith.c's
 * as_i64/as_f64 atom coercions are that carve-out). */
#ifndef Q_DOLLAR_H
#define Q_DOLLAR_H

#include <rayforce.h>
#include <stdint.h>

/* The `$` verb.  Borrowed operands; returns an OWNED value or error. */
ray_t* q_dollar(ray_t* x, ray_t* y);

/* Resolve a q cast/Tok designator value to a rayfall type tag.  Accepts a sym
 * name (`long`float`int`short`boolean`real`symbol...), a single-char string
 * ("j" "f" "i" "h" "b" "e" "s"; upper-case = Tok), or a short atom holding the
 * kdb type number (positive = cast, negative = Tok per ref/tok.md — the
 * rayfall tags already equal the kdb numbers).  The null symbol ` is Tok "S".
 * *is_tok is set to 1 for Tok (string-parse) designators.  Returns 0
 * (RAY_LIST — never a valid target) for unknown or deferred designators. */
int8_t q_cast_designator(ray_t* t, int* is_tok);

/* Convert x to the tag type with q semantics.  Exactly: RAY_LIST distributes
 * per element (collapsed); float ATOMS and float VECTORS pre-round via rint
 * (kdb ties-even) for integer targets; symbol target is identity on syms and
 * 'nyi otherwise; every other input delegates to base ray_cast_fn (typed
 * vectors included).  Returns owned; 'nyi error for deferred targets. */
ray_t* q_dollar_cast(int8_t tag, ray_t* x);

/* Parse string atom(s) to the tag type (kdb Tok, ref/tok.md).  Leading/
 * trailing blanks are trimmed; unparseable or out-of-range parses yield the
 * TYPED NULL (never an error); lists and string vectors distribute (implicit
 * recursion stops at strings, not atoms). */
ray_t* q_dollar_tok(int8_t tag, ray_t* x);

/* q `w$s` pad (ref/pad.md): left-justify s in |w| spaces (w<0 right-justifies),
 * truncating longer strings; atomic through lists/dicts/tables. */
ray_t* q_dollar_pad(int64_t w, ray_t* x);

/* Enumerate `x$y` (sym lhs naming a domain list) — 'nyi: no enum domains. */
ray_t* q_dollar_enum(ray_t* x, ray_t* y);

/* `$` as matrix multiply / dot product: both operands f64 vectors or
 * rectangular lists of f64 vectors (delegates to the `mmu` keyword's home). */
ray_t* q_dollar_mmu(ray_t* x, ray_t* y);

#endif /* Q_DOLLAR_H */
