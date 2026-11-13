/* q_log — `-11!` streaming execute (basics/internal.md § -11!, kb/logging.md
 * § Replaying log files): a bounded-window cursor over a plain tickerplant
 * log, every chunk through the handle-0 console door.  `-l`/`-L` automatic
 * logging, fifo replay and compressed logs (kb/file-compression.md:39) are
 * not here. */
#ifndef QLANG_Q_LOG_H
#define QLANG_Q_LOG_H
#include <rayforce.h>

/* x borrowed.  Owned: the long chunk count; for (-2;x) on a log that ends
 * inside a chunk, the long pair (chunks;validLength) instead of 'badtail. */
ray_t* q_log_replay(ray_t* y);

/* `get` of a general-list file whose items run past its header count (a log): every item to the end of the image
 * already in hand, decoded by the same walker as a replay.  Owned list, or an error — 'corrupt for a torn tail. */
ray_t* q_log_records(const uint8_t* img, size_t len);

#endif
