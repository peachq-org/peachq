# Hashing

`.hash` is a set of fast, non-cryptographic hash functions: xxHash (`xxh32`, `xxh64`, `xxh3_64`, `xxh3_128`),
rapidhash, and MurmurHash3 (`murmur3_32`, `murmur3_128`, `murmur3_x64_128`). They bucket, key, sample and
fingerprint; they are not for passwords or signatures (`md5` and `.Q.sha1` are the digests q ships).

The values are those of DuckDB's `hashfuncs` extension for the same bytes and seed, so a key bucketed in q matches
one bucketed in SQL.

## Loading

The `.hash` namespace is part of the standard library and loads on its first use.

## The eight functions

Each takes a string, a byte vector or a symbol (its text) and hashes the bytes; a char atom is one byte:

```q
q).hash.xxh32 "abc"
852579327
q).hash.xxh64 "abc"
4952883123889572249
q).hash.rapidhash `abc
-3798966695878718046
q).hash.murmur3_32 0x616263
3017643002
```

A 32-bit hash is a long holding the unsigned value. A 64-bit hash is the long with the same 64 bits, so a value
with its top bit set prints negative: `0x0 vs` shows the bytes, and `raze string 0x0 vs` the hex you would compare
with another system.

A 128-bit hash is 16 bytes. `xxh3_128` is in xxHash's canonical order (high 64 bits first), so `raze string` of it
is the hex form other xxHash implementations print:

```q
q)raze string .hash.xxh3_128 "abc"
"06b05ab6733a618578af5f94892f3950"
```

`murmur3_128` and `murmur3_x64_128` are the 16 bytes the reference implementation writes, which is what every
MurmurHash3 library hands back as bytes; `raze string reverse` reads them as one little-endian 128-bit number.

A column goes in one call: a symbol vector, or a list of strings, symbols and byte vectors, answers one hash per
element - a long vector for the 32- and 64-bit hashes, a list of 16-byte vectors for the 128-bit ones - and runs
several times faster than `each` over a million rows, because the loop never leaves C:

```q
q).hash.xxh64 `AAPL`MSFT`IBM
q).hash.murmur3_32 ("abc";`abc;0x616263)
```

A list of one-character strings is spelt `(enlist "a";enlist "b")`: `("a";"b")` is q's own collapse to the string
`"ab"`, one subject. The lift is one level deep; a dictionary maps its values and keeps its keys. Anything else -
a number, a byte atom, a nested list - is `'type` for the whole call.

## Seeds

The plain forms use seed 0. A seed is a deliberate act -
Spark's Murmur3 bucketing uses 42, sketches and experiment assignment pick their own - and goes through
`.hash.seeded`:

```q
q).hash.seeded[`murmur3_32;42;"abc"]
1313807976
q).hash.seeded[`xxh64;0;"abc"]~.hash.xxh64 "abc"
1b
```

`xxh32` and the three `murmur3` hashes take a 32-bit seed, 0 to 4294967295; the others take any 64-bit pattern,
so `-1` is the seed 18446744073709551615. A seed out of range, or an unknown algorithm name, is `'domain`.
