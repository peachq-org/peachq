/* q_highlight — the q console's syntax highlighter, installed as the line editor's ray_highlight_fn.
 *
 * LEXICAL: the rendering is a function of the line and the back-lit pair only, never of the running session.
 * Builtin names colour from a build-time table of the documented builtins (lib/help-builtins.tsv minus the
 * unimplemented rows of lib/help-builtins-gaps.tsv); `.q.x` colours as `x`; `.z.*` colours by shape. */
#ifndef Q_HIGHLIGHT_H
#define Q_HIGHLIGHT_H

#include <stdint.h>

int32_t q_highlight(char* dst, int32_t dst_cap, const char* buf, int32_t buf_len, int32_t match_pos1,
                    int32_t match_pos2);

#endif
