# MurmurHash3 (vendored)

- **Upstream:** https://github.com/aappleby/smhasher (Austin Appleby's reference implementation)
- **Version:** commit `61a0530f28277f2e850bfc39600ce61d02b518de` — the revision the vcpkg `murmurhash` port
  (which DuckDB's `hashfuncs` community extension builds against) pins. The reference has not changed since 2016.
- **License:** public domain — the author's disclaimer at the head of each file is the whole licence text; a copy
  is `docs/licenses/murmurhash3-LICENSE`.
- **Files vendored:** `src/MurmurHash3.cpp`, `src/MurmurHash3.h`, verbatim.
  `MurmurHash3.c` is peachq's: three lines that compile the `.cpp` as C17 unchanged (see below).
- **sha256:** `MurmurHash3.cpp` 30f121ed155ebf336af398aabb7d8d157afdfafc8d981e7b48d2a1ceb4b63e4e,
  `MurmurHash3.h` 9af2003e3885c841fa0ac08cb5d5f31541ee9d793f8a9c4a4f771f43fb0d6f41

## Why it is here

peachq's `.hash.murmur3_32` / `.hash.murmur3_128` / `.hash.murmur3_x64_128` (`qlib/hash.q`, glue in
`src/qlang/ops/q_hash.c`) answer the same values as DuckDB `hashfuncs`'s `murmurhash3_32` /
`murmurhash3_128` / `murmurhash3_x64_128`, which are these three functions over the same bytes.

## Why the shim

The reference is C++ only in its file extension: it is C17 except for two helpers (`rotl32`, `rotl64`) declared
plain `inline`. In C99 inline semantics such a definition is not an external definition, so an unoptimised build
(our ASan/UBSan debug `./q`) fails to link. `MurmurHash3.c` includes the `.cpp` and re-declares the two helpers
without `inline`, which by C17 6.7.4p7 turns this TU's definitions into external ones. The upstream files stay
byte-identical and the generic `third_party/%.o` rule builds the shim without `-Wextra`/`-Werror` (the reference's
switch fallthroughs).

The reference also reads whole 32/64-bit words from the input pointer in place, so the glue hands it aligned
memory (a short symbol's text lives inline in its header at an odd offset) — the only one of the three vendored
hashes that needs this.

## Updating

Re-fetch the two upstream files at the desired commit, record it and the sha256s above, and re-run `make q-qunit`
(`qlib/test/hashTest.q` pins the reference's vectors and the values observed from the extension).
