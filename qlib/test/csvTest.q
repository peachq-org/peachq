/ .csv conformance: the incremental CSV reader (.csv.read / .csv.info).  Each test writes its own fixture
/ inline with Save Text (`0:`) — the bytes sit next to the assertion that reads them — and only the one
/ genuinely shared fixture (BASIC) lives in setUp.  Files land in the harness's wiped working directory, so
/ there is no teardown.  The embedded-newline rows are THE architecture pin: an RFC-4180 quoted field
/ containing "\n" must load as one row — including when the quote straddles a read-chunk boundary (forced
/ tiny via the buffer_size option).  Errors are asserted by CLASS through assertThrows, never a stack trace.
system "d .csvTest";

BASIC:`:csvTestBasic.csv;
BASICTBL:([]a:1 2 3;b:(enlist "x";enlist "y";enlist "x");c:1.5 2.5 3.5);
NOOPT:()!();

setUpBasic:{.csvTest.BASIC 0: ("a,b,c";"1,x,1.5";"2,y,2.5";"3,x,3.5")};

/ ---- target forms -----------------------------------------------------------

testAccumulateReturnsTable:{
    t:.csv.read[.csvTest.BASIC;::;::;.csvTest.NOOPT];
    .qunit.assertEquals[t; .csvTest.BASICTBL; "longs, floats — and text is ALWAYS a string column, never sym"]};

testTargetGlobalCreatesThenInserts:{
    / the fixture global is deleted from its namespace: a test lambda carries the .csvTest context,
    / so a bare root name would re-root under it and a root delete would miss what the insert made
    @[{![`.csvTest;();0b;enlist x]};`G;::];
    s:.csv.read[.csvTest.BASIC;`.csvTest.G;::;.csvTest.NOOPT];
    .qunit.assertEquals[count .csvTest.G; 3; "first load creates the global"];
    s2:.csv.read[.csvTest.BASIC;`.csvTest.G;::;.csvTest.NOOPT];
    .qunit.assertEquals[count .csvTest.G; 6; "second load inserts into it"];
    .qunit.assertEquals[s2`rows; 3; "the summary counts this call's rows only"]};

testTargetLambdaReceivesBatches:{
    .csvTest.Batches::();
    s:.csv.read[.csvTest.BASIC;{[tblData;errData;misc] .csvTest.Batches,:enlist (tblData;errData;misc)};::;.csvTest.NOOPT];
    .qunit.assertEquals[count .csvTest.Batches; s`chunks; "the lambda fires once per batch"];
    .qunit.assertEquals[sum {count x 0} each .csvTest.Batches; 3; "the batch tables carry every row"];
    .qunit.assertEquals[.csvTest.Batches[0;1]; (); "errData is empty for now"];
    .qunit.assertEquals[.csvTest.Batches[0;2]; (::); "misc is generic null for now"]};

testTargetLambdaArityMismatch:{
    .qunit.assertThrows[.csv.read[.csvTest.BASIC;;::;.csvTest.NOOPT]; {[a] a}; "rank*"; "a rank-1 lambda is refused"];
    .qunit.assertThrows[.csv.read[.csvTest.BASIC;;::;.csvTest.NOOPT]; {[a;b] a}; "rank*"; "a rank-2 lambda is refused"]};

testTargetJunkIsRefused:{
    .qunit.assertThrows[.csv.read[.csvTest.BASIC;;::;.csvTest.NOOPT]; 42; "type*"; "a long is no target"]};

/ ---- types argument ---------------------------------------------------------

testTypesDictOverride:{
    t:.csv.read[.csvTest.BASIC;::;(enlist `a)!enlist "s";.csvTest.NOOPT];
    .qunit.assertEquals[t; ([]a:`1`2`3;b:(enlist "x";enlist "y";enlist "x");c:1.5 2.5 3.5); "the override wins; untouched columns keep their sniff"]};

/ the dict form reaches the SAME skip machinery as the string form's " " — a drop travels by name
testTypesDictDropsByName:{
    t:.csv.read[.csvTest.BASIC;::;(enlist `b)!enlist " ";.csvTest.NOOPT];
    .qunit.assertEquals[t; ([]a:1 2 3;c:1.5 2.5 3.5); "a \" \" dict value drops that column by name"];
    u:.csv.read[.csvTest.BASIC;::;`a`b!"s ";.csvTest.NOOPT];
    .qunit.assertEquals[u; ([]a:`1`2`3;c:1.5 2.5 3.5); "drop and override mix in one dict"]};

testTypesDictUnknownColumn:{
    .qunit.assertThrows[.csv.read[.csvTest.BASIC;::;;.csvTest.NOOPT]; (enlist `nope)!enlist "s"; "domain*";
        "an override naming no column is refused"]};

