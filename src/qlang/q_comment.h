/* q_comment — WHICH comment run documents WHICH definition, handed to q as
 * NAMED positional arguments through two hooks: `.help.register_file[file;ns;
 * header]` and `.help.register_definition[fullname;ns;file;line;header]`.
 * peachq owns no doc store in C (owner ruling): `.help` is pure q in
 * lib/help.q and this file only feeds it.
 *
 * The CONTRACT a driver owes: bracket each script, classify every physical
 * line, and end each statement — one comment run documents at most one name.
 *
 * register_file means "begin this file" and is UPSERT-ONLY on the q side (the
 * store mirrors the session — `\l` never removes): called at EVERY script
 * begin with an empty header (a pure no-op there), then again with the real
 * header if the leading run resolves to one.  INVARIANT: the leading comment
 * run always resolves BEFORE the first definition registers — on a break
 * (blank line -> file header) or on code (-> it documents that definition,
 * and no second register_file call happens). */
#ifndef PEACHQ_Q_COMMENT_H
#define PEACHQ_Q_COMMENT_H

#include <rayforce.h>
#include <stddef.h>

/* One script's capture state; a nested load saves and restores it. */
typedef struct {
    int64_t file, stmt_line;
    int seen_code, file_run_done;
} q_comment_script_t;
q_comment_script_t q_comment_script_begin(int64_t file_sym);
void           q_comment_script_end(q_comment_script_t saved);

/* The line classification, one call per physical line: extend the run, END it
 * (blank line, block comment, continuation — which records the file's LEADING
 * run and drops any other), or arm it for the fresh logical line at `line` —
 * a leading run ending on CODE arms for that definition, never the file. */
void q_comment_line(const char* p, size_t n);
void q_comment_break(void);
void q_comment_fresh_line(int64_t line);

/* The statement's own binding claims the armed header (the global-set home
 * calls this) — which is how a NON-lambda, a `\d`-qualified name and a stale
 * rebind are all one case.  The SAME seam is kdb's "first global assignment"
 * (ref/value.md `n`), so a lambda val is stamped with n/f/l here — captured
 * once, from the file+line this module already holds. */
void q_comment_on_global_set(int64_t sym, ray_t* val);

/* End of statement: hand any claimed record to its hook (unbound = no-op).
 * MUST be called between statements, never mid-eval, and before the console
 * drain so the hook's own output leaves with the statement. */
void q_comment_stmt_end(void);

#endif /* PEACHQ_Q_COMMENT_H */
