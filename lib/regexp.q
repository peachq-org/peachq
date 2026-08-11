/ regexp.q — THE public .regexp surface (user-docs/regex.md).
/ Lambdas, not natives, so the surface is DISCOVERABLE: peachq renders a lambda
/ from its verbatim source, so typing `.regexp.matches` prints the signature and
/ names its arguments, where a native prints as an opaque primitive.  The
/ parameter names here are the document's words — subject, pattern, replacement.
/ Every function is FIXED-ARITY: q has no optional argument (a lambda called short
/ projects rather than defaulting), which is exactly why there is no options
/ argument.  Flags ride inside the pattern — (?i) (?s) (?m) — and the two RE2
/ options with no inline spelling are functions instead: escape and replace_all.
/ Shape is not handled here: each .regexp.i.* native takes the subject WHOLE and
/ distributes over a collection or dict in ONE call (ops/q_str.c's
/ q_str_subject_map, shared with `like`), so a million-row column is one dispatch.
/ ANY-ORDER LAW: definitions only at top level.

/ does the pattern match anywhere in the subject?
.regexp.matches:{[subject;pattern] .regexp.i.test[subject;pattern;0b]}
/ must the WHOLE subject match?
.regexp.full_match:{[subject;pattern] .regexp.i.test[subject;pattern;1b]}
/ the matched text, "" when nothing matches
.regexp.extract:{[subject;pattern] .regexp.i.extract[subject;pattern]}
/ the capture groups of the FIRST match, () when nothing matches
.regexp.groups:{[subject;pattern] .regexp.i.groups[subject;pattern]}
/ every non-overlapping match
.regexp.extract_all:{[subject;pattern] .regexp.i.extract_all[subject;pattern]}
/ one group-list per match, so groups_all[...][;1] is group 2 across every match
.regexp.groups_all:{[subject;pattern] .regexp.i.groups_all[subject;pattern]}
/ the FIRST match replaced; \1 to \9 name capture groups
.regexp.replace:{[subject;pattern;replacement] .regexp.i.replace[subject;pattern;replacement;0b]}
/ EVERY match replaced — DuckDB's "g" option, given a name of its own
.regexp.replace_all:{[subject;pattern;replacement] .regexp.i.replace[subject;pattern;replacement;1b]}
/ the subject split on the pattern; no match splits into the subject itself
.regexp.split:{[subject;pattern] .regexp.i.split[subject;pattern]}
/ the pattern that matches `text` LITERALLY — RE2's QuoteMeta, and what replaced
/ DuckDB's "l" option, which was never expressible inside a pattern
.regexp.escape:{[text] .regexp.i.escape text}

/ The version correspondence, COMPARED and never enforced: our RE2 is vendored
/ from one DuckDB release, but DuckDB is optional and PEACHQ_DUCKDB_LIB may
/ legitimately point at another, so this answers a boolean for a caller (or a
/ test) to judge rather than signalling.  Takes an OPEN provider handle and asks
/ over it — the same route any user has, no internal reached into.
.regexp.duckdb_match:{[handle] .regexp.version~first (handle "SELECT version() AS v")`v}
