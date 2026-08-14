/*
 *   Copyright (c) 2025-2026 Anton Kundenko <singaraiona@gmail.com>
 *   All rights reserved.

 *   Permission is hereby granted, free of charge, to any person obtaining a copy
 *   of this software and associated documentation files (the "Software"), to deal
 *   in the Software without restriction, including without limitation the rights
 *   to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 *   copies of the Software, and to permit persons to whom the Software is
 *   furnished to do so, subject to the following conditions:

 *   The above copyright notice and this permission notice shall be included in all
 *   copies or substantial portions of the Software.

 *   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *   AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *   LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 *   OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 *   SOFTWARE.
 */

#ifndef RAY_TERM_H
#define RAY_TERM_H

#include <rayforce.h>

#if defined(RAY_OS_WINDOWS)
#include <windows.h>
#define KEYCODE_RETURN '\r'
#else
#include <termios.h>
#define KEYCODE_RETURN '\n'
#endif

#define KEYCODE_BACKSPACE '\b'
#define KEYCODE_DELETE    0x7f
#define KEYCODE_TAB       '\t'
#define KEYCODE_UP        'A'
#define KEYCODE_DOWN      'B'
#define KEYCODE_LEFT      'D'
#define KEYCODE_RIGHT     'C'
#define KEYCODE_HOME      'H'
#define KEYCODE_END       'F'
#define KEYCODE_ESCAPE    0x1b
#define KEYCODE_CTRL_A    0x01
#define KEYCODE_CTRL_B    0x02
#define KEYCODE_CTRL_C    0x03
#define KEYCODE_CTRL_D    0x04
#define KEYCODE_CTRL_E    0x05
#define KEYCODE_CTRL_F    0x06
#define KEYCODE_CTRL_K    0x0b
#define KEYCODE_CTRL_L    0x0c
#define KEYCODE_CTRL_N    0x0e
#define KEYCODE_CTRL_P    0x10
#define KEYCODE_CTRL_R    0x12
#define KEYCODE_CTRL_U    0x15
#define KEYCODE_CTRL_W    0x17

#define TERM_BUF_SIZE 4096
#define HIST_DEFAULT_CAP 256

/* Pluggable syntax-highlighter hook.  Same signature as term.c's built-in
 * highlighter: render `buf`/`buf_len` into `dst` (never writing past
 * `dst_cap`), optionally back-lighting the matched bracket pair at
 * match_pos1 / match_pos2 (-1 when none), and return the number of bytes
 * written.  Lets callers (e.g. the q REPL) inject a language-correct
 * tokenizer while the default (NULL) keeps rayforce's built-in behaviour. */
typedef int32_t (*ray_highlight_fn)(char* dst, int32_t dst_cap,
                                    const char* buf, int32_t buf_len,
                                    int32_t match_pos1, int32_t match_pos2);

/* Optional pluggable multi-line continuation policy.  Given the accumulated
 * multiline buffer and the current line, return the count of still-open
 * brackets (>0 keeps reading a continuation line, 0 submits).  NULL → the
 * built-in counter, whose `;`-as-line-comment rule is correct for rayfall but
 * WRONG for q (where `;` is a separator, so `(1 2 3;4 5)` false-continued).
 * The q REPL installs its own (kdb is line-at-a-time → no continuation). */
typedef int32_t (*ray_continuation_fn)(const char* mbuf, int32_t mbuf_len,
                                       const char* buf, int32_t buf_len);

/* The injectable console transport.  Every byte the engine emits goes
 * through `write`; `flush` marks the points where the engine used to
 * fflush(stdout) (ordering vs the front end's stdio stream); `get_size`
 * is polled at each redraw.  Any member may be NULL: write falls back to
 * fd 1, flush to fflush(stdout), get_size to keeping the current size
 * (push it with ray_term_set_size instead). */
typedef struct ray_term_io {
    void (*write)(void* ctx, const void* buf, size_t len);
    void (*flush)(void* ctx);
    int  (*get_size)(void* ctx, int32_t* width, int32_t* height);
    void* ctx;
} ray_term_io_t;

typedef struct ray_hist {
    char**   entries;
    int32_t  count;
    int32_t  capacity;
    int32_t  index;
    int32_t  curr_saved;
    char     curr[TERM_BUF_SIZE];
    int32_t  curr_len;
} ray_hist_t;

