/ RE2 regex conformance: the `rlike` keyword and the .regexp namespace.
/ Sources: user-docs/regex.md, the published RE2 syntax
/ (github.com/google/re2/wiki/Syntax) and DuckDB's documented regexp semantics.
/ There is no options argument — flags live inside the pattern ((?i) (?s) (?m)) —
/ and the two RE2 options with no inline form are functions: .regexp.escape
/ (literal) and .regexp.replace_all (global).
system "d .regexpTest";

testRlikeKeyword:{
    .qunit.assertEquals["abc" rlike "b"; 1b; "rlike matches anywhere in the subject"];
    .qunit.assertEquals["abc" rlike "^b"; 0b; "^ anchors at the start"];
    .qunit.assertEquals["abc" rlike "^a.c$"; 1b; "^ and $ anchor both ends"];
    .qunit.assertEquals["abc" rlike "d"; 0b; "no match answers 0b"];
    .qunit.assertEquals["word" rlike "^w"; 1b; "a plain word matches at the start"]};

testRlikeSubjectForms:{
    .qunit.assertEquals["c" rlike "c"; 1b; "a char atom is a subject"];
    .qunit.assertEquals[`one rlike "^o"; 1b; "a symbol subject reads as its text"]};

testRlikeOverCollections:{
    .qunit.assertEquals[`AAPL`MSFT`IBM rlike "^[AM]"; 110b; "a symbol vector answers one flag per element"];
    .qunit.assertEquals[("words";"other") rlike "^w"; 10b; "a list of strings answers one flag per string"];
    .qunit.assertEquals[("ab";`c) rlike "b|c"; 11b; "strings and symbols mix in one subject list"]};

testRlikeEmptySubject:{
    .qunit.assertEquals["" rlike "^$"; 1b; "the empty string matches the empty pattern"];
    .qunit.assertEquals["" rlike "."; 0b; "the empty string has no character for ."];
    .qunit.assertEquals[` rlike "^$"; 1b; "the null symbol reads as empty text"]};

testRlikeInQuery:{
    r:select from ([] sym:`AAPL`MSFT`IBM) where sym rlike "^[AM]";
    .qunit.assertEquals[r; ([] sym:`AAPL`MSFT); "rlike is a where-clause predicate"]};

testMatches:{
    .qunit.assertEquals[.regexp.matches["abc";"b"]; 1b; "matches is unanchored"];
    .qunit.assertEquals[.regexp.matches["abc";"d"]; 0b; "matches answers 0b when absent"]};

testFullMatch:{
    .qunit.assertEquals[.regexp.full_match["abc";"b"]; 0b; "full_match rejects a partial match"];
    .qunit.assertEquals[.regexp.full_match["abc";"a.c"]; 1b; "full_match accepts a whole-subject pattern"];
    .qunit.assertEquals[.regexp.full_match["abc";"abc"]; 1b; "full_match accepts the literal subject"]};

testFlagsInPattern:{
    .qunit.assertEquals[.regexp.matches["ABC";"b"]; 0b; "matching is case-sensitive by default"];
    .qunit.assertEquals[.regexp.matches["ABC";"(?i)b"]; 1b; "(?i) inside the pattern turns case off"];
    .qunit.assertEquals["ABC" rlike "(?i)b"; 1b; "rlike gets its flags the same way"]};

testCharacterClassEscape:{
    .qunit.assertEquals[.regexp.matches["a1";"\\d"]; 1b; "a q-doubled backslash reaches RE2 as the digit class"];
    .qunit.assertEquals[.regexp.matches["ab";"\\d"]; 0b; "\\d does not match a letter"]};

testExtract:{
    .qunit.assertEquals[.regexp.extract["2026-08-10";"[0-9]{4}"]; "2026"; "extract answers the first match"];
    .qunit.assertEquals[.regexp.extract["abc";"z"]; ""; "no match answers an empty string"];
    .qunit.assertEquals[type .regexp.extract["abc";"z"]; 10h; "the empty no-match result is still a string"];
    .qunit.assertEquals[type .regexp.extract["a1";"[0-9]"]; 10h; "a one-character result is a one-element string, never a char atom"]};

testGroups:{
    g:.regexp.groups["2026-08-10";"([0-9]{4})-([0-9]{2})"];
    .qunit.assertEquals[g; ("2026";"08"); "groups answers one string per capture group, in pattern order"];
    .qunit.assertEquals[`year`month!g; `year`month!("2026";"08"); "naming the groups is an ordinary dict build"];
    .qunit.assertEquals[.regexp.groups["b";"(a)|(b)"]; ("";enlist"b"); "a group that did not participate reads as an empty string"];
    .qunit.assertEquals[.regexp.groups["abc";"z"]; (); "no match answers an empty list"];
    .qunit.assertEquals[type .regexp.groups["abc";"z"]; 0h; "that empty is a general list"]};

testExtractAll:{
    .qunit.assertEquals[.regexp.extract_all["a1b22c";"[0-9]+"]; (enlist"1";"22"); "extract_all answers every match"];
    .qunit.assertEquals[.regexp.extract_all["abc";"[0-9]"]; (); "no match answers an empty list"]};

/ one group-list PER match, so picking a group across every match is ordinary
/ indexing — DuckDB spells that regexp_extract_all(s,p,group)
testGroupsAll:{
    .qunit.assertKnown[.regexp.groups_all["a1 b22";"([a-z])([0-9]+)"]; `groupsAll; "groups_all answers a group-list per match"];
    .qunit.assertEquals[.regexp.groups_all["a1 b22";"([a-z])([0-9]+)"][;1]; (enlist"1";"22"); "indexing at 1 takes the second group of every match"];
    .qunit.assertEquals[.regexp.groups_all["abc";"([0-9])"]; (); "no match answers an empty list"]};

testReplace:{
    .qunit.assertEquals[.regexp.replace["a-b-c";"-";"+"]; "a+b-c"; "replace rewrites the first match only"];
    .qunit.assertEquals[.regexp.replace_all["a-b-c";"-";"+"]; "a+b+c"; "replace_all rewrites every match"];
    .qunit.assertEquals[.regexp.replace["John Smith";"(\\w+) (\\w+)";"\\2 \\1"]; "Smith John"; "\\1 \\2 in the replacement name the capture groups"];
    .qunit.assertEquals[.regexp.replace["abc";"z";"+"]; "abc"; "no match leaves the subject alone"]};

testSplit:{
    .qunit.assertEquals[.regexp.split["a,b,,c";","]; (enlist"a";enlist"b";"";enlist"c"); "an adjacent pair of delimiters yields an empty field"];
    .qunit.assertEquals[.regexp.split["abc";","]; enlist"abc"; "no delimiter answers the whole subject as one field"]};

/ escape is RE2's QuoteMeta, replacing DuckDB's "l" option — an RE2::Options
/ property that could never have an in-pattern spelling. Composing beats a flag:
/ visible at the call site, and it works with every function.
testEscape:{
    .qunit.assertEquals[.regexp.escape "1.5-2.0?"; "1\\.5\\-2\\.0\\?"; "escape backslashes every metacharacter"];
    .qunit.assertEquals[.regexp.matches["a.c";.regexp.escape "."]; 1b; "an escaped dot matches a literal dot"];
    .qunit.assertEquals[.regexp.matches["abc";.regexp.escape "."]; 0b; "an escaped dot no longer matches any character"]};

/ distribution is a property of the namespace, not of `each`: a collection
/ subject maps in one native call with the pattern compiled once
testDistributionOverLists:{
    .qunit.assertEquals[.regexp.matches[`AAPL`MSFT`IBM;"^[AM]"]; 110b; "matches distributes over a symbol vector"];
    .qunit.assertEquals[.regexp.full_match[("abc";"ab");"a.c"]; 10b; "full_match distributes over a list of strings"];
    .qunit.assertEquals[.regexp.extract[("a1";"b22");"[0-9]+"]; (enlist"1";"22"); "extract distributes to one string per subject"];
    .qunit.assertEquals[.regexp.extract_all[("a1";"b22c33");"[0-9]+"]; (enlist enlist"1";("22";"33")); "extract_all answers a list per subject, so a collection gives list-of-lists"];
    .qunit.assertEquals[.regexp.split[("a,b";"c,d");","]; ((enlist"a";enlist"b");(enlist"c";enlist"d")); "split distributes to a field list per subject"];
    .qunit.assertEquals[.regexp.replace_all[("a-b";"c-d");"-";"+"]; ("a+b";"c+d"); "replace_all distributes over a list of strings"];
    .qunit.assertEquals[.regexp.escape ("1.5";"2?"); ("1\\.5";"2\\?"); "escape distributes over a list of strings"]};

testDistributionOverDicts:{
    .qunit.assertEquals[.regexp.matches[`a`b!("x1";"yy");"[0-9]"]; `a`b!10b; "a dict keeps its keys and maps its values"];
    .qunit.assertEquals[.regexp.extract[`a`b!("x1";"y22");"[0-9]+"]; `a`b!(enlist"1";"22"); "extract keeps the dict keys"];
    .qunit.assertEquals[.regexp.replace_all[`x`y!("a-b";"c-d");"-";"+"]; `x`y!("a+b";"c+d"); "replace_all keeps the dict keys"]};

testTypedEmpties:{
    .qunit.assertEquals[.regexp.matches[0#`;"x"]; `boolean$(); "an empty collection answers a TYPED empty, never a bare ()"];
    .qunit.assertEquals[type .regexp.matches[0#`;"x"]; 1h; "that empty carries the boolean type"];
    .qunit.assertEquals[.regexp.extract[();"x"]; (); "an empty general list answers an empty general list"]};