testTypesFullString:{
    t:.csv.read[.csvTest.BASIC;::;"s f";.csvTest.NOOPT];
    .qunit.assertEquals[t; ([]a:`1`2`3;c:1.5 2.5 3.5); "explicit symbol and float; the blank char skips b"]};

testTypeStringSkipChar:{
    t:.csv.read[.csvTest.BASIC;::;"j f";.csvTest.NOOPT];
    .qunit.assertEquals[t; ([]a:1 2 3;c:1.5 2.5 3.5); "the \" \" schema char drops the middle column entirely"]};

testTypesFullStringWrongLength:{
    .qunit.assertThrows[.csv.read[.csvTest.BASIC;::;;.csvTest.NOOPT]; "sf"; "length*";
        "a full schema must cover every column"]};

testTypesStarKeepsStrings:{
    t:.csv.read[.csvTest.BASIC;::;(enlist `b)!enlist "*";.csvTest.NOOPT];
    .qunit.assertEquals[t`b; (enlist "x";enlist "y";enlist "x"); "star keeps the raw strings"]};

/ ---- options ----------------------------------------------------------------

testDelimOption:{
    f:`:csvTestSemi.csv 0: ("a;b";"1;x";"2;y");
    t:.csv.read[f;::;::;(enlist `delim)!enlist ";"];
    .qunit.assertEquals[t; ([]a:1 2;b:(enlist "x";enlist "y")); "a semicolon file loads and parses under delim"]};

testHeaderOption:{
    f:`:csvTestBare.csv 0: ("10,20";"30,40");
    t:.csv.read[f;::;::;(enlist `header)!enlist 0b];
    .qunit.assertEquals[t; ([]x:10 30;x1:20 40); "header 0b: row one is data and the columns get generated names"];
    t2:.csv.read[.csvTest.BASIC;::;::;(enlist `header)!enlist 1b];
    .qunit.assertEquals[t2; .csvTest.BASICTBL; "header 1b forces the header row"]};

testPaddingOptionKeyIsIgnored:{
    f:`:csvTestSemi.csv 0: ("a;b";"1;x";"2;y");
    t:.csv.read[f;::;::;``delim!(::;";")];
    .qunit.assertEquals[t; ([]a:1 2;b:(enlist "x";enlist "y")); "the empty-sym key is padding, so the short-dict idiom works"]};

testUnknownOptionSignals:{
    .qunit.assertThrows[.csv.read[.csvTest.BASIC;::;::;]; (enlist `nonsense)!enlist 1; "option*";
        "an unknown option is never silently ignored"]};

testRecognizedUnimplementedOptionSignals:{
    .qunit.assertThrows[.csv.read[.csvTest.BASIC;::;::;]; (enlist `skip)!enlist 2; "option*"; "skip is not yet implemented"];
    .qunit.assertThrows[.csv.read[.csvTest.BASIC;::;::;]; (enlist `comment)!enlist "#"; "option*"; "comment likewise"];
    .qunit.assertThrows[.csv.read[.csvTest.BASIC;::;::;]; (enlist `nullstr)!enlist "NA"; "option*"; "nullstr likewise"];
    .qunit.assertThrows[.csv.read[.csvTest.BASIC;::;::;]; (enlist `all_varchar)!enlist 1b; "option*"; "all_varchar likewise"];
    .qunit.assertThrows[.csv.read[.csvTest.BASIC;::;::;]; (enlist `store_rejects)!enlist 1b; "option*"; "store_rejects likewise"];
    .qunit.assertThrows[.csv.read[.csvTest.BASIC;::;::;]; (enlist `rejects_table)!enlist `r; "option*"; "rejects_table likewise"]};

testDegenerateDelimRefused:{
    .qunit.assertThrows[.csv.read[.csvTest.BASIC;::;::;]; (enlist `delim)!enlist "\""; "domain*";
        "the quote char cannot delimit"];
    .qunit.assertThrows[.csv.read[.csvTest.BASIC;::;::;]; (enlist `delim)!enlist "\n"; "domain*";
        "a newline cannot delimit"]};

/ ---- the incremental-core pins ----------------------------------------------

/ one LOGICAL row: field a is the quoted "x\ny", field b is 2
testQuotedEmbeddedNewlineIsOneRow:{
    f:`:csvTestQuoted.csv 0: ("a,b";"\"x";"y\",2");
    t:.csv.read[f;::;"*j";.csvTest.NOOPT];
    .qunit.assertEquals[t; ([]a:enlist "x\ny";b:enlist 2); "the quoted newline does not split the row"]};

testQuoteSpansChunkBoundary:{
    f:`:csvTestQuoted.csv 0: ("a,b";"\"x";"y\",2");
    / a 4-byte read buffer forces the quoted field across many chunk carries
    t:.csv.read[f;::;"*j";`buffer_size`sample_size!(4;100)];
    .qunit.assertEquals[t; ([]a:enlist "x\ny";b:enlist 2); "the carry reassembles the row byte-identically"]};

testTypeFreezeAbortsOnBadCell:{
    / long is frozen from the 3-row sample, then a cell the frozen type cannot parse
    f:`:csvTestBadCell.csv 0: ("a";"1";"2";"3";"oops");
    .qunit.assertThrows[.csv.read[f;::;::;]; (enlist `sample_size)!enlist 3; "csv*"; "a cell failing its frozen type aborts, never a silent null"]};

testRaggedRowAborts:{
    f:`:csvTestRagged.csv 0: ("a,b";"1,2";"3");
    .qunit.assertThrows[.csv.read[;::;::;.csvTest.NOOPT]; f; "csv*"; "a short row aborts"]};

testUnterminatedQuoteAborts:{
    f:`:csvTestOpenQuote.csv 0: ("a,b";"\"x,2");
    .qunit.assertThrows[.csv.read[;::;::;.csvTest.NOOPT]; f; "csv*"; "an unclosed quote aborts"]};

/ detect and parse share one validator per shape: whatever the sniffer types, its own parser must accept
testNearMissShapesSniffAsText:{
    / near-misses of float, date and time — the sniffer must call every one text
    f:`:csvTestBadShape.csv 0: ("a,b,c";"1e,2026-02-31,12:99:00";"2e+,2026-02-30,25:00:00");
    t:.csv.read[f;::;::;.csvTest.NOOPT];
    .qunit.assertEquals[t; flip `a`b`c!(("1e";"2e+");("2026-02-31";"2026-02-30");("12:99:00";"25:00:00")); "a digit-less exponent, an impossible date and an out-of-range time are all text"];
    .qunit.assertThrows[.csv.read[f;::;;.csvTest.NOOPT]; (enlist `b)!enlist "d"; "csv*"; "and forcing the date type aborts rather than normalizing"]};

testHeaderOnlyFileIsEmptyTable:{
    f:`:csvTestHdrOnly.csv 0: enlist "a,b";
    t:.csv.read[f;::;::;.csvTest.NOOPT];
    .qunit.assertEquals[cols t; `a`b; "the header still names the columns"];
    .qunit.assertEquals[count t; 0; "and there are no rows"]};

testEmptyFileAborts:{
    f:`:csvTestEmpty.csv 0: ();
    .qunit.assertThrows[.csv.read[;::;::;.csvTest.NOOPT]; f; "csv*"; "an empty file has no schema"]};

testCrlfRows:{
    f:`:csvTestCrlf.csv 0: ("a,b\r";"1,2\r");
    t:.csv.read[f;::;::;.csvTest.NOOPT];
    .qunit.assertEquals[t; ([]a:enlist 1;b:enlist 2); "CRLF terminates rows and leaves no stray carriage return"]};

testCrOnlyRows:{
    f:`:csvTestCrOnly.csv 0: enlist "a,b\r1,2\r3,4";
    t:.csv.read[f;::;::;.csvTest.NOOPT];
    .qunit.assertEquals[t; ([]a:1 3;b:2 4); "a bare CR terminates rows too — old-Mac endings are not one row"]};

/ a textual header cell over a NUMERIC column marks the header even when other header cells look numeric
testPartlyNumericHeader:{
    f:`:csvTestNumHdr.csv 0: ("x,10";"1,2";"3,4");
    t:.csv.read[f;::;::;.csvTest.NOOPT];
    .qunit.assertEquals[t; flip (`$("x";"10"))!(1 3;2 4); "x-over-longs demotes, so row one is the header"]};

testLongOverflowSniffsAsFloat:{
    f:`:csvTestBigInt.csv 0: ("n";"10000000000000000000");
    t:.csv.read[f;::;::;.csvTest.NOOPT];
    .qunit.assertEquals[t; ([]n:enlist 1e19); "past the long domain the column reads as float — never a silent overflow"]};

/ the DEFAULT null is the EMPTY field only (DuckDB's default): NA/NULL/... are ordinary text until the
/ nullstr option lands, so under sniffing they make the column text, and under a frozen type they abort
testEmptyFieldIsTheOnlyNull:{
    f:`:csvTestNulls.csv 0: ("a,b,c";"1,,x";"2,5,NA");
    t:.csv.read[f;::;::;.csvTest.NOOPT];
    .qunit.assertEquals[t; ([]a:1 2;b:0N 5;c:(enlist "x";"NA")); "empty is null; NA is a value that turns the column text"];
    g:`:csvTestNullsFrozen.csv 0: ("a";"1";"NA");
    .qunit.assertThrows[.csv.read[;::;"j";.csvTest.NOOPT]; g; "csv*"; "NA under a frozen long is a bad cell, not a null"]};

/ ---- the target-schema law: types > target meta > sniff ----------------------

/ (i) the falsifying row for sniff-vs-table nondeterminism: bare cells that would sniff long parse as
/ float because the EXISTING target table says float
testTargetSchemaOutranksSniff:{
    @[{![`.csvTest;();0b;enlist x]};`T1;::];
    .csvTest.T1::([]a:enlist 1.5;b:enlist "old");
    f:`:csvTestTgtFloat.csv 0: ("a,b";"2,new";"3,txt");
    s:.csv.read[f;`.csvTest.T1;::;.csvTest.NOOPT];
    .qunit.assertEquals[.csvTest.T1; ([]a:1.5 2 3;b:("old";"new";"txt")); "bare-long cells parse under the target's float"];
    .qunit.assertEquals[s`ignored; 0#`x; "nothing was projected away"]};

/ (ii) column names, not positions, bind the CSV to the target
testTargetReorderedColumnsLoadByName:{
    @[{![`.csvTest;();0b;enlist x]};`T2;::];
    .csvTest.T2::([]a:enlist 1;b:enlist `x);
    f:`:csvTestTgtReorder.csv 0: ("b,a";"y,2";"z,3");
    .csv.read[f;`.csvTest.T2;::;.csvTest.NOOPT];
    .qunit.assertEquals[.csvTest.T2; ([]a:1 2 3;b:`x`y`z); "reordered CSV columns land by name — and as syms, per the target"]};

/ (iii) the subsetting rider: a CSV column the target lacks is never parsed, never materialized
testTargetExtraColumnIgnored:{
    @[{![`.csvTest;();0b;enlist x]};`T3;::];
    .csvTest.T3::([]a:enlist 1;b:enlist 1.5);
    f:`:csvTestTgtExtra.csv 0: ("a,junk,b";"2,zzz,2.5");
    s:.csv.read[f;`.csvTest.T3;::;.csvTest.NOOPT];
    .qunit.assertEquals[.csvTest.T3; ([]a:1 2;b:1.5 2.5); "the extra CSV column never lands"];
    .qunit.assertEquals[s`ignored; enlist `junk; "and the summary names it"]};

testTargetKeyedTableUpserts:{
    @[{![`.csvTest;();0b;enlist x]};`K1;::];
    .csvTest.K1::([k:`p`q] v:1 2);
    f:`:csvTestTgtKeyed.csv 0: ("k,v";"q,20";"r,30");
    .csv.read[f;`.csvTest.K1;::;.csvTest.NOOPT];
    .qunit.assertEquals[.csvTest.K1; ([k:`p`q`r] v:1 20 30); "a keyed target routes through upsert — the key row updates"]};

/ positional means position -> target COLUMN: the derived map carries the target's names too, so the
/ name-matching insert seam still lands the batch
testTargetHeaderlessLoadsPositionally:{
    @[{![`.csvTest;();0b;enlist x]};`T6;::];
    .csvTest.T6::([]a:enlist 1.5;b:enlist `x);
    f:`:csvTestTgtNohdr.csv 0: ("2,y";"3,z");
    .csv.read[f;`.csvTest.T6;::;.csvTest.NOOPT];
    .qunit.assertEquals[.csvTest.T6; ([]a:1.5 2 3;b:`x`y`z); "a headerless file takes the target's names and types by position"]};

/ conformity beyond the subsetting rider is the insert seam's OWN contract — its class, not a csv one
testTargetMissingColumnIsInsertsError:{
    @[{![`.csvTest;();0b;enlist x]};`T4;::];
    .csvTest.T4::([]a:enlist 1;b:enlist 2);
    f:`:csvTestTgtMissing.csv 0: (enlist "a";enlist "5");
    .qunit.assertThrows[.csv.read[;`.csvTest.T4;::;.csvTest.NOOPT]; f; "mismatch*"; "a missing column is insert's own 'mismatch"]};

/ explicit types outrank the target only by AGREEING with it — a conflict is 'mismatch before any parse
testTargetTypesConflictIsMismatch:{
    @[{![`.csvTest;();0b;enlist x]};`T5;::];
    .csvTest.T5::([]a:enlist 1.5);
    f:`:csvTestTgtConflict.csv 0: (enlist "a";enlist "2");
    .qunit.assertThrows[.csv.read[;`.csvTest.T5;(enlist `a)!enlist "j";.csvTest.NOOPT]; f; "mismatch*";
        "an explicit type conflicting with the target's schema is 'mismatch"]};

/ ---- info -------------------------------------------------------------------

/ info ADVISES sym where cardinality suggests it (csvguess-style); read never syms on its own, so
/ adopting the advice is exactly one explicit step: feed the dict back as types
testInfoRoundTrip:{
    d:.csv.info[.csvTest.BASIC;.csvTest.NOOPT];
    .qunit.assertEquals[d; `a`b`c!"jsf"; "info advises sym for the low-cardinality text column"];
    t:.csv.read[.csvTest.BASIC;::;d;.csvTest.NOOPT];
    .qunit.assertEquals[t; ([]a:1 2 3;b:`x`y`x;c:1.5 2.5 3.5); "feeding the advice back makes the syms explicit"]};

testSummaryDictShape:{
    s:.csv.read[.csvTest.BASIC;`.csvTest.G2;::;.csvTest.NOOPT];
    .qunit.assertEquals[key s; `rows`rejected`chunks`ignored`types; "the summary keys, in order"];
    .qunit.assertEquals[s`rows; 3; "rows counts the data rows"];
    .qunit.assertEquals[s`rejected; 0; "nothing is rejected yet"];
    .qunit.assertEquals[s`ignored; 0#`x; "no target schema, so nothing was projected away"];
    .qunit.assertEquals[s`types; `a`b`c!"j*f"; "types echoes the frozen schema — sniffed text is a string column"]};
