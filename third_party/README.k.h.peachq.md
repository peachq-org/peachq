# `k.h` (vendored) — provenance

- **Upstream:** `c/c/k.h` from https://github.com/KxSystems/kdb (KX Systems).
- **Fetched:** 2026-09-22, `master`.
- **File sha256:** `7da364e03c053942ab35ea52da28c6fe75dda69aba6dc25fbd7503e42a72209a`
- **License:** Apache-2.0 — upstream's `LICENSE`, shipped verbatim as
  `docs/licenses/kx-kdb-LICENSE`.
- **Shipped verbatim.** Not edited, not reformatted. If it is ever re-fetched, the sha above must be
  updated in the same commit.

## What it is for

It is the header a real kdb+ C extension compiles against, and it is here for exactly one consumer:
`test/kapi/fixture.c`, the `2:` test extension. The fixture must see KX's own `struct k0`, its own
`kI(x)` / `xt` / `xn` macros and its own function declarations, or the test would be pinning our idea
of the contract instead of the contract itself.

**Nothing under `src/` includes it.** `src/qlang/io/q_kapi.c` declares its own `struct k0`, so a build
of `./q` never depends on a vendored third-party header for its own ABI. That the two layouts agree is
what `test/q/kapi.qcmd` proves.