/ accepting a shape is a one-way door and rejecting is reversible, so anything
/ not certainly text is 'type
testRejectsNonTextSubject:{
    m:.regexp.matches[;"a"];
    .qunit.assertThrows[m; (("a";"bb");("c";"dd")); "type"; "a list of string lists is not a subject"];
    .qunit.assertThrows[m; (1 2;3 4); "type"; "a list of int vectors is not a subject"];
    .qunit.assertThrows[m; ("ab";1); "type"; "one non-text element rejects the whole list"];
    .qunit.assertThrows[m; `a`b!(1;2); "type"; "a dict of non-text values is not a subject"];
    .qunit.assertThrows[m; ([] a:1 2); "type"; "a table is not a subject"]};

testRejectsUncompilablePattern:{
    .qunit.assertThrows[.regexp.matches["abc";]; "("; "regex"; "a pattern RE2 will not compile is 'regex"];
    .qunit.assertThrows[.regexp.replace["a-b";"-";]; "\\1"; "regex"; "a replacement that does not fit the pattern is 'regex"]};

/ THE COLLAPSE TRAP, which no guard can catch: equal-length char atoms collapse
/ into ONE string at PARSE time, so ("k";"p") is already "kp" before any function
/ sees it. Unequal lengths stay two subjects.
testCollapseTrap:{
    .qunit.assertEquals[type ("k";"p"); 10h; "two one-char atoms parse as one string"];
    .qunit.assertEquals[count ("k";"p"); 2; "and its count is the character count"];
    .qunit.assertEquals[.regexp.matches[("k";"p");"k"]; 1b; "so it arrives as a single subject"];
    .qunit.assertEquals[.regexp.matches[("k";"pp");"k"]; 10b; "unequal lengths stay two subjects"];
    .qunit.assertEquals[.regexp.matches[enlist each ("k";"p");"k"]; 10b; "enlist each is the way out"];
    .qunit.assertEquals[.regexp.matches[`k`p;"k"]; 10b; "symbols are the other way out"]};