typedef struct ray_term {
    ray_t*    _block;
#if defined(RAY_OS_WINDOWS)
    HANDLE   h_stdin;
    HANDLE   h_stdout;
    DWORD    old_stdin_mode;
    DWORD    old_stdout_mode;
#else
    struct termios oldattr;
    struct termios newattr;
#endif
    int32_t  input_len;
    char     input[8];
    int32_t  buf_len;
    int32_t  buf_pos;
    char     buf[TERM_BUF_SIZE];
    int32_t  term_width;
    int32_t  term_height;
    int32_t  prompt_len;
    /* Optional prefix shown before the standard `‣` prompt (e.g. the
     * remote host:port when in remote-REPL mode).  Empty by default.
     * Set via ray_term_set_prompt_prefix(); both the byte string and
     * its rendered visual width must be in sync — visual is what the
     * line editor uses for cursor math. */
    char     prompt_prefix[80];
    int32_t  prompt_prefix_len;
    int32_t  prompt_prefix_vis;
    /* Optional full-prompt override (openq): when set, ray_term_prompt draws
     * exactly this ANSI string in place of the prefix + built-in `‣` glyph, so
     * the q REPL can show a bare `q)`.  Empty by default → glyph behaviour
     * unchanged.  vis is the rendered column width (for cursor math). */
    char     prompt_override[64];
    int32_t  prompt_override_len;
    int32_t  prompt_override_vis;
    int32_t  last_total_rows;
    int32_t  last_cursor_row;
    ray_hist_t hist;
    int32_t  search_mode;
    char     search_buf[256];
    int32_t  search_len;
    int32_t  search_match_idx;
    /* Ghost text (inline completion suggestion) */
    char     ghost[TERM_BUF_SIZE];
    int32_t  ghost_len;
    int32_t  ghost_word_start; /* position in buf where the completed word starts */
    int32_t  ghost_word_len;   /* length of the prefix that was matched */
    /* Host-armed hint (autosuggest at an EMPTY fresh prompt): full display
     * text + how many leading bytes insert on accept (the command, never the
     * `/ comment` tail).  Rendered through the ghost slot, so typing hides
     * it; Tab / → accepts. */
    char     hint[512];
    int32_t  hint_len;
    int32_t  hint_accept;
    /* Multi-source completion candidates */
    const char* comp_items[256];  /* borrowed pointers — valid until next collect */
    int32_t     comp_count;
    /* Tab-cycle completion state */
    int32_t     comp_cycling;     /* 1 if currently cycling completions */
    int32_t     comp_cycle_idx;   /* index into comp_items for current cycle */
    int32_t     comp_cycle_start; /* buf position where cycled word starts */
    int32_t     comp_cycle_len;   /* length of currently inserted completion */
    /* Scratch buffer for null-terminated completion word copies */
    char        comp_scratch[TERM_BUF_SIZE];
    int32_t     comp_scratch_len;
    /* Multi-line input state */
    char        multiline_buf[TERM_BUF_SIZE];
    int32_t     multiline_len;
    /* Escape sequence state machine (for event-driven feed) */
    int32_t     esc_state;     /* 0=normal, 1=ESC, 2=ESC[, 3=ESCO, 4=ESC[3, 5=unknown CSI */
    int32_t     esc_buf_len;   /* bytes accumulated in unknown CSI sequence */
    /* Optional pluggable syntax highlighter; NULL → use the built-in one. */
    ray_highlight_fn highlight_fn;
    /* Optional pluggable continuation policy; NULL → built-in counter. */
    ray_continuation_fn continuation_fn;
    /* The console transport (see ray_term_io_t); zeroed = OS defaults. */
    ray_term_io_t io;
    /* 1 when ray_term_create set up the OS console (raw mode / console
     * modes, history file): destroy and the fatal-signal path restore and
     * save.  0 for ray_term_create_io terms. */
    int32_t owns_console;
} ray_term_t;

/* The OS-console door: raw tty mode + history file + ioctl size polling.
 * The engine itself never touches the OS — ray_term_create_io binds it to
 * a caller-supplied transport instead (no tty, no signals, no history
 * file; feed input by writing term->input[0] + ray_term_feed, push size
 * with ray_term_set_size). */
