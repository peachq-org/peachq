/ str.q — THE public .str surface: Python-shaped string helpers, so a user reuses the names they already know.
/ Python's exact SPELLINGS (isalpha, startswith, removeprefix — not the snake_case of qlib/q-coding-standards.md,
/ an owner-ratified departure for familiarity) and Python's exact SEMANTICS, with q idiom only where Python has no
/ opinion: a symbol reads wherever a string does, and everything works over a LIST of strings as readily as over one.
/ Python's empty-string rule beats q's — every predicate answers 0b for "", where q's own `all ""` is 1b.
/ Absent on purpose — q has them and a second spelling is worse than none: lower/upper, vs/sv (split/join), ss/ssr.
/ strip/lstrip/rstrip are C natives (src/qlang/ops/q_strns.c) — character scanning in q is slow.  Lambdas, not
/ natives, are the surface, so it stays DISCOVERABLE and the coercion and dispatch below keep ONE home.
/ ANY-ORDER LAW: definitions only at top level.

/ THE one coercion AND type gate.  A symbol reads as its string, a char atom as the one-char string, a symbol vector
/ as the list its callers' 0h arm recurses over.  Anything else is 'type — a long vector would silently answer 0b.
.str.i.text:{[s] $[type[s] in -11 11h; string s; -10h=type s; enlist s; type[s] in 0 10h; s; '`type]};

/ every character of a NON-EMPTY string in `alphabet` — Python's rule, where q's `all ""` would answer 1b.
.str.i.all_in:{[s;alphabet] $[0=count s; 0b; all s in alphabet]};

/ Python's cased-character rule: at least one letter, and every letter in `alphabet`.  Digits and punctuation are
/ uncased, so they neither qualify nor disqualify — "ABC1" is upper, "123" is not.
.str.i.cased_in:{[s;alphabet] .str.i.all_in[s where s in .Q.a,.Q.A; alphabet]};

/ leading and trailing " \t\n\r" removed.  NOT q's `trim`, whose domain is the char null instead.
/ @param s (any) a string, a symbol, or a list of either
.str.strip:{[s]
    s:.str.i.text s;
    $[0h=type s; .z.s each s; .str.i.strip s]};

/ leading " \t\n\r" removed.
/ @param s (any) a string, a symbol, or a list of either
.str.lstrip:{[s]
    s:.str.i.text s;
    $[0h=type s; .z.s each s; .str.i.lstrip s]};

/ trailing " \t\n\r" removed.
/ @param s (any) a string, a symbol, or a list of either
.str.rstrip:{[s]
    s:.str.i.text s;
    $[0h=type s; .z.s each s; .str.i.rstrip s]};

/ does s begin with prefix?  An empty prefix always does, and a prefix longer than s never does — which is why this
/ is `sublist` and not `#`, whose take WRAPS: 3#"ab" is "aba", so "ab" would wrongly start with "aba".
/ @param s (any) a string, a symbol, or a list of either
/ @param prefix (any) a string or symbol
.str.startswith:{[s;prefix]
    s:.str.i.text s;
    prefix:.str.i.text prefix;
    $[0h=type s; .z.s[;prefix] each s; prefix~(count prefix) sublist s]};

/ does s end with suffix?  Empty and over-long suffixes as above.
/ @param s (any) a string, a symbol, or a list of either
/ @param suffix (any) a string or symbol
.str.endswith:{[s;suffix]
    s:.str.i.text s;
    suffix:.str.i.text suffix;
    $[0h=type s; .z.s[;suffix] each s; suffix~(neg count suffix) sublist s]};

/ s without a leading prefix, or s unchanged when it does not start with one.  Only ONE copy is removed.
/ @param s (any) a string, a symbol, or a list of either
/ @param prefix (any) a string or symbol
.str.removeprefix:{[s;prefix]
    s:.str.i.text s;
    prefix:.str.i.text prefix;
    $[0h=type s; .z.s[;prefix] each s; .str.startswith[s;prefix]; (count prefix)_ s; s]};

/ s without a trailing suffix, or s unchanged when it does not end with one.  Only ONE copy is removed.
/ @param s (any) a string, a symbol, or a list of either
/ @param suffix (any) a string or symbol
.str.removesuffix:{[s;suffix]
    s:.str.i.text s;
    suffix:.str.i.text suffix;
    $[0h=type s; .z.s[;suffix] each s; .str.endswith[s;suffix]; (neg count suffix)_ s; s]};

/ is every character a letter, and is there at least one?
/ @param s (any) a string, a symbol, or a list of either
.str.isalpha:{[s]
    s:.str.i.text s;
    $[0h=type s; .z.s each s; .str.i.all_in[s;.Q.a,.Q.A]]};

/ is every character a digit, and is there at least one?
/ @param s (any) a string, a symbol, or a list of either
.str.isdigit:{[s]
    s:.str.i.text s;
    $[0h=type s; .z.s each s; .str.i.all_in[s;.Q.n]]};

/ is every character a letter or a digit, and is there at least one?  NOT .Q.an, which is the IDENTIFIER alphabet
/ and admits "_" — .str.isalnum "a_b" is 0b.
/ @param s (any) a string, a symbol, or a list of either
.str.isalnum:{[s]
    s:.str.i.text s;
    $[0h=type s; .z.s each s; .str.i.all_in[s;.Q.a,.Q.A,.Q.n]]};

/ is every character one of " \t\n\r", and is there at least one?
/ @param s (any) a string, a symbol, or a list of either
.str.isspace:{[s]
    s:.str.i.text s;
    $[0h=type s; .z.s each s; .str.i.all_in[s;" \t\n\r"]]};

/ is every CASED character upper case, and is there at least one?  Uncased characters are ignored, so "ABC1" is 1b.
/ @param s (any) a string, a symbol, or a list of either
.str.isupper:{[s]
    s:.str.i.text s;
    $[0h=type s; .z.s each s; .str.i.cased_in[s;.Q.A]]};

/ is every CASED character lower case, and is there at least one?  Uncased characters are ignored, so "abc1" is 1b.
/ @param s (any) a string, a symbol, or a list of either
.str.islower:{[s]
    s:.str.i.text s;
    $[0h=type s; .z.s each s; .str.i.cased_in[s;.Q.a]]};
