/* q_pq — the standard library's C floor: the `.pq` autoload, the
 * `.pq.load_natives` root, and the bundle member door behind `\l pq/<file>.q`.
 * Nothing here runs at q_runtime_create except the help-db binding. */
#ifndef PEACHQ_Q_PQ_H
#define PEACHQ_Q_PQ_H

#include <rayforce.h>

/* The FIRST reference to a `.pq` name (q_env's hook) runs `\l pq/pq.q` once —
 * through the one resolver, so a pq/ directory's copy wins over the bundle
 * member.  NULL on a full load or when it already ran (a failed load displayed
 * its error and is not retried); else the abort's owned re-signal (the script
 * seam's law). */
ray_t* q_pq_autoload(void);

/* `\l pq` — reload every library file (`.pq.i.files`); the
 * reference itself autoloads `.pq`.  Returns as q_pq_autoload. */
ray_t* q_pq_load(void);

/* A missing `.<stem>…` read: when `<stem>.q` is a library file (a pq/ directory's,
 * else the bundle member's) the load record has not seen, load `.pq` then run
 * `\l pq/<stem>.q`.
 * NULL when nothing ran; else RAY_NULL_OBJ on a load, or the abort's owned
 * re-signal.  `.pq` itself is q_pq_autoload's. */
ray_t* q_pq_autoload_ns(const char* stem, size_t n);

/* `\l pq/<file>.q` (`lit` as given, a `.q` suffix optional): `path` is the file
 * a pq/ directory had, else the embedded member runs as its own named script; a name in neither is 'path as given.
 * pq.q's first load IS the autoload: `.pq.load_natives` is bound just before
 * it runs, the only `.pq` member its first line can see.  Returns as
 * q_pq_autoload. */
ray_t* q_pq_load_file(const char* lit, size_t alen, const char* path);

/* Bind `.help.i.loaddb` — the on-demand door onto the embedded help-db
 * bundle (the generated builtin one-liners).  Called at runtime create so a bare
 * prompt has it.  FIRST HELP ACCESS is the only trigger (owner 2026-09-03): every
 * reader door in help.q calls it, `\l pq` does NOT, and the load is once per
 * runtime. */
void q_pq_helpdb_register(void);

/* Forget the once-per-runtime flags and the load record (a fresh runtime reloads everything). */
void q_pq_reset(void);

/* The cwd or $QHOME moved: a namespace remembered as having no library file is looked for again. */
void q_pq_path_changed(void);

#endif /* PEACHQ_Q_PQ_H */