ray_term_t* ray_term_create(void);
ray_term_t* ray_term_create_io(const ray_term_io_t* io);
void       ray_term_destroy(ray_term_t* term);
int64_t    ray_term_getc(ray_term_t* term);
void       ray_term_set_size(ray_term_t* term, int32_t width, int32_t height);

int32_t ray_term_visual_width(const char* str, int32_t len);
void    ray_term_goto_position(ray_term_t* term, int32_t from_pos, int32_t to_pos);

void   ray_term_redraw(ray_term_t* term);
void   ray_term_prompt(ray_term_t* term);

/* Install a pluggable syntax highlighter (see ray_highlight_fn).  Pass NULL
 * to restore the built-in highlighter.  Callers own the function; it must
 * outlive the term. */
void   ray_term_set_highlighter(ray_term_t* term, ray_highlight_fn fn);

/* Arm (or with NULL/"" clear) the empty-prompt hint; accept_len = leading
 * bytes Tab/→ insert into the buffer. */
void   ray_term_set_hint(ray_term_t* term, const char* text, int32_t accept_len);

/* Install a pluggable multi-line continuation policy (see ray_continuation_fn).
 * Pass NULL to restore the built-in bracket counter. */
void   ray_term_set_continuation_fn(ray_term_t* term, ray_continuation_fn fn);

/* Set (or clear, when prefix == NULL or empty) the prompt prefix.
 * Used by the remote-REPL session to put the server address ahead
 * of `‣` so the user can never mistake it for a local prompt.  The
 * prefix bytes are copied into the term struct; caller may free
 * after the call. */
void   ray_term_set_prompt_prefix(ray_term_t* term, const char* prefix);

/* Override the entire main prompt with a caller-supplied ANSI string of the
 * given visual width; pass NULL/"" to restore the default `‣` prompt. */
void   ray_term_set_prompt(ray_term_t* term, const char* ansi, int32_t vis_width);

/* Event-driven terminal API — split the former blocking line reader into begin + feed.
 * ray_term_begin: show prompt, reset line state.
 * ray_term_feed:  process one byte from term->input[0].
 *   Returns ray_t* string when a complete line is ready,
 *   NULL when more input is needed,
 *   RAY_TERM_EOF on EOF/Ctrl-D at empty buffer. */
#define RAY_TERM_EOF ((ray_t*)(uintptr_t)1)
void    ray_term_begin(ray_term_t* term);
ray_t*  ray_term_feed(ray_term_t* term);

void    ray_hist_create(ray_hist_t* hist);
void    ray_hist_destroy(ray_hist_t* hist);
void    ray_hist_add(ray_hist_t* hist, const char* buf, int32_t len);
int32_t ray_hist_prev(ray_hist_t* hist, char* buf, int32_t buf_len);
int32_t ray_hist_next(ray_hist_t* hist, char* buf);
void    ray_hist_load(ray_hist_t* hist, const char* path);
void    ray_hist_save(ray_hist_t* hist, const char* path);
int32_t ray_hist_search(ray_hist_t* hist, const char* needle, int32_t needle_len,
                       int32_t start_idx);

int32_t ray_term_find_matching_paren(const char* buf, int32_t buf_len,
                                    int32_t cursor_pos);

/* Collect completion candidates from all sources (env, keywords, columns,
 * history words).  Stores results in term->comp_items / comp_count.
 * prefix/prefix_len is the word being completed. */
void ray_term_collect_completions(ray_term_t* term, const char* prefix,
                                 int32_t prefix_len);


/* Multi-line input: count unmatched opening brackets in multiline_buf + buf */
int32_t ray_term_count_unmatched(ray_term_t* term);
void    ray_term_continuation_prompt(ray_term_t* term);

/* Signal handling — install handlers to restore terminal on exit */
void ray_term_install_signals(ray_term_t* term);

/* Global interrupt flag — set by the SIGINT handler (or any adapter via
 * ray_term_request_interrupt), checked by the eval loop */
int  ray_term_interrupted(void);
void ray_term_clear_interrupt(void);
void ray_term_request_interrupt(void);

/* Temporarily enable ISIG so Ctrl-C generates SIGINT during eval.
 * Call ray_term_eval_end() to restore raw mode after eval returns. */
void ray_term_eval_begin(ray_term_t* term);
void ray_term_eval_end(ray_term_t* term);

#define HIST_MAX_ENTRIES 1000
#define HIST_DEFAULT_PATH ".rayforce_history"

#endif /* RAY_TERM_H */
