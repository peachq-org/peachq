# rapidhash (vendored)

- **Upstream:** https://github.com/Nicoshev/rapidhash
- **Version:** commit `bc4b4baa48a15ff52ff4725e1ccdcda62815221c` = tag `rapidhash_v3` — the EXACT revision
  DuckDB's `hashfuncs` community extension pins in its vcpkg overlay port
  (`query-farm/hashfuncs: vcpkg-overlay/rapidhash/portfile.cmake`, unchanged since 2026-02-18). rapidhash v1, v2
  and v3 are different hash functions with different outputs; only this revision agrees with the extension.
- **License:** the `LICENSE` file shipped at that commit (MIT text, copyright Nicolas De Carli), verbatim; also
  `docs/licenses/rapidhash-LICENSE`.
- **Files vendored:** `rapidhash.h` (header-only), `LICENSE`
- **sha256:** `rapidhash.h` 807874eeb23339eaa4b435fbadd33d46718c88e32f3eb67bab3eb965eecc626e

## Why it is here

peachq's `.hash.rapidhash` (`qlib/hash.q`, glue in `src/qlang/ops/q_hash.c`) answers DuckDB `hashfuncs`'s
`rapidhash` value bit for bit, so the two sides can bucket or key the same data identically. The header is
included by the glue TU alone; it compiles clean under peachq's `-Wall -Wextra -Werror -std=c17` on gcc, clang and
mingw-w64, and carries its own portable fallback where `__int128` is absent.

## Updating

Only when `hashfuncs` moves its pin: re-fetch `rapidhash.h` and `LICENSE` at the new commit, record it and the
sha256 above, and re-run `make q-qunit` (`qlib/test/hashTest.q` pins values observed from the extension). No local
modifications are made.
