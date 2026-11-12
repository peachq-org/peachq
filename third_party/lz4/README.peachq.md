# lz4 (vendored)

- **Upstream:** https://github.com/lz4/lz4 — `https://github.com/lz4/lz4/releases/download/v1.9.4/lz4-1.9.4.tar.gz`
- **Version:** `1.9.4` (2022-08-15)
- **Tarball sha256:** `0b0e3aa07c8c063ddf40b082bdf7e37a1562bda40a0ff5272957f3e987e0e54b` (matches the release's own
  `lz4-1.9.4.tar.gz.sha256` asset)
- **License:** BSD 2-Clause (see `LICENSE`) — Copyright (c) 2011-2020, Yann Collet
- **Files vendored, verbatim from `lib/`:** `lz4.c` `lz4.h` `lz4hc.c` `lz4hc.h` `LICENSE`. The block API only: the
  frame format (`lz4frame.*`), `lz4file.*`, `xxhash.*`, the CLI, examples and build systems are not vendored.

## Why it is here

kdb's compression algorithm 4 (lz4hc) stores each kxzipped block as a RAW LZ4 block (no magic, no size prefix; the
decoded size is the logical block size, or the remainder for the last block). `src/qlang/io/q_io.c` decodes those
blocks with `LZ4_decompress_safe` and writes them with `LZ4_compress_HC` at the requested level. kdb loads the host's
`liblz4.so.1` (`kb/file-compression.md` § LZ4); stock lz4 is vendored instead, so the bytes we write are the same on
every platform (Linux, the Windows cross-compile, wasm) and reads need no library on the host.

**Why 1.9.4 and not 1.10+:** re-encoding kdb's six lz4hc blocks (tick_scratch, level 1, block 2^16) at the tail's
level gives 6/6 byte-identical blocks with 1.9.4 (and with the host's 1.9.3) but 0/6 with 1.10.0 and 1.11.1: 1.10.0
replaced HC levels 1-2 with its new "lz4mid" strategy. `test/q_lz4.c` pins this. The fixes listed in 1.10.0-1.11.1
`NEWS` for the library are in the frame, streaming, ring-buffer and `destSize` paths, and in argument checks this
seam never reaches (it passes valid sizes to the two one-shot block calls).

## Build

The Makefile's generic `third_party/%.o` rule builds `lz4.c` and `lz4hc.c` without `-Wextra`/`-Werror`; no defines.
`lz4hc.c` includes `lz4.c` for the shared definitions only (`LZ4_COMMONDEFS_ONLY`). The executable exports only
`tools/kapi.syms`, so these symbols never interpose on a `liblz4.so.1` that a dlopen'd library brings with it.

## Updating

A version that changes level 1-2 output breaks byte identity with kdb's files: run `make test` (`test/q_lz4.c`
re-encodes kdb's blocks and expects kdb's bytes) before anything else. Then fetch the release tarball, verify its
sha256, copy the files above verbatim, and update the pins here.
