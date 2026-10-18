/ Non-cryptographic hashes - xxHash (xxh32, xxh64, xxh3_64, xxh3_128), rapidhash and MurmurHash3 - for bucketing, keying,
/ sampling and fingerprinting (md5 and .Q.sha1 are the digests).
/ Every function hashes the bytes of a string, a byte vector or a symbol (its text); a char atom is one byte.  A symbol
/ vector, or a list of those subjects, answers one hash per element in a single call (a list of one-char strings is
/ spelt (enlist "a";enlist "b") - ("a";"b") is the string "ab"); anything else is 'type.  A 32-bit hash rides a long;
/ a 64-bit hash is the long with the same bit pattern (negative when the top bit is set); a 128-bit hash is 16 bytes,
/ a list of them over a collection.  The plain forms use seed 0; .hash.seeded takes the seed explicitly.  The values
/ are those of DuckDB's hashfuncs extension for the same bytes and seed, so a key bucketed in q matches one bucketed in SQL.
/ .
/ @eg .hash.xxh64 "hello world"
/ @eg .hash.xxh64 `AAPL`MSFT`IBM
/ @eg .hash.seeded[`murmur3_32;42;"hello world"]

.pq.load_natives`hash;

/ xxHash XXH32 with seed 0, as a long holding the unsigned 32-bit value.
/ @eg .hash.xxh32 "abc"
.hash.xxh32:{.hash.i.hash[`xxh32;0;x]};
/ xxHash XXH64 with seed 0, as the long with the same 64 bits.
/ @eg .hash.xxh64 "abc"
.hash.xxh64:{.hash.i.hash[`xxh64;0;x]};
/ xxHash XXH3 64-bit with seed 0, as the long with the same 64 bits.
/ @eg .hash.xxh3_64 "abc"
.hash.xxh3_64:{.hash.i.hash[`xxh3_64;0;x]};
/ xxHash XXH3 128-bit with seed 0, as 16 bytes in xxHash's canonical order (high 64 bits first), so `raze string` of
/ the result is the hex other xxHash implementations print.
/ @eg raze string .hash.xxh3_128 "abc"
.hash.xxh3_128:{.hash.i.hash[`xxh3_128;0;x]};
/ rapidhash v3 with seed 0, as the long with the same 64 bits.
/ @eg .hash.rapidhash "abc"
.hash.rapidhash:{.hash.i.hash[`rapidhash;0;x]};
/ MurmurHash3 x86_32 with seed 0, as a long holding the unsigned 32-bit value.
/ @eg .hash.murmur3_32 "abc"
.hash.murmur3_32:{.hash.i.hash[`murmur3_32;0;x]};
/ MurmurHash3 x86_128 with seed 0, as the 16 bytes the reference writes (four little-endian words), what every
/ MurmurHash3 library hands back as bytes; `raze string reverse` reads them as one little-endian number.
/ @eg .hash.murmur3_128 "abc"
.hash.murmur3_128:{.hash.i.hash[`murmur3_128;0;x]};
/ MurmurHash3 x64_128 with seed 0, as the 16 bytes the reference writes (two little-endian words), what every
/ MurmurHash3 library hands back as bytes; `raze string reverse` reads them as one little-endian number.
/ @eg .hash.murmur3_x64_128 "abc"
.hash.murmur3_x64_128:{.hash.i.hash[`murmur3_x64_128;0;x]};

/ Any of the eight hashes with an explicit seed; seed 0 is the plain form.
/ @param algo (symbol) one of `xxh32`xxh64`xxh3_64`xxh3_128`rapidhash`murmur3_32`murmur3_128`murmur3_x64_128; the
/ same name as a string also reads
/ @param seed (long) any integer atom: xxh32 and the murmur3 hashes take 0 to 4294967295, the others any 64-bit
/ pattern (-1 is the seed 18446744073709551615)
/ @param data (any) the string, byte vector, symbol or char to hash, or a symbol vector / list of those for one hash each
/ @return as the algorithm's plain form; a vector (or list of 16-byte vectors) over a collection
/ @throws domain an unknown algorithm, or a 32-bit seed out of range
/ @throws type data (or an element of it) is not text, bytes or a symbol, or seed is not an integer
/ @eg .hash.seeded[`xxh64;42;"abc"]
/ @eg .hash.seeded[`murmur3_32;42;("abc";`abc;0x616263)]
.hash.seeded:{[algo;seed;data] .hash.i.hash[algo;seed;data]};
