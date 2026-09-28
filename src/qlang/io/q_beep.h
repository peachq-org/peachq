/* q_beep — the .termbox.i.beep native: a blocking tone (hz, ms) — ALSA on
 * Linux, Beep() on Windows, CoreAudio on macOS — with the terminal bell as the
 * fallback wherever no audio path opens.  Bound by `.pq.load_natives` beside
 * the termbox natives; `.termbox` wraps .termbox.beep on top. */
#ifndef QLANG_IO_Q_BEEP_H
#define QLANG_IO_Q_BEEP_H

void q_beep_register(void);

#endif
