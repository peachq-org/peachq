# yyjson (vendored)

- **Upstream:** https://github.com/ibireme/yyjson
- **Version:** 0.10.0 (pinned release tag)
- **License:** MIT (see `LICENSE`) — Copyright (c) 2020 YaoYuan
- **Files vendored:** `yyjson.c`, `yyjson.h`, `LICENSE`

## Why it is here

openq's `.j.k` (JSON → q) uses yyjson to do the raw tokenize / validate / unescape /
number-parse. The JSON-node → `ray_t` mapping (q-type semantics) lives in openq's own glue
(`src/qlang/q_json.c`); yyjson only produces a validated DOM. Read flags enable
`YYJSON_READ_ALLOW_INF_AND_NAN` so openq can read JSON that kdb+ itself saved
(`inf`/`-inf`/`nan`).

## Clean-room note

yyjson is a generic, permissively-licensed JSON parser — **not** kdb-derived. It carries no
clean-room concern: the *semantics* of q's JSON mapping come solely from the KX CC-BY docs
(`qdocs/`), never from another q-JSON implementation. This is a normal vendored dependency;
the frozen rayforce base stays untouched and zero-dependency. Wired into the q-layer build
only (see `Makefile`).

## Updating

Re-fetch `yyjson.c` / `yyjson.h` / `LICENSE` from the upstream repo at the desired release
tag and update the version above. No local modifications are made to the vendored sources.
