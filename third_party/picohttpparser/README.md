# picohttpparser (vendored)

- **Upstream:** https://github.com/h2o/picohttpparser
- **Version:** master @ `f4d94b48b31e0abae029ebeafcfd9ca0680ede58` (fetched 2026-07-18; upstream has no release tags)
- **License:** MIT (see `LICENSE`; upstream dual-licenses MIT / Perl — we take MIT) — Copyright (c) 2009-2014 Kazuho Oku, Tokuhiro Matsuno, Daisuke Murase, Shigeo Mitsunari
- **Files vendored:** `picohttpparser.c`, `picohttpparser.h`, `LICENSE`

## Why it is here

openq's single-port HTTP slice (kb/http.md: the kdb webserver shares the IPC listening
port) needs a correct, tiny HTTP/1.x *request-line + header* parser for the static file
server and the `.z.ph` hook. picohttpparser is the best-of-breed minimal parser (two
files, zero dependencies, battle-tested in h2o). All HTTP *semantics* — routing, the
`./html` root, the path-traversal guard, MIME map, response composition — live in openq's
own `src/qlang/q_http.c`; the vendored code only tokenizes the request.

## Clean-room note

picohttpparser is a generic, permissively-licensed HTTP parser — **not** kdb-derived.
The behaviour of the openq webserver (same-port serving, `.z.ph` contract) comes solely
from the KX CC-BY docs (`qdocs/`). The frozen rayforce base stays zero-dependency; this
is wired into the build the same way as `third_party/yyjson` (see `Makefile`).

## Updating

Re-fetch `picohttpparser.c` / `picohttpparser.h` from the upstream repo at the desired
commit and update the pin above. No local modifications are made to the vendored sources.
