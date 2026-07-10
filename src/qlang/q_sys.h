/* q_sys — the unified q `\`-command dispatcher.  A single Q_SYS[] manifest
 * (mirroring q_dotz.c's static-table idiom) maps each `\`-command token to its
 * handler + flags.  Stage 2 enumerates EVERY kdb `\`-command as a row:
 *   - the five working commands (\d \v \f \a \S) keep their exact behaviour;
 *   - config getter/setter commands (\P \c \o \z) accept their SETTER form as a
 *     silent no-op (kdb-true empty output) and return 'nyi for the GETTER form;
 *   - every other known-but-unimplemented command returns 'nyi;
 *   - an unknown `\`-token is CLASSIFIED (QS_UNKNOWN) and handed back to the
 *     caller — the core never shells out itself;
 *   - \\ (quit) and bare \ (terminate / toggle) are CLASSIFIED (QS_QUIT /
 *     QS_TOGGLE) — the core never exit()s the process itself.
 * The core is MODE-LESS: every entry-point-specific choice (exit? shell out?
 * how?) lives in the per-form adapter that calls it (q_repl.c / qdoc.c /
 * q_system_fn), so the test runner never diverges the shared binary's path.
 * Frozen-clean: no src/lang or src/ops edits. */
#ifndef Q_SYS_H
#define Q_SYS_H

#include <rayforce.h>
#include <stddef.h>

/* Re-initialize the rng to kdb's constant startup seed (-314159i,
 * basics/syscmds.md \S) and record it as the last-initialized seed.  Called by
 * q_runtime_create; `\S n` re-initializes thereafter. */
void q_sys_seed_init(void);

/* Reset the Tier-2 `\`-command config (\c \C \g \o \W \e \s) AND the `\P`
 * display precision to their kdb defaults.  Called by q_runtime_create so a
 * config change in one .qcmd file (fresh-per-file runtime) never leaks into
 * the next. */
void q_sys_cfg_init(void);

/* Bind a kdb-protocol IPC listener on the runtime event poll and record it as
 * the live `\p` listener — the SINGLE HOME for both `\p N`/`\p 0W` (q_sys.c
 * h_p) and startup `-p`/`-p 0W` (qmain.c).  `port == 0` → OS-chosen ephemeral
 * (the `0W` path).  On success: reads the ACTUAL bound port back via
 * getsockname, drops any previous listener (kdb listens on ONE port), sets the
 * authoritative `\p` getter state (`g_listen_port`, so `system "p"` reports the
 * real port after EITHER path) and marks the listener active, then returns the
 * bound port (>0).  On any failure (no poll, bind/listen, or readback) returns
 * 0 and leaves the previous listener intact — never advertises port 0.  Silent
 * (prints nothing — full kdb port fidelity). */
uint16_t q_sys_listen(uint16_t port);

/* The AUTHORITATIVE live listening port (`\p` getter state): the bound port
 * (>0) while a listener is up, 0 after `\p 0` / when never bound.  qmain's
 * post-script server-mode decision keys off THIS (not a stale local `port`),
 * so a startup-script `\p 0` that closes the `-p` listener no longer strands
 * the process in a listener-less server loop. */
uint16_t q_sys_listen_port(void);

/* What the mode-less core made of a `\`-command line.  The core CLASSIFIES and
 * hands policy back to the caller — it never exit()s or shells out itself.  Each
 * per-form adapter acts on the kind: the REPL exits on QS_QUIT and shells out on
 * QS_UNKNOWN (raw system(3) status); `system "…"` (q_system_fn) captures stdout
 * on QS_UNKNOWN; the doctest runner (qdoc.c) survives QS_QUIT/QS_TOGGLE and
 * DECLINES to execute QS_UNKNOWN (leaving it to the parser — keeps \ls/\curl
 * corpus rows from touching the filesystem / network).  Nothing test-specific
 * lives in the shared core. */
typedef enum {
    QS_NOT_CMD = 0,  /* line does not begin with `\` — the caller should parse it */
    QS_VALUE,        /* handled: .val is an OWNED value/error to display (NULL = silent) */
    QS_QUIT,         /* `\\` — the caller decides whether to exit() */
    QS_TOGGLE,       /* bare `\` — q/k toggle; the caller decides (REPL → 'nyi) */
    QS_UNKNOWN,      /* unknown `\token` — .shell/.shell_len is the command slice */
} q_sys_kind;

typedef struct {
    q_sys_kind  kind;
    ray_t*      val;        /* QS_VALUE only: OWNED value/error, or NULL when silent */
    const char* shell;      /* QS_UNKNOWN only: slice INTO the input line (not owned) */
    size_t      shell_len;
} q_sys_result;

/* Classify a console line that may begin with `\`.  Returns a q_sys_result; the
 * core takes NO process-level action (never exit()s, never shells out). */
q_sys_result q_sys_dispatch(const char* line, size_t n);

/* Shell escape for the interactive REPL `\`-form: runs `rem` via system(3) and
 * returns its RAW status as a long (kdb-true `\foo`).  Exposed so the REPL
 * adapter invokes it on QS_UNKNOWN; `system "…"` uses its own stdout-capturing
 * path.  `rem`/`rlen` is a slice; it is copied NUL-terminated internally. */
ray_t* q_sys_shell(const char* rem, size_t rlen);

/* The q-owned `system "…"` verb (bound by q_builtins_register as a QK_ENV row).
 * The STRING-form adapter: normalizes the string (conceptually prepends `\`),
 * runs the mode-less core, and on QS_UNKNOWN captures stdout as a list of char
 * vectors — single-homing the command logic with the `\`-slash form. */
ray_t* q_system_fn(ray_t* x);

#endif /* Q_SYS_H */
