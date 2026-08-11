# RE2 (vendored) — provenance and regeneration

- **Upstream, as we take it:** `third_party/re2` from the DuckDB source tree,
  https://github.com/duckdb/duckdb
- **Version pin:** DuckDB **v1.4.5** (`duckdb-1.4.5.tar.gz`, GitHub release tag)
- **Tarball sha256:** `29931ac91cf9077292773099a900c67be6d13b933978e176249b6e5b75b0b958`
- **Vendored-tree sha256:** `fb94c254556b9794a060017e5d5af97f201bf2882dd9be43246647560ebded60`
  (see *Drift* below)
- **License:** BSD-3-Clause — `LICENSE` in this directory is upstream's, shipped
  verbatim, and also recorded under `docs/licenses/re2-LICENSE`. `AUTHORS` is
  upstream's too. Fits the vendoring policy (small, permissive, yyjson precedent).

## Why DuckDB's copy and not google/re2

The whole point of the pin: a peachq predicate and the equivalent DuckDB SQL
predicate must mean the same thing, because a query can push down to DuckDB
storage. That agreement is only real if both sides run the same matcher, so we
vendor the matcher DuckDB itself ships. DuckDB's copy is also **already
de-abseil'd** — no `absl/` reference anywhere in the tree — which is what makes
it vendorable at all under our zero-dependency rule. Its `re2::` namespace is
renamed `duckdb_re2::`, which is harmless here (the shim is the only caller).

## There is no RE2 version, and what we do instead

RE2 as DuckDB vendors it carries **no version marker of any kind**: no
`RE2_VERSION`, nothing in its `CMakeLists.txt`, no version header. It is an
unversioned snapshot of a fork. So:

- the **pin is the DuckDB release** it came from, recorded in
  `src/qlang/io/q_re2_pin.h` and reported to q as `.regexp.version`;
- **drift** in the tree itself is caught by a digest, since no version string
  can catch it: `tools/re2-pin.sh` recomputes the vendored-tree sha256 above and
  runs at every rebuild of the RE2 archive, failing the build when the two
  disagree.

(A third mechanism — the pin compiled into both sides so a stale module was
refused at load — went away with the module itself; see below.)

## Vendoring shape

The tree is **verbatim upstream**, pruned to `re2/`, `util/`, `LICENSE` and
`AUTHORS`. No local patches, so a future re-vendor is a straight copy. Upstream's
`CMakeLists.txt` is dropped: peachq builds with a plain Makefile, and keeping a
build file nothing runs would only invite the belief that it does.

`RE2_ON_VALGRIND` (which DuckDB's CMake defines) is deliberately NOT defined —
it only enables upstream's valgrind-detection hooks, and the tree builds clean
without it.

## How it is built

RE2::Options is left at its DEFAULTS: q has no options argument, so
case-insensitivity and the newline flags ride inside the pattern (`(?i)` and
friends) and the two Options with no inline spelling became ordinary functions —
`QuoteMeta` is `.regexp.escape` (replacing DuckDB's `l`), and replace's global
flag is `.regexp.replace_all` (replacing `g`).

All 23 `.cc` files plus `src/qlang/io/q_re2_shim.cc` compile into
`build/libpqre2.a`, which is **linked into the executable on every platform**
(owner ruling 2026-08-10, reversing the dlopen'd-module design: it had no
Windows build arm, so `q.exe` shipped with no regex at all). See the RE2 block
in `Makefile`. `./q` therefore links a C++ runtime, and **a C++ compiler is now
a build requirement**, not an optional extra.

Windows cross-compiles the same sources with `x86_64-w64-mingw32-g++` and links
`-static-libstdc++ -static-libgcc` plus a static libwinpthread, so `q.exe` still
depends on nothing but KERNEL32/msvcrt/WS2_32. The POSIX-threads mingw flavour is
required: RE2 uses `std::mutex`/`std::once_flag`, which the win32-threads
libstdc++ does not define. On Debian/Ubuntu: `apt install g++-mingw-w64-x86-64`.

## Regeneration recipe (version bump)

```sh
V=1.4.6
curl -LO https://github.com/duckdb/duckdb/archive/refs/tags/v$V.tar.gz
sha256sum v$V.tar.gz                       # record it above
tar xzf v$V.tar.gz duckdb-$V/third_party/re2
rm -rf third_party/re2/re2 third_party/re2/util
cp -r duckdb-$V/third_party/re2/re2 duckdb-$V/third_party/re2/util third_party/re2/
cp duckdb-$V/third_party/re2/LICENSE duckdb-$V/third_party/re2/AUTHORS third_party/re2/
cp third_party/re2/LICENSE docs/licenses/re2-LICENSE
grep -rl absl third_party/re2 && echo "STOP: abseil reappeared — not vendorable"
tools/re2-pin.sh --print                   # the new tree digest
# then edit src/qlang/io/q_re2_pin.h: PQRE2_DUCKDB_PIN + PQRE2_SRC_SHA256,
# and the two hashes + the release above; finally
make && make q-test
```

Bumping the pin changes `.regexp.version`, so
`test/q/aigenerated/regex_version.qcmd` moves with it — that row is the pin's
ledger.
