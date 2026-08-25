# fmt (vendored) — provenance and regeneration

- **Upstream, as we take it:** `third_party/fmt` from the DuckDB source tree, https://github.com/duckdb/duckdb
- **Version pin:** DuckDB commit **`b929b8640f8722144bc0474dae4277148240a57e`** (2026-08-25 `main`)
- **Vendored-tree sha256:** `e12db194dc6da94fa4cd7ca60fd771ac3e746679c3cde92823746d70c17cabc9` (see *Drift* below)
- **License:** MIT — `LICENSE` in this directory is upstream's, shipped verbatim, and also recorded under
  `docs/licenses/fmt-LICENSE`. Every vendored file keeps its own copyright header. Fits the vendoring policy
  (small, permissive, the re2/yyjson precedent).

## Why DuckDB's copy and not upstream fmt

DuckDB's fmt is **patched**, and the patches are exactly what `.str.printf` is built on:

- **printf thousands-separator flags.** `%,d` `%'d` `%_d` group the integer part, the flag char BEING the
  separator, and `%.d` groups with `.`. Upstream fmt has none of this. It is also why `%'d` groups with an
  APOSTROPHE here where kdb-x's printf module always means a comma — a recorded divergence, resolved in
  DuckDB's favour (`actionable-plans/2026-08-25-str-printf-format.md`, ruling 3).
- **128-bit integer lanes** (`duckdb::hugeint_t`/`uhugeint_t`), and the namespace rename to `duckdb_fmt`.

Taking their copy also keeps `.str.format` and DuckDB SQL's `format()` meaning the same thing — the same
argument the re2 vendoring rests on, since a query can push down to DuckDB storage.

## Why the pin is a commit, not a version

fmt defines `FMT_VERSION`, but that names the upstream fmt release DuckDB forked, not DuckDB's patches on top —
the version string cannot move when a patch does. So the pin is the DuckDB commit
(`src/qlang/io/q_strfmt_pin.h`), and drift in the tree itself is caught by a digest: `tools/fmt-pin.sh`
recomputes the vendored-tree sha256 above and runs at every rebuild of the fmt archive, failing the build when
the two disagree.

The pin is a `main` commit rather than a release tag (which is what the re2 pin uses) for one reason: the fix
making printf's `%f`/`%e`/`%g` preserve a SIGNED integer argument's signedness landed after v1.4.5, and our lane
collapse routes q longs into exactly that path.

## Vendoring shape — verbatim, plus six stub headers that are OURS

`include/fmt/{core,format,format-inl,printf,ostream}.h`, `format.cc` and `LICENSE` are **verbatim upstream**;
they are what the digest covers. Everything else in this directory is ours and excluded from the digest, so
"the rest is verbatim" stays a checkable claim.

fmt as DuckDB ships it includes six DuckDB headers. peachq vendors no other part of DuckDB, so `stub/duckdb/`
supplies them — ~60 lines total, each marked `PEACHQ STUB`:

| stub | what fmt asks of it |
|---|---|
| `common/hugeint.hpp`, `common/uhugeint.hpp` | `hugeint_t`/`uhugeint_t` — the compiler's `__int128` pair; q never constructs one |
| `common/exception.hpp` | `InvalidInputException`, `InternalException` — what fmt throws; the text is never read |
| `common/limits.hpp` | `NumericLimits<T>::Maximum()` for the two 128-bit types only |
| `common/operator/cast_operators.hpp` | `Cast::Operation` on one path — the widening of an integer to 128 bits |
| `original/std/memory.hpp` | DuckDB's checked `unique_ptr` swap; we want the standard one, which `format.h` falls back to |

Upstream's `CMakeLists.txt` is dropped: peachq builds with a plain Makefile, and keeping a build file nothing
runs would only invite the belief that it does.

## How it is built

`format.cc` plus `src/qlang/io/q_strfmt_shim.cc` compile into `build/libpqfmt.a`, linked into the executable on
every platform — the same terms as `libpqre2.a`; see the fmt block in `Makefile`. Windows cross-compiles the
same sources with `x86_64-w64-mingw32-g++` under the existing re2-style rules (mingw-w64 gcc supports
`__int128` and C++ exceptions).

All of fmt stops at the shim: `q_strfmt_abi.h` is a flat `extern "C"` seam carrying four lanes and a byte
string, and every C++ exception is caught before it. No q type is visible to C++, and no C++ type is visible
to C.

## Regeneration recipe (pin bump)

```sh
S=<duckdb commit sha>
B=https://raw.githubusercontent.com/duckdb/duckdb/$S/third_party/fmt
for f in include/fmt/core.h include/fmt/format.h include/fmt/format-inl.h \
         include/fmt/printf.h include/fmt/ostream.h format.cc LICENSE; do
  curl -sfL "$B/$f" -o "third_party/fmt/$f"
done
cp third_party/fmt/LICENSE docs/licenses/fmt-LICENSE
grep -rn 'include "duckdb/' third_party/fmt/include   # any NEW duckdb header needs a new stub
tools/fmt-pin.sh --print                              # the new tree digest
# then edit src/qlang/io/q_strfmt_pin.h: PQFMT_DUCKDB_PIN + PQFMT_SRC_SHA256,
# and the commit + digest above; finally
make && make q-qunit
```

`qlib/test/strFormatTest.q` is the ledger a bump is checked against — its scanner-neutrality block is
unmodified-fmt grammar, so a change in fmt's own behaviour shows up there rather than in a user's output.
