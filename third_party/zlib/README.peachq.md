# zlib (vendored)

- **Upstream:** https://zlib.net — `https://zlib.net/fossils/zlib-1.3.1.tar.gz`
- **Version:** `1.3.1` (2024-01-22)
- **Tarball sha256:** `9a93b2b7dfdac77ceba5a558a580e74667dd6fede4585b91eefb60f03b72df23`
- **License:** zlib licence (see `LICENSE`) — Copyright (C) 1995-2024 Jean-loup Gailly and Mark Adler
- **Files vendored, verbatim:** `adler32.c` `crc32.c` `crc32.h` `deflate.c` `deflate.h` `gzguts.h` `inffast.c`
  `inffast.h` `inffixed.h` `inflate.c` `inflate.h` `inftrees.c` `inftrees.h` `trees.c` `trees.h` `zutil.c` `zutil.h`
  `zlib.h` `zconf.h` `LICENSE` `README`. The deflate/inflate/checksum core only: the `gz*` file API, `compress.c`,
  `uncompr.c`, `infback.c`, `contrib/`, examples and build systems are not vendored. `gzguts.h` is here only
  because `zutil.c` includes it.

## Why it is here

`src/qlang/net/q_gz.c` is peachq's one DEFLATE seam: `.Q.gz`, HTTP gzip, and the kxzipped (algorithm 2) block
codec. kdb writes kxzipped blocks through the host's `libz.so.1`, and stock zlib reproduces them byte-for-byte
(`actionable-plans/2026-11-09-kdb-format-findings.md` § Q2: 21,155 of 21,155 blocks over 7 writers at levels 5/6/7,
identical at 1.2.11 and 1.3.1). It is vendored, not loaded from the host, so the bytes we write are the same on
every platform (Linux, the Windows cross-compile, wasm). 1.3.1 rather than 1.2.11: CVE-2018-25032 is in 1.2.11's
deflate. It replaced miniz, whose bitstream is valid DEFLATE but matched 0 of 177 kdb blocks.

## Build

The Makefile's generic `third_party/%.o` rule builds the eight `.c` files without `-Wextra`/`-Werror`; no defines.
The executable exports only `tools/kapi.syms`, so these symbols never interpose on a `libz.so.1` that a dlopen'd
library (OpenSSL, DuckDB, Kafka) brings with it.

## Updating

Fetch the new release tarball, verify its sha256, copy the files above verbatim and update the pins here. Then run
`make test` (`test/q_gz.c` re-deflates a kdb-written block and expects kdb's bytes) and the kxzip/gzip suites.
