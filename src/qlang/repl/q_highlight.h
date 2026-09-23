/* q_highlight — the q console's syntax highlighter, installed as the line editor's ray_highlight_fn.  A byte's role
 * is lexical, a function of the line and the back-lit pair only; a role's colour is the C default unless a `.pq.hl`
 * dictionary overrides it, and nothing is coloured while q_console_color says colour is off. */
#ifndef Q_HIGHLIGHT_H
#define Q_HIGHLIGHT_H

#include <stdint.h>

int32_t q_highlight(char* dst, int32_t dst_cap, const char* buf, int32_t buf_len, int32_t match_pos1,
                    int32_t match_pos2);

#endif
