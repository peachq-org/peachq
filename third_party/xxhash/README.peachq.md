# xxHash (vendored)

- **Upstream:** https://github.com/Cyan4973/xxHash
- **Version:** release `v0.8.4` (2026-09-19), the version the vcpkg `xxhash` port that DuckDB's `hashfuncs`
  community extension builds against resolves to. XXH32 / XXH64 / XXH3 outputs have been frozen since 0.8.0, so
  any 0.8.x agrees with the extension.
- **License:** BSD-2-Clause (`LICENSE`, upstream's, verbatim; also `docs/licenses/xxhash-LICENSE`)
- **Files vendored:** `xxhash.h`, `xxhash.c` (upstream's own three-line implementation TU), `LICENSE`
- **sha256:** `xxhash.h` 3dc8d161e867a62d3417f7885b55fdaded8b7b497e2f165d05c94f4fe24f2ca4,
  `xxhash.c` 5c3591fe6e6c86a619eb26760e9520e37a6fd5152882ab5ad93f912e2a855966

## Why it is here

peachq's `.hash` namespace (`qlib/hash.q`, glue in `src/qlang/ops/q_hash.c`) answers the same values as DuckDB's
`hashfuncs` extension (`xxh32`, `xxh64`, `xxh3_64`, `xxh3_128` / `xxh3_128_hex`). That agreement is only real
when both sides run the same code, so the library is vendored rather than re-implemented. It is built as its own
TU through the Makefile's generic `third_party/%.o` rule (no `-Wextra`/`-Werror`), not as `XXH_INLINE_ALL`: the
glue includes `xxhash.h` for declarations only, which keeps peachq's own `-Werror` build untouched.

## Updating

Re-fetch `xxhash.h`, `xxhash.c` and `LICENSE` from the upstream tag, record the sha256s above, and re-run
`make q-qunit` (`qlib/test/hashTest.q` pins the published vectors). No local modifications are made.
