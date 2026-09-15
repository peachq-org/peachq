/* q_termbox — the .termbox.i.* natives: an xterm-only cell terminal (raw mode
 * nested inside the REPL's, front/back cell buffers, diff-present with
 * 16/256/truecolor, decoded key + SGR-mouse input with a timeout) and its
 * headless door (init on two file paths).  Registered at the `\l pq` gate;
 * lib/termbox.q wraps the public .termbox.* spellings on top. */
#ifndef QLANG_IO_Q_TERMBOX_H
#define QLANG_IO_Q_TERMBOX_H

#include <stddef.h>

void q_termbox_register(void);

/* Write raw bytes through the open session's output stream (the tty, or the
 * headless file); 0 = no session is open and nothing was written. */
int q_termbox_emit(const char* p, size_t n);

#endif
