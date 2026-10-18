# Cryptographic digests

`.crypto` computes cryptographic digests, HMACs and random bytes with the OpenSSL library already on the machine:
the SHA-2 and SHA-3 families, BLAKE2, MD5, SHA-1 and MD4, keyed as HMAC, plus `randbytes` for keys, salts and
nonces. (`.hash` is the sibling for fast, non-cryptographic hashing; `md5` and `.Q.sha1` are the digests q ships.)

The algorithm names are spelt as DuckDB's `crypto` extension spells them, so a digest computed here equals one
computed in SQL.

## Loading

The `.crypto` namespace is part of the standard library and loads on its first use. It needs OpenSSL (1.1 or 3) on
the machine, found the way the TLS client finds it; without one every call is `'nyi`, and a static build answers
the `'static-dlopen` line naming the download that can load libraries.

## Digests

`.crypto.hash[algo;x]` takes a string, a byte vector, a symbol (its text) or a char (one byte) and answers the digest
as bytes, the way `md5` does; `raze string` gives the hex:

```q
q).crypto.hash[`$"sha2-256";"abc"]
0xba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad
q)raze string .crypto.hash[`md5;"abc"]
"900150983cd24fb0d6963f7d28e17f72"
q).crypto.sha256 "abc"            / .crypto.hash[`$"sha2-256";]
```

The names (a hyphen means the symbol is written `` `$"sha2-256" ``):

| family | names |
|---|---|
| SHA-2 | `sha2-224` `sha2-256` `sha2-384` `sha2-512` |
| SHA-3 | `sha3-224` `sha3-256` `sha3-384` `sha3-512` |
| Keccak | `keccak224` `keccak256` `keccak384` `keccak512` |
| BLAKE2 | `blake2b-512` `blake2s-256` |
| legacy | `md5` `sha1` `md4` |

The `keccak*` names answer the SHA-3 digests.

`.crypto.algos[]` lists the names the OpenSSL on this machine can compute; a name not in it is `'domain`. OpenSSL 3
keeps MD4 in its legacy provider, so `md4` is usually absent there, and a 1.1.0 box has no SHA-3 or BLAKE2.

A column goes in one call: a symbol vector, or a list of strings, symbols and byte vectors, answers one digest per
element - a list of byte vectors - and the loop never leaves C:

```q
q).crypto.sha256 `AAPL`MSFT`IBM
q).crypto.hash[`md5;("abc";`abc;0x616263)]
```

A list of one-character strings is spelt `(enlist "a";enlist "b")`: `("a";"b")` is q's own collapse to the string
`"ab"`, one subject. The lift is one level deep; a dictionary maps its values and keeps its keys. Anything else -
a number, a byte atom, a nested list - is `'type` for the whole call.

## HMAC

`.crypto.hmac[algo;secret;data]` is the keyed digest of RFC 2104; the message takes the same forms as `.crypto.hash`,
a column included, the key is one string, byte vector, symbol or char, and `""` is a key:

```q
q)raze string .crypto.hmac[`$"sha2-256";"key";"abc"]
"9c196e32dc0175f86f4b1cb89289d6619de6bee699e4c378e68309ed97a1a6ab"
```

## Random bytes

`.crypto.randbytes n` answers `n` bytes from OpenSSL's cryptographically secure generator, which seeds itself from
the operating system - `\S` does not reach it, so it is for keys, salts and tokens, not for a reproducible
simulation (that is `?`). `n` is an integer from 1 to 4294967295: a count outside that range is `'domain`, and
anything that is not an integer is `'type`.

```q
q)raze string .crypto.randbytes 16
"3f7c0a9e5d1b84c2a6e0f19d7b42c8e3"
```
