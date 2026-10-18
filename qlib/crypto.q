/ Cryptographic digests (SHA-2, SHA-3, BLAKE2, MD5, SHA-1, MD4), HMAC and random bytes from the platform's OpenSSL.
/ hash and hmac read the bytes of a string, a byte vector or a symbol (its text); a char atom is one byte.  A symbol
/ vector, or a list of those subjects, answers one digest per element in a single call (a list of one-char strings is
/ spelt (enlist "a";enlist "b") - ("a";"b") is the string "ab"); anything else is 'type.  A digest, a MAC and a random
/ draw are byte vectors, as md5 answers, a list of them over a collection; raze string gives the hex.  A box without
/ OpenSSL answers 'nyi, as a TLS connection does there.
/ .
/ @eg raze string .crypto.sha256 "hello world"
/ @eg .crypto.sha256 `AAPL`MSFT`IBM
/ @eg .crypto.hmac[`$"sha2-256";"secret";"hello world"]

.pq.load_natives`crypto;

/ The digest named by algo: md4 md5 sha1 sha2-224 sha2-256 sha2-384 sha2-512 sha3-224 sha3-256 sha3-384 sha3-512
/ keccak224 keccak256 keccak384 keccak512 blake2b-512 blake2s-256; the keccak names answer SHA-3.  .crypto.algos[]
/ lists the names this OpenSSL computes; any other name is 'domain (OpenSSL 3 keeps md4 in its legacy provider, so
/ it is usually absent).
/ @param algo (symbol) the digest name; the same name as a string also reads
/ @param data (any) the string, byte vector, symbol or char to digest, or a symbol vector / list of those for one
/ digest each
/ @return (byte list) the digest; a list of them over a collection
/ @throws domain a name this process has no digest for
/ @throws type data (or an element of it) is not text, bytes or a symbol
/ @eg .crypto.hash[`$"sha2-256";"abc"]
/ @eg raze string .crypto.hash[`md5;"abc"]
/ @eg .crypto.hash[`$"sha2-256";("abc";`abc;0x616263)]
.crypto.hash:{[algo;data] .crypto.i.hash[algo;data]};

/ The HMAC (RFC 2104) of data under secret, with the digest named by algo as .crypto.hash names it.
/ @param algo (symbol) the digest name, as .crypto.hash takes it
/ @param secret (any) the key, ONE string, byte vector, symbol or char; "" is a key
/ @param data (any) the message, in the same forms, or a symbol vector / list of those for one MAC each under the
/ same key
/ @return (byte list) the MAC, as long as the digest; a list of them over a collection
/ @throws domain a name this process has no digest for
/ @throws type secret is not one text, bytes or symbol, or data (or an element of it) is not
/ @eg raze string .crypto.hmac[`$"sha2-256";"key";"abc"]
/ @eg .crypto.hmac[`$"sha2-256";"key";`abc`hello]
.crypto.hmac:{[algo;secret;data] .crypto.i.hmac[algo;secret;data]};

/ n cryptographically secure random bytes from OpenSSL's generator, which seeds itself from the OS; \S does not
/ reach it.
/ @param n (long) an integer atom, 1 to 4294967295
/ @return (byte list) n bytes
/ @throws domain n outside 1 to 4294967295
/ @throws type n is not an integer
/ @eg .crypto.randbytes 16
.crypto.randbytes:{[n] .crypto.i.randbytes n};

/ The digest names .crypto.hash accepts in this process: the seventeen it knows, less any this OpenSSL cannot
/ compute.
/ @return (symbol list) the names, always holding `$"sha2-256"
/ @eg .crypto.algos[]
.crypto.algos:{[] .crypto.i.algos[]};

/ SHA-256 of data: .crypto.hash[`$"sha2-256";].
/ @eg raze string .crypto.sha256 "abc"
.crypto.sha256:.crypto.hash[`$"sha2-256";];
