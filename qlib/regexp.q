/ Regular expressions in RE2 syntax: match, extract, capture groups, replace, split and escape.
/ subject is one string or a list of them; flags go inside the pattern: (?i) (?s) (?m).  Every function is
/ fixed-arity, so there is no options argument.
/ .
/ @eg .regexp.extract["order 123 of 456";"[0-9]+"]
/ @eg .regexp.replace_all[("a-b";"c-d");"-";"_"]

.pq.load_natives`regexp;

/ Does the pattern match anywhere in the subject?
/ @eg .regexp.matches["Hello";"(?i)^h"]
.regexp.matches:{[subject;pattern] .regexp.i.test[subject;pattern;0b]}
/ Does the pattern match the whole subject?
/ @eg .regexp.full_match["abc";"a.c"]
.regexp.full_match:{[subject;pattern] .regexp.i.test[subject;pattern;1b]}
/ The first matched text, "" when nothing matches.
/ @eg .regexp.extract["abc123";"[0-9]+"]
.regexp.extract:{[subject;pattern] .regexp.i.extract[subject;pattern]}
/ The capture groups of the first match, () when nothing matches.
/ @eg .regexp.groups["2026-09-19";"(\\d+)-(\\d+)-(\\d+)"]
.regexp.groups:{[subject;pattern] .regexp.i.groups[subject;pattern]}
/ Every non-overlapping match.
/ @eg .regexp.extract_all["a1b22c333";"[0-9]+"]
.regexp.extract_all:{[subject;pattern] .regexp.i.extract_all[subject;pattern]}
/ One group list per match, so .regexp.groups_all[...][;1] is the second group across every match.
/ @eg .regexp.groups_all["a=1, b=2";"(\\w)=(\\d)"]
.regexp.groups_all:{[subject;pattern] .regexp.i.groups_all[subject;pattern]}
/ The first match replaced.
/ @param replacement may name capture groups as \1 to \9
/ @eg .regexp.replace["2026-09-19";"(\\d+)-(\\d+)-(\\d+)";"\\3/\\2/\\1"]
.regexp.replace:{[subject;pattern;replacement] .regexp.i.replace[subject;pattern;replacement;0b]}
/ Every match replaced.
/ @param replacement may name capture groups as \1 to \9
/ @eg .regexp.replace_all["a1b2";"[0-9]";"#"]
.regexp.replace_all:{[subject;pattern;replacement] .regexp.i.replace[subject;pattern;replacement;1b]}
/ The subject split on the pattern; no match answers the subject itself as one piece.
/ @eg .regexp.split["a, b;c";"[,;] *"]
.regexp.split:{[subject;pattern] .regexp.i.split[subject;pattern]}
/ The pattern that matches text literally (RE2's QuoteMeta).
/ @eg .regexp.matches["1+1=2";.regexp.escape "1+1"]
.regexp.escape:{[text] .regexp.i.escape text}

/ Whether an open DuckDB handle's RE2 is the version vendored here; answers a boolean rather than signalling, since
/ PEACHQ_DUCKDB_LIB may legitimately point at another release.
/ @eg .regexp.duckdb_match .duckdb.main[]
.regexp.duckdb_match:{[handle] .regexp.version~first (handle "SELECT version() AS v")`v}