/ split follows DUCKDB's algorithm exactly (verified against the v1.4.5
/ library): a zero-width match at the REMAINDER's start is not a delimiter — it
/ yields one character and moves on.
testZeroWidthSplit:{
    .qunit.assertEquals[.regexp.split["abc";""]; (enlist"a";enlist"b";enlist"c"); "an empty pattern splits into characters"];
    .qunit.assertEquals[.regexp.split["a";"a*"]; ("";""); "a full-subject zero-width-capable match leaves two empty fields"];
    .qunit.assertEquals[.regexp.split["ab";"a*"]; ("";enlist"b"); "the unmatched tail survives as one field"];
    .qunit.assertEquals[.regexp.split["aéb";""]; (enlist"a";"é";enlist"b"); "RE2 matches in UTF-8, so that advance never cuts a multi-byte character in half"];
    .qunit.assertKnown[.regexp.split["aXbXc";"X*"]; `zeroWidthSplit; "zero-width matches interleave with the characters they skip"]};

/ extract_all keeps EVERY zero-width match, which is also DuckDB's answer — the
/ two functions differ here and both are deliberate
testZeroWidthExtractAll:{
    .qunit.assertEquals[.regexp.extract_all["a";"a*"]; (enlist"a";""); "the zero-width match past the end is kept"];
    .qunit.assertEquals[.regexp.extract_all["ab";"a*"]; (enlist"a";"";""); "one zero-width match per remaining position"]};
