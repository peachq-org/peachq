/* q_key — the single C home for the q `key` verb (ref/key.md), which is also
 * the monadic `!`.  Three entry points, widest first: q_key is the registry
 * classifier (any operand); q_key_name owns every SYMBOL operand — the
 * namespace roster, root objects, file handles and the name trichotomy;
 * q_key_dir owns the filesystem handles alone.  Callers holding an interned
 * name should call the most specific of the three.
 *
 * `key` is a MULTI-CONCEPT verb in the sense of CLAUDE.md rule 3 — one glyph
 * over unrelated concepts (dictionary keys, key columns of a keyed table,
 * files in a folder, whether a file or name exists, type of a vector, til) —
 * so its classifier receives WHOLE args and each overload owns its own
 * boundary.  Unlike q_bang.c this file is NOT pure value semantics: q's name
 * and namespace overloads are DEFINED over the name environment
 * (ref/key.md:119), so the sym arms read it through q_env.h.  Which symbol
 * marks a namespace dictionary stays q_env's to know (q_env_marker_sym).
 *
 * The foreign-key and enumerator arms (ref/key.md:148) are the enumerations
 * divergence — WON'T DO, see ARCHITECTURE.md. */
#ifndef Q_KEY_H
#define Q_KEY_H

#include <rayforce.h>
#include <stdint.h>

/* The `key` verb.  Vector -> its type name; non-negative int atom -> til;
 * symbol -> q_key_name; dictionary (a keyed table is one) -> its keys;
 * anything else 'type.  Borrowed x; returns an OWNED value or error. */
ray_t* q_key(ray_t* x);

/* Every SYMBOL operand (ref/key.md).  `` ` `` -> the root's namespaces;
 * `` `. `` -> objects in the root; `` `.foo `` -> that namespace's keys (the
 * marker included, as kdb prints it); `` `:path `` -> q_key_dir; otherwise the
 * name trichotomy — a dict's keys, else the sym itself when the name is bound,
 * else `()`.  Returns an OWNED value or error. */
ray_t* q_key_name(int64_t sym);

/* A file handle `` `:path `` (ref/key.md:68,91,102-103): a directory -> its
 * entries as a sorted symbol vector (empty vector when the directory is
 * empty); an existing file -> the descriptor itself; neither -> the empty
 * GENERAL list, which is how `()~key`:foo` tells absent from empty.  `hsym` is
 * the interned handle INCLUDING its leading ':'.  Returns an OWNED value. */
ray_t* q_key_dir(int64_t hsym);

#endif /* Q_KEY_H */
