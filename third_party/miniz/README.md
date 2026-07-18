# miniz (vendored)

- **Upstream:** https://github.com/richgel999/miniz
- **Version:** release `3.0.2` (2023; amalgamated single-file build, reports `MZ_VERSION "11.0.2"`)
- **License:** MIT (see `LICENSE`) — Copyright 2013-2014 RAD Game Tools and Valve Software; Copyright 2010-2014 Rich Geldreich and Tenacious Software LLC
- **Files vendored:** `miniz.c`, `miniz.h`, `LICENSE`

## Why it is here

openq's `.Q.gz` (ref/dotq.md — GZip) needs DEFLATE compression/decompression. miniz is the
best-of-breed minimal, zero-dependency DEFLATE implementation. openq uses only its raw
deflate/inflate primitives (`tdefl_compress_mem_to_heap`, `tinfl_decompress`) and `mz_crc32`.

The **gzip (RFC 1952) framing is hand-rolled** in `src/qlang/q_gz.c` — miniz's zlib-compat
layer does not support zlib's `windowBits+16` gzip mode, so openq writes the 10-byte header,
CRC-32, and ISIZE footer itself around miniz's raw-deflate stream. That `q_gz_deflate` /
`q_gz_inflate` seam is also where a future dlopen-system-zlib preference would swap in.

## Lean build

Compiled with `-DMINIZ_NO_ARCHIVE_APIS -DMINIZ_NO_ARCHIVE_WRITING_APIS -DMINIZ_NO_STDIO`
(set in the `Makefile` per-file rule, NOT by editing the vendored source): the ZIP archive
reader/writer and all stdio file I/O are stripped — openq only needs the in-memory codec.

## Clean-room note

miniz is a generic, permissively-licensed DEFLATE library — **not** kdb-derived. The
behaviour of `.Q.gz` (its three arities, return shapes) comes solely from the KX CC-BY docs
(`qdocs/`). The frozen rayforce base stays zero-dependency; this is wired into the build the
same way as `third_party/yyjson` and `third_party/picohttpparser` (see `Makefile`).

## Updating

Re-fetch `miniz.c` / `miniz.h` / `LICENSE` from the upstream release at the desired version
and update the pin above. No local modifications are made to the vendored sources.
