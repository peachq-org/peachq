/ .csv conformance: the incremental CSV reader (.csv.read / .csv.info).  Each test writes its own fixture
/ inline with Save Text (`0:`) - the bytes sit next to the assertion that reads them - and only the one
/ genuinely shared fixture (BASIC) lives in setUp.  Files land in the harness's wiped working directory, so
/ there is no teardown.  The embedded-newline rows are THE architecture pin: an RFC-4180 quoted field
/ containing "\n" must load as one row - including when the quote straddles a read-chunk boundary (forced
/ tiny via the buffer_size option).  Errors are asserted by CLASS through assertThrows, never a stack trace.
system "d .csvTest";

BASIC:`:csvTestBasic.csv;
BASICTBL:([]a:1 2 3;b:(enlist "x";enlist "y";enlist "x");c:1.5 2.5 3.5);
NOOPT:()!();

setUpBasic:{.csvTest.BASIC 0: ("a,b,c";"1,x,1.5";"2,y,2.5";"3,x,3.5")};

/ ---- target forms -----------------------------------------------------------

testAccumulateReturnsTable:{
    t:.csv.read[.csvTest.BASIC;::;::;.csvTest.NOOPT];
    .qunit.assertEquals[t; .csvTest.BASICTBL; "longs, floats - and text is ALWAYS a string column, never sym"]};

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
    .qunit.assertEquals[(cols .csvTest.Batches[0;1];count .csvTest.Batches[0;1]); (`line`column`error`csvLine;0);
        "errData is that batch's reject records - the empty table on a clean batch, schema stable"];
    .qunit.assertEquals[.csvTest.Batches[0;2]; `chunk`rows!0 3; "misc carries the chunk index and the rows so far"]};

testTargetLambdaArityMismatch:{
    .qunit.assertThrows[.csv.read[.csvTest.BASIC;;::;.csvTest.NOOPT]; {[a] a}; "rank*"; "a rank-1 lambda is refused"];
    .qunit.assertThrows[.csv.read[.csvTest.BASIC;;::;.csvTest.NOOPT]; {[a;b] a}; "rank*"; "a rank-2 lambda is refused"]};

testTargetJunkIsRefused:{
    .qunit.assertThrows[.csv.read[.csvTest.BASIC;;::;.csvTest.NOOPT]; 42; "type*"; "a long is no target"]};

/ ---- types argument ---------------------------------------------------------

testTypesDictOverride:{
    t:.csv.read[.csvTest.BASIC;::;(enlist `a)!enlist "s";.csvTest.NOOPT];
    .qunit.assertEquals[t; ([]a:`1`2`3;b:(enlist "x";enlist "y";enlist "x");c:1.5 2.5 3.5); "the override wins; untouched columns keep their sniff"]};

/ the dict form reaches the SAME skip machinery as the string form's " " - a drop travels by name
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
    .qunit.assertThrows[.csv.read[.csvTest.BASIC;::;::;]; (enlist `nullstr)!enlist "NA"; "option*"; "nullstr likewise"];
    .qunit.assertThrows[.csv.read[.csvTest.BASIC;::;::;]; (enlist `all_varchar)!enlist 1b; "option*"; "all_varchar likewise"]};

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
    / near-misses of float, date and time - the sniffer must call every one text
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
    .qunit.assertEquals[t; ([]a:1 3;b:2 4); "a bare CR terminates rows too - old-Mac endings are not one row"]};

/ a textual header cell over a NUMERIC column marks the header even when other header cells look numeric
testPartlyNumericHeader:{
    f:`:csvTestNumHdr.csv 0: ("x,10";"1,2";"3,4");
    t:.csv.read[f;::;::;.csvTest.NOOPT];
    .qunit.assertEquals[t; flip (`$("x";"10"))!(1 3;2 4); "x-over-longs demotes, so row one is the header"]};

testLongOverflowSniffsAsFloat:{
    f:`:csvTestBigInt.csv 0: ("n";"10000000000000000000");
    t:.csv.read[f;::;::;.csvTest.NOOPT];
    .qunit.assertEquals[t; ([]n:enlist 1e19); "past the long domain the column reads as float - never a silent overflow"]};

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
    .qunit.assertEquals[.csvTest.T2; ([]a:1 2 3;b:`x`y`z); "reordered CSV columns land by name - and as syms, per the target"]};

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
    .qunit.assertEquals[.csvTest.K1; ([k:`p`q`r] v:1 20 30); "a keyed target routes through upsert - the key row updates"]};

/ positional means position -> target COLUMN: the derived map carries the target's names too, so the
/ name-matching insert seam still lands the batch
testTargetHeaderlessLoadsPositionally:{
    @[{![`.csvTest;();0b;enlist x]};`T6;::];
    .csvTest.T6::([]a:enlist 1.5;b:enlist `x);
    f:`:csvTestTgtNohdr.csv 0: ("2,y";"3,z");
    .csv.read[f;`.csvTest.T6;::;.csvTest.NOOPT];
    .qunit.assertEquals[.csvTest.T6; ([]a:1.5 2 3;b:`x`y`z); "a headerless file takes the target's names and types by position"]};

/ conformity beyond the subsetting rider is the insert seam's OWN contract - its class, not a csv one
testTargetMissingColumnIsInsertsError:{
    @[{![`.csvTest;();0b;enlist x]};`T4;::];
    .csvTest.T4::([]a:enlist 1;b:enlist 2);
    f:`:csvTestTgtMissing.csv 0: (enlist "a";enlist "5");
    .qunit.assertThrows[.csv.read[;`.csvTest.T4;::;.csvTest.NOOPT]; f; "mismatch*"; "a missing column is insert's own 'mismatch"]};

/ explicit types outrank the target only by AGREEING with it - a conflict is 'mismatch before any parse
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
    .qunit.assertEquals[s`types; `a`b`c!"j*f"; "types echoes the frozen schema - sniffed text is a string column"]};

/ ---- ported type-law pins (read-side rows of the deleted rayfall csv suites, csv-read PR-2) -----

testPromoteDateTimestamp:{
    f:`:csvTestMixTs.csv 0: ("ts";"2000-01-01";"2001-01-01T12:00:00";"2002-06-15T08:30:45");
    t:.csv.read[f;::;::;.csvTest.NOOPT];
    .qunit.assertEquals[t; ([]ts:(2000.01.01D00:00:00.000000000;2001.01.01D12:00:00.000000000;2002.06.15D08:30:45.000000000));
        "DATE+TIMESTAMP widens to timestamp; dates gain a midnight time part"]};

testPromoteIncompatibleMixIsText:{
    f:`:csvTestDateInt.csv 0: ("x";"2000-01-01";"42";"2001-06-15");
    t:.csv.read[f;::;::;.csvTest.NOOPT];
    .qunit.assertEquals[t; ([]x:("2000-01-01";"42";"2001-06-15")); "DATE+long is incompatible, so the column stays text - a string column"]};

/ the lattice freezes BOOL+long at long (and BOOL+float at float) - .csv.info pins the freeze; the
/ read then aborts because "true" cannot parse as the frozen type and the empty field is the only null
testPromoteBoolNumericFreezesNumeric:{
    f:`:csvTestBoolInt.csv 0: ("a";"true";"1";"0";"false");
    .qunit.assertEquals[.csv.info[f;.csvTest.NOOPT]; (enlist `a)!enlist "j"; "BOOL+long freezes long"];
    .qunit.assertThrows[.csv.read[;::;::;.csvTest.NOOPT]; f; "csv*"; "and the bool cell then aborts"];
    g:`:csvTestBoolFlt.csv 0: ("a";"true";"1.5";"false");
    .qunit.assertEquals[.csv.info[g;.csvTest.NOOPT]; (enlist `a)!enlist "f"; "BOOL+float freezes float"];
    .qunit.assertThrows[.csv.read[;::;::;.csvTest.NOOPT]; g; "csv*"; "same abort under float"]};

testNanInfLiterals:{
    f:`:csvTestNanInf.csv 0: ("a";"NaN";"Inf";"+Inf";"-Inf";"nan";"inf";"1.5");
    t:.csv.read[f;::;::;.csvTest.NOOPT];
    .qunit.assertEquals[t; ([]a:0n 0w 0w -0w 0n 0w 1.5);
        "NaN and Inf are float VALUES (live-infinity model): NaN is null, the infinities live"]};

/ the rfl suite's rich sentinel set is re-pinned to the owner ruling: the empty field is the ONLY
/ default null, so every spelling in the old vocabulary is ordinary text until nullstr lands
testNullVocabularyIsTextByDefault:{
    f:`:csvTestSentinels.csv 0: ("a,b";"N/A,1";"n/a,2";"NA,3";"na,4";"null,5";"NULL,6";"None,7";"none,8";".,9";"1,10");
    t:.csv.read[f;::;::;.csvTest.NOOPT];
    .qunit.assertEquals[t; ([]a:("N/A";"n/a";"NA";"na";"null";"NULL";"None";"none";enlist ".";enlist "1");b:1+til 10);
        "every old sentinel spelling is a value that turns the column text; the numbers stay data"]};

testIntWidensToFloat:{
    f:`:csvTestWiden.csv 0: ("v";"1";"2";"3";"4.5");
    t:.csv.read[f;::;::;.csvTest.NOOPT];
    .qunit.assertEquals[t; ([]v:1 2 3 4.5); "long rows widen to float once a float appears, kept exactly"]};

testScientificNotation:{
    f:`:csvTestSci.csv 0: ("x";"1e+5";"2.5e-3";"1E-2";"3.14");
    t:.csv.read[f;::;::;.csvTest.NOOPT];
    .qunit.assertEquals[t; ([]x:100000 0.0025 0.01 3.14); "signed exponents and E parse as float"]};

testNumericJunkIsText:{
    f:`:csvTestJunk.csv 0: ("x";"12abc";"1.2.3";"-";"+";"e5";"42");
    t:.csv.read[f;::;::;.csvTest.NOOPT];
    .qunit.assertEquals[t; ([]x:("12abc";"1.2.3";enlist "-";enlist "+";"e5";"42"));
        "numeric near-misses drag the column to text - a string column"]};

testTimeFractionParses:{
    f:`:csvTestTimeFrac.csv 0: ("a";"10:30:45.123";"12:00:00.5";"08:15:30.12";"10:30:45.123456");
    t:.csv.read[f;::;::;.csvTest.NOOPT];
    .qunit.assertEquals[t; ([]a:(0D10:30:45.123000000;0D12:00:00.500000000;0D08:15:30.120000000;0D10:30:45.123456000));
        "1-3 digit fractions are time; a wider written fraction promotes the column to timespan, never truncation"]};

testTimestampFractionParses:{
    f:`:csvTestTsFrac.csv 0: ("ts";"2000-01-01T12:00:00.123456789";"2001-06-15 08:30:00.000000001";"2002-03-04T00:00:00.1");
    t:.csv.read[f;::;::;.csvTest.NOOPT];
    .qunit.assertEquals[t; ([]ts:2000.01.01D12:00:00.123456789 2001.06.15D08:30:00.000000001 2002.03.04D00:00:00.100000000);
        "nanosecond fractions parse exactly, short ones pad"]};

/ superseded by THE TZ POSTURE below: an explicit p never adopts an offset, so every offset SHAPE is a
/ frozen-type miss and only the offsetless cell parses.  %z is the door that reads one (see the tz block)
testTimestampTzUnderExplicitP:{
    {[s] f:`:csvTestTsTz.csv 0: ("ts";s);
        .qunit.assertThrows[.csv.read[;::;enlist "p";()!()]; f; "csv*"; "an offset under explicit p misses: ",s]}
        each ("2001-01-01 01:01:01+05:30";"2001-01-01 01:01:01+01:00";"2001-01-01 01:01:01Z";
        "2001-01-01 01:01:01+0530";"2001-01-01 06:01:01-05:00";"2001-01-01 01:01:01.500000000+01:00");
    g:`:csvTestTsPlain.csv 0: ("ts";"2001-01-01 01:01:01";"2001-01-01 01:01:01.500000000");
    .qunit.assertEquals[.csv.read[g;::;enlist "p";.csvTest.NOOPT];
        ([]ts:(2001.01.01D01:01:01;2001.01.01D01:01:01.500000000));
        "the offsetless forms, fraction included, parse verbatim under the same explicit p"]};

testSingleColumnFile:{
    f:`:csvTestOneCol.csv 0: ("only";"10";"20";"30");
    t:.csv.read[f;::;::;.csvTest.NOOPT];
    .qunit.assertEquals[t; ([]only:10 20 30); "a file with no delimiter is one column, typed normally"]};

testNoTrailingNewline:{
    f:`:csvTestNoNl.csv 1: "a,b\n1,2\n3,4";
    t:.csv.read[f;::;::;.csvTest.NOOPT];
    .qunit.assertEquals[t; ([]a:1 3;b:2 4); "the last row needs no trailing newline and parses whole"]};

testTsvDelimOption:{
    f:`:csvTestTsv.csv 0: ("a\tb";"1\t2";"3\t4");
    t:.csv.read[f;::;::;(enlist `delim)!enlist "\t"];
    .qunit.assertEquals[t; ([]a:1 3;b:2 4); "a tab delim option reads TSV with typed cells"]};

testTsvAutoDetect:{
    f:`:csvTestTsv.csv 0: ("a\tb";"1\t2";"3\t4");
    t:.csv.read[f;::;::;.csvTest.NOOPT];
    .qunit.assertEquals[cols t; `a`b; "a TSV file splits without an explicit delim"]};

/ ---- THE DELIMITER-SNIFF LAW: among , ; tab | a candidate qualifies only if EVERY sample row parses
/ to the same field count above one column (none malformed); most columns wins, ties keep candidate
/ order (comma first), none qualifying keeps comma.  Structure only - data statistics play no part.
testDelimSniffCandidates:{
    f:`:csvTestSniffSemi.csv 0: ("a;b";"1;x";"2;y");
    .qunit.assertEquals[.csv.read[f;::;::;.csvTest.NOOPT]; ([]a:1 2;b:(enlist "x";enlist "y")); "semicolon sniffs"];
    g:`:csvTestSniffPipe.csv 0: ("a|b|c";"1|2|3";"4|5|6");
    .qunit.assertEquals[.csv.read[g;::;::;.csvTest.NOOPT]; ([]a:1 4;b:2 5;c:3 6); "pipe sniffs"]};

testDelimSniffCommaWinsTies:{
    f:`:csvTestSniffTie.csv 0: ("a,b;c";"1,2;3";"4,5;6");
    .qunit.assertEquals[cols .csv.read[f;::;::;.csvTest.NOOPT]; `a,`$"b;c"; "at equal column counts comma outranks semicolon"]};

testDelimSniffMostColumnsWins:{
    f:`:csvTestSniffCols.csv 0: ("a;x,y;b";"1;p,q;2";"3;r,s;4");
    .qunit.assertEquals[count cols .csv.read[f;::;::;.csvTest.NOOPT]; 3; "three consistent semicolon columns beat two comma ones"]};

testDelimSniffInconsistentKeepsComma:{
    f:`:csvTestSniffRag.csv 0: ("a;b;c";"1;2";"3;4");
    t:.csv.read[f;::;::;.csvTest.NOOPT];
    .qunit.assertEquals[count cols t; 1; "a ragged semicolon layout disqualifies the candidate - one comma column"]};

testBomStripped:{
    f:`:csvTestBom.csv 1: "\357\273\277a,b\n1,2\n";
    .qunit.assertEquals[.csv.read[f;::;::;.csvTest.NOOPT]; ([]a:enlist 1;b:enlist 2);
        "a leading UTF-8 BOM is stripped before the header parses"];
    .qunit.assertEquals[.csv.read[f;::;::;(enlist `buffer_size)!enlist 1]; ([]a:enlist 1;b:enlist 2);
        "and never straddles a read buffer smaller than itself"]};

testBoolCaseInsensitive:{
    f:`:csvTestBoolCase.csv 0: ("f";"True";"False";"tRUe";"FALSE");
    .qunit.assertEquals[.csv.read[f;::;::;.csvTest.NOOPT]; ([]f:1010b);
        "true/false sniff as bool in any case - the B domain is case-insensitive"]};

testLeadingZeroIntegerStaysText:{
    f:`:csvTestLz.csv 0: ("id,n";"007,7";"042,42";"9,9");
    .qunit.assertEquals[.csv.read[f;::;::;.csvTest.NOOPT]; ([]id:("007";"042";enlist "9");n:7 42 9);
        "a leading-zero digit run is an identifier, not a number; plain digits stay long"];
    .qunit.assertEquals[.csv.read[f;::;`id`n!"jj";.csvTest.NOOPT]`id; 7 42 9;
        "an explicit j still parses them - the law is sniff-side only"]};

testBlankHeaderCells:{
    f:`:csvTestBlankHdr.csv 0: ("a,  ,c";"1,2,3";"4,5,6");
    .qunit.assertEquals[cols .csv.read[f;::;::;.csvTest.NOOPT]; `a`x1`c;
        "header names trim; a blank header cell gets the generated name for its position"];
    g:`:csvTestEmptyHdr.csv 0: (",,";"1,x,2.5";"3,y,4.5");
    t:.csv.read[g;::;::;.csvTest.NOOPT];
    .qunit.assertEquals[(cols t;count t); (`x`x1`x2;2);
        "an all-blank first row above typed data is a header of generated names, not a data row"]};

/ ---- THE DATEFORMAT LAW: day-first/month-first forms are AMBIGUOUS as forms, so the sniffer never
/ types them - text without an explicit dateformat/timestampformat (DuckDB strptime vocabulary,
/ implemented subset %Y %y %m %d %H %M %S %f %%); a format REPLACES the grammar for its type
testDateformatOption:{
    f:`:csvTestDfmt.csv 0: ("d";"13/10/2021";"04/10/2021");
    .qunit.assertEquals[.csv.read[f;::;::;.csvTest.NOOPT]; ([]d:("13/10/2021";"04/10/2021"));
        "a day-first form stays text - the sniffer never guesses day against month"];
    .qunit.assertEquals[.csv.read[f;::;::;(enlist `dateformat)!enlist "%d/%m/%Y"]; ([]d:2021.10.13 2021.10.04);
        "the explicit dateformat parses it as dates"]};

testTimestampformatOption:{
    f:`:csvTestTsfmt.csv 0: ("ts";"01-01-90 12:12:00";"02-02-02 14:15:30");
    t:.csv.read[f;::;::;(enlist `timestampformat)!enlist "%d-%m-%y %H:%M:%S"];
    .qunit.assertEquals[t; ([]ts:(1990.01.01D12:12:00;2002.02.02D14:15:30));
        "timestampformat parses, %y on the POSIX 68/69 century pivot"]};

testDateformatReplacesGrammar:{
    f:`:csvTestDfmtRep.csv 0: ("d";"2026-08-22");
    .qunit.assertEquals[.csv.read[f;::;::;(enlist `dateformat)!enlist "%d/%m/%Y"]; ([]d:enlist "2026-08-22");
        "a format REPLACES the default date grammar rather than extending it"]};

testDateformatBadCellAborts:{
    f:`:csvTestDfmtBad.csv 0: ("d";"13/10/2021";"14/10/2021";"2021-10-15");
    .qunit.assertThrows[.csv.read[f;::;enlist "d";]; (enlist `dateformat)!enlist "%d/%m/%Y"; "csv*";
        "a cell outside the format under a frozen d aborts, never a silent null"]};

testDateformatYearIsFourDigits:{
    f:`:csvTestDfmtY4.csv 0: ("d";"1112020");
    .qunit.assertEquals[.csv.read[f;::;::;(enlist `dateformat)!enlist "%d%m%Y"]; ([]d:enlist 1112020);
        "%Y is exactly four digits: an ambiguous packed cell falls back to long, never a wrong-century date"]};

testDateformatUnknownSpecifierSignals:{
    .qunit.assertThrows[.csv.read[.csvTest.BASIC;::;::;]; (enlist `dateformat)!enlist "%q"; "option*";
        "a specifier outside the implemented subset signals"];
    .qunit.assertThrows[.csv.read[.csvTest.BASIC;::;::;]; (enlist `dateformat)!enlist "%d/%m/%Y %H:%M"; "option*";
        "clock specifiers belong to timestampformat, not dateformat"];
    .qunit.assertThrows[.csv.read[.csvTest.BASIC;::;::;]; (enlist `dateformat)!enlist "%m/%d"; "option*";
        "a dateless format (no year) is refused"]};

/ ---- THE TZ POSTURE (owner ruling 2026-08-23): kdb parses no timezone, so a tz-suffixed cell is TEXT at
/ defaults.  An offset is read only where the caller asks for one - %z in timestampformat, DuckDB's own
/ semantics: apply the row's own numeric offset and store UTC.  Zone NAMES need tz data and are not here.
testTzSuffixStaysTextAtDefaults:{
    f:`:csvTestTz.csv 0: ("a";"2026-08-22T12:34:56+02:00";"2026-08-23T00:30:00Z");
    t:.csv.read[f;::;::;.csvTest.NOOPT];
    .qunit.assertEquals[t; ([]a:("2026-08-22T12:34:56+02:00";"2026-08-23T00:30:00Z"));
        "a tz-suffixed timestamp is text at defaults - the bytes survive verbatim, no silent shift"]};

testTimestampformatOffset:{
    f:`:csvTestTzFmt.csv 0: ("a";"2026-08-22T12:34:56+02:00";"2026-08-23T00:30:00+02:00");
    t:.csv.read[f;::;::;(enlist `timestampformat)!enlist "%Y-%m-%dT%H:%M:%S%z"];
    .qunit.assertEquals[t; ([]a:(2026.08.22D10:34:56;2026.08.22D22:30:00));
        "%z applies the row's own offset and stores UTC, carrying the date back across midnight"]};

testTimestampformatOffsetShapes:{
    f:`:csvTestTzSh.csv 0: ("a";"2026-08-22T12:34:56Z";"2026-08-22T12:34:56-0530";"2026-08-22T12:34:56+00:00");
    t:.csv.read[f;::;::;(enlist `timestampformat)!enlist "%Y-%m-%dT%H:%M:%S%z"];
    .qunit.assertEquals[t; ([]a:(2026.08.22D12:34:56;2026.08.22D18:04:56;2026.08.22D12:34:56));
        "Z, +-HHMM and +-HH:MM are the offset shapes; a fractionless format still needs the whole cell"];
    g:`:csvTestTzFr.csv 0: ("a";"2026-08-22T12:34:56.25+02:00");
    .qunit.assertEquals[.csv.read[g;::;::;(enlist `timestampformat)!enlist "%Y-%m-%dT%H:%M:%S.%f%z"];
        ([]a:enlist 2026.08.22D10:34:56.250000000); "%f composes with %z"]};

testTimestampformatOffsetIsTimestampOnly:{
    .qunit.assertThrows[.csv.read[.csvTest.BASIC;::;::;]; (enlist `dateformat)!enlist "%d/%m/%Y%z"; "option*";
        "an offset is a clock specifier: dateformat refuses it"];
    .qunit.assertThrows[.csv.read[.csvTest.BASIC;::;::;]; (enlist `timestampformat)!enlist "%Y-%m-%d %Z"; "option*";
        "zone NAMES need tz data we do not carry - %Z stays outside the implemented subset"]};

/ the miss law over a silent drop: DuckDB's forced-TIMESTAMP path discards the offset without a word
testTzUnderExplicitTimestampMisses:{
    f:`:csvTestTzP.csv 0: ("a";"2026-08-22T12:34:56+02:00");
    .qunit.assertThrows[.csv.read[f;::;;.csvTest.NOOPT]; (enlist `a)!enlist "p"; "csv*";
        "an unasked-for offset under a frozen p is a miss, never a silently dropped offset"];
    .csv.read[f;`.csvTest.GTZ;(enlist `a)!enlist "p";`ignore_errors`rejects_table!(1b;`.csvTest.RTZ)];
    .qunit.assertEquals[(.csvTest.RTZ`column;.csvTest.RTZ`error); (enlist `a;enlist `cast);
        "and under a continue-mode lever it leaves a cast reject naming the column"]};

testHeaderTextOverAllEmpty:{
    f:`:csvTestHdrEmpty.csv 0: ("8,note,tag";"0,,x";"1,,y");
    t:.csv.read[f;::;::;.csvTest.NOOPT];
    .qunit.assertEquals[(cols t;count t); (`$("8";"note";"tag");2);
        "a textual row-0 cell over an all-empty column is header evidence the data cannot produce"]};

testTypedCellsTolerateBlanks:{
    f:`:csvTestPad.csv 0: ("a,b";" 567,x";"12 ,y";"  ,z");
    .qunit.assertEquals[.csv.read[f;::;::;.csvTest.NOOPT]; ([]a:(" 567";"12 ";"  ");b:enlist each "xyz");
        "one blanks-only cell keeps the column text: blanks-only is a VALUE, never a null"];
    g:`:csvTestPad2.csv 0: ("a,b";" 567,x";"12 ,y");
    .qunit.assertEquals[.csv.read[g;::;::;.csvTest.NOOPT]; ([]a:567 12;b:enlist each "xy");
        "padded numerics sniff and parse as longs - kdb casts and DuckDB both read them"]};

testDuplicateHeaderDedupes:{
    f:`:csvTestDupHdr.csv 0: ("a,b,a";"1,2,3";"4,5,6");
    .qunit.assertEquals[.csv.read[f;::;::;.csvTest.NOOPT]; ([]a:1 4;b:2 5;a_1:3 6);
        "the later twin takes _1 - DuckDB's dedupe, never an abort, never a silent merge"];
    g:`:csvTestDupHdr3.csv 0: ("a,a,a_1";"1,2,3");
    .qunit.assertEquals[cols .csv.read[g;::;::;.csvTest.NOOPT]; `a`a_1`a_1_1;
        "each rename takes the first suffix free among the names already fixed"]};

testDelimSniffExplicitWins:{
    f:`:csvTestSniffExp.csv 0: ("a;b\tc";"1;2\t3";"4;5\t6");
    t:.csv.read[f;::;::;(enlist `delim)!enlist ";"];
    .qunit.assertEquals[count cols t; 2; "an explicit delim is never second-guessed by the sniffer"]};

testBoolExplicitForms:{
    f:`:csvTestBoolForms.csv 0: ("f";"true";"false";"1";"0";"TRUE";"FALSE");
    t:.csv.read[f;::;enlist "b";.csvTest.NOOPT];
    .qunit.assertEquals[t; ([]f:101010b); "true/false, 1/0 and TRUE/FALSE all parse under an explicit b"];
    f 0: ("f";"true";"maybe");
    .qunit.assertThrows[.csv.read[;::;enlist "b";.csvTest.NOOPT]; f; "csv*";
        "an unparseable bool cell aborts, never a silent 0b"]};

/ ---- THE TOLERANCE LAW (csv PR-3): options govern whether the load SURVIVES a bad row, never whether
/ it is counted.  Every bad row - skipped, padded or salvaged - leaves one reject record (line, column,
/ error class, raw text) and bumps the summary's `rejected` unconditionally.  Defaults are DuckDB's own
/ strict posture: strict_mode=1b, ignore_errors=0b, null_padding=0b, store_rejects=0b - so the default
/ behaviour is byte-identical to before this PR.

/ (i) the falsifying pair: the SAME ragged file pads under null_padding and still aborts under defaults
testNullPaddingPadsAndRecords:{
    f:`:csvTestNp.csv 0: ("a,b,c";"1,2,3";"4,5";"6,7,8");
    t:.csv.read[f;::;::;(enlist `null_padding)!enlist 1b];
    .qunit.assertEquals[t; ([]a:1 4 6;b:2 5 7;c:(3;0N;8)); "the short row pads with nulls, the rest is untouched"];
    .qunit.assertThrows[.csv.read[;::;::;()!()]; f; "csv*"; "the same file under empty opts still aborts - defaults unchanged"]};

testNullPaddingRecordsTheAudit:{
    f:`:csvTestNpRec.csv 0: ("a,b,c";"1,2,3";"4,5";"6,7,8");
    @[{![`.csvTest;();0b;enlist x]};`GNP;::];
    s:.csv.read[f;`.csvTest.GNP;::;`null_padding`rejects_table!(1b;`.csvTest.RNP)];
    .qunit.assertEquals[s`rejected; 1; "a padded row counts - tolerance never bypasses the accounting"];
    .qunit.assertEquals[s`rows; 3; "and the padded row still lands in the data"];
    r:.csvTest.RNP;
    .qunit.assertEquals[(r`line;r`error;r`csvLine); (enlist 3;enlist `padded;enlist "4,5");
        "the reject record is the audit trail: line, class, raw text"]};

testIgnoreErrorsSkipsAndCounts:{
    / one cast miss (the frozen-type-miss law under a continue-mode option) and one ragged row
    f:`:csvTestIg.csv 0: ("a,b";"1,x";"oops,y";"3");
    s:.csv.read[f;`.csvTest.GIG;"j*";(enlist `ignore_errors)!enlist 1b];
    .qunit.assertEquals[s`rows; 1; "only the good row lands"];
    .qunit.assertEquals[s`rejected; 2; "both bad rows count"];
    .qunit.assertEquals[count .csvTest.GIG; 1; "the target holds exactly the good row"]};

testRejectsTableShape:{
    f:`:csvTestRj.csv 0: ("a,b";"1,x";"oops,y";"3");
    @[{![`.csvTest;();0b;enlist x]};`GRJ;::];
    .csv.read[f;`.csvTest.GRJ;"j*";`ignore_errors`rejects_table!(1b;`.csvTest.RRJ)];
    r:.csvTest.RRJ;
    .qunit.assertEquals[cols r; `line`column`error`csvLine; "the record shape maps onto DuckDB's reject_errors"];
    .qunit.assertEquals[r`line; 3 4; "1-based physical line numbers, header included"];
    .qunit.assertEquals[r`column; `a`; "a cast miss names its column; a row-level fault has no column"];
    .qunit.assertEquals[r`error; `cast`toofewcolumns; "the error classes"];
    .qunit.assertEquals[r`csvLine; ("oops,y";enlist "3"); "the raw csv line rides along"]};

/ store_rejects implies continuing (DuckDB's semantics), and naming rejects_table implies store_rejects
testStoreRejectsImpliesContinuing:{
    f:`:csvTestSr.csv 0: ("a,b";"1,2";"3");
    t:.csv.read[f;::;::;(enlist `rejects_table)!enlist `.csvTest.MyRej];
    .qunit.assertEquals[t; ([]a:enlist 1;b:enlist 2); "storing rejects keeps the load alive past the bad row"];
    .qunit.assertEquals[(count .csvTest.MyRej;first .csvTest.MyRej`error); (1;`toofewcolumns);
        "rejects_table names the global and opts in by itself - a :: load's rejects live in the stored table"]};

/ the DEFAULT rejects table name is DuckDB's own: reject_errors, landing like any q assignment
testStoreRejectsDefaultName:{
    f:`:csvTestSrDef.csv 0: ("a,b";"1,2";"3");
    .csv.read[f;::;::;(enlist `store_rejects)!enlist 1b];
    r:value "reject_errors";
    .qunit.assertEquals[(count r;first r`error); (1;`toofewcolumns); "store_rejects=1b stores under `reject_errors"]};

/ the stored table is one load's audit: a pre-existing global under that name is replaced, never merged
testStoreRejectsReplacesTheGlobal:{
    .csvTest.OldRej::([]line:99 98;column:``;error:`x`y;csvLine:("";""));
    f:`:csvTestSrRep.csv 0: ("a,b";"1,2";"3");
    .csv.read[f;::;::;(enlist `rejects_table)!enlist `.csvTest.OldRej];
    .qunit.assertEquals[count .csvTest.OldRej; 1; "the fresh load's single reject replaces the stale table"];
    g:`:csvTestSrRep2.csv 0: ("a,b";"1,2";"3,4");
    .csv.read[g;::;::;(enlist `rejects_table)!enlist `.csvTest.OldRej];
    .qunit.assertEquals[count .csvTest.OldRej; 0; "a clean load stores the empty table - the audit says so"]};

/ THE LENIENT-QUOTE LAW (strict_mode=0b): a field whose quotes cannot parse under RFC-4180 is read as
/ LITERAL bytes to the next delimiter - a quote inside an unquoted field is a byte, an unterminated or
/ junk-tailed quote region reverts to its opening quote.  Every salvaged row is recorded and counted.
testStrictModeOffSalvagesQuotes:{
    f:`:csvTestSm.csv 1: "a,b\n1,he\"llo\n2,x\n";
    .qunit.assertThrows[.csv.read[;::;::;()!()]; f; "csv*"; "a stray quote aborts under defaults"];
    t:.csv.read[f;::;::;(enlist `strict_mode)!enlist 0b];
    .qunit.assertEquals[t; ([]a:1 2;b:("he\"llo";enlist "x")); "the stray quote is a literal byte under strict_mode=0b"]};

testStrictModeOffUnterminatedAtEof:{
    f:`:csvTestSmEof.csv 1: "a,b\n1,\"x\n";
    t:.csv.read[f;::;::;`strict_mode`rejects_table!(0b;`.csvTest.RSM)];
    .qunit.assertEquals[t; ([]a:enlist 1;b:enlist "\"x"); "an unterminated quote reverts to literal bytes, opening quote kept"];
    .qunit.assertEquals[(.csvTest.RSM`line;.csvTest.RSM`error); (enlist 2;enlist `unterminatedquote);
        "and the salvaged row is recorded"]};

testStrictModeOffKeepsProperQuotes:{
    f:`:csvTestSmOk.csv 1: "a,b\n\"x\ny\",1\n\"say \"\"hi\"\"\",2\n";
    t:.csv.read[f;::;"*j";(enlist `strict_mode)!enlist 0b];
    .qunit.assertEquals[t; ([]a:("x\ny";"say \"hi\"");b:1 2);
        "well-formed quoting - embedded newlines, doubled quotes - still parses quoted under 0b"];
    g:`:csvTestSmB0.csv 1: "\"x\ny\",1\n\"z\",2\n";
    .qunit.assertEquals[.csv.read[g;::;"*j";`strict_mode`header!(0b;0b)]; ([]x:("x\ny";enlist "z");x1:1 2);
        "byte 0 of the FILE is a field start: a leading quoted field keeps its newline (codex P1)"]};

/ the lenient scan suspends on a quote region the carry cannot yet resolve - a 1-byte read buffer
/ forces every suspend/resume seam, salvage and proper-quote alike
testStrictModeOffQuoteAtBufferBoundary:{
    f:`:csvTestSmBuf.csv 1: "a,b\n\"x\ny\",1\nhe\"llo,2\n";
    t:.csv.read[f;::;"*j";`strict_mode`buffer_size`sample_size!(0b;1;100)];
    .qunit.assertEquals[t; ([]a:("x\ny";"he\"llo");b:1 2);
        "quote resolution never depends on where a read chunk ends"];
    g:`:csvTestSmBuf2.csv 1: "a,b\n1,\"x\n";
    .qunit.assertEquals[.csv.read[g;::;::;`strict_mode`buffer_size!(0b;1)]; ([]a:enlist 1;b:enlist "\"x");
        "an unterminated quote fed byte-by-byte still resolves literal at EOF"]};

testLambdaErrDataAndMisc:{
    f:`:csvTestLam.csv 0: ("a,b";"1,2";"3";"4,5");
    .csvTest.LamCalls::();
    s:.csv.read[f;{[tblData;errData;misc] .csvTest.LamCalls,:enlist (tblData;errData;misc)};::;(enlist `null_padding)!enlist 1b];
    .qunit.assertEquals[s`rejected; 1; "the padded row counts in the lambda summary too"];
    ed:.csvTest.LamCalls[0;1];
    .qunit.assertEquals[cols ed; `line`column`error`csvLine; "errData is that batch's reject records as a table"];
    .qunit.assertEquals[ed`error; enlist `padded; "carrying the padded row"];
    m:.csvTest.LamCalls[0;2];
    .qunit.assertEquals[key m; `chunk`rows; "misc is an extensible dict"];
    .qunit.assertEquals[m`chunk; 0; "carrying at least the 0-based chunk index"]};

/ ---- input-space corners of the tolerance paths ------------------------------

testNullPaddingHeaderOnlyFile:{
    f:`:csvTestNpHdr.csv 0: enlist "a,b";
    t:.csv.read[f;::;::;(enlist `null_padding)!enlist 1b];
    .qunit.assertEquals[(cols t;count t); (`a`b;0); "a header-only file has nothing to pad and stays empty"]};

testIgnoreErrorsEveryRowBad:{
    f:`:csvTestIgAll.csv 0: ("a,b";"1";"2";"3");
    @[{![`.csvTest;();0b;enlist x]};`GAB;::];
    s:.csv.read[f;`.csvTest.GAB;::;`ignore_errors`rejects_table!(1b;`.csvTest.RAB)];
    .qunit.assertEquals[(s`rows;s`rejected); (0;3); "every row bad: nothing lands, everything counts"];
    .qunit.assertEquals[count .csvTest.RAB; 3; "and every one is in the audit"]};

testNullPaddingNeverPadsLongRows:{
    f:`:csvTestNpLong.csv 0: ("a,b";"1,2";"3,4,5");
    .qunit.assertThrows[.csv.read[;::;::;(enlist `null_padding)!enlist 1b]; f; "csv*";
        "null_padding pads SHORT rows only - extra columns still abort"]};

/ ---- codex input-space round: the four silent wrong-meaning successes, pinned -----------------

/ (1) a ragged file must not defeat the delimiter sniff when a lever tolerates raggedness -
/ oracle-verified: DuckDB picks ';' here under either lever
testSniffRelaxesUnderTolerance:{
    f:`:csvTestSnTol.csv 1: "a;b\n1;2\n3\n";
    @[{![`.csvTest;();0b;enlist x]};`GST;::];
    s:.csv.read[f;`.csvTest.GST;::;(enlist `ignore_errors)!enlist 1b];
    .qunit.assertEquals[(cols .csvTest.GST;s`rows;s`rejected); (`a`b;1;1);
        "the modal width carries ';' past the short row, which is then rejected, not schema-defining"];
    t:.csv.read[f;::;::;(enlist `null_padding)!enlist 1b];
    .qunit.assertEquals[t; ([]a:1 3;b:(2;0N)); "and under null_padding the short row pads instead"]};

/ (2) row bounds are PHYSICAL rows even around stray quotes: two malformed lines are two rejects,
/ never one merged record - the carry scan and the field parser read quotes by the same grammar
testIgnoreErrorsStrayQuotesRejectPerLine:{
    f:`:csvTestIgQ.csv 1: "a,b\n1,he\"llo\n2,x\"\n3,z\n";
    @[{![`.csvTest;();0b;enlist x]};`GIQ;::];
    s:.csv.read[f;`.csvTest.GIQ;::;`ignore_errors`rejects_table!(1b;`.csvTest.RIQ)];
    .qunit.assertEquals[(s`rows;s`rejected); (1;2); "two bad physical rows, one good one"];
    .qunit.assertEquals[(.csvTest.RIQ`line;.csvTest.RIQ`error); (2 3;`unquotedvalue`unquotedvalue);
        "each on its own line with its own record"]};

/ (3) a salvaged HEADER is audited like any salvaged row
testSalvagedHeaderIsRecorded:{
    f:`:csvTestSalvHdr.csv 1: "\"a\"x,b\n1,2\n";
    s:.csv.read[f;`.csvTest.GSH;::;`strict_mode`rejects_table!(0b;`.csvTest.RSH)];
    .qunit.assertEquals[(s`rows;s`rejected); (1;1); "the header's salvage counts"];
    .qunit.assertEquals[(.csvTest.RSH`line;.csvTest.RSH`error); (enlist 1;enlist `unquotedvalue);
        "recorded on line 1 with the quote class"]};

/ (4) reject lines are PHYSICAL lines: a quoted field's embedded newlines advance the counter
testRejectLinesCountEmbeddedNewlines:{
    f:`:csvTestLnQ.csv 1: "a,b\n\"x\ny\",1\n3\n";
    @[{![`.csvTest;();0b;enlist x]};`GLN;::];
    .csv.read[f;`.csvTest.GLN;"*j";`ignore_errors`rejects_table!(1b;`.csvTest.RLN)];
    .qunit.assertEquals[.csvTest.RLN`line; enlist 4; "the short row sits on physical line 4, not row 3"]};

/ codex craftsmanship round: the two carry/flush corners
testStrictModeOffFinalQuoteAtEof:{
    f:`:csvTestSmFq.csv 1: "a\n\"x\"";
    .qunit.assertEquals[.csv.read[f;::;::;(enlist `strict_mode)!enlist 0b]; ([]a:enlist enlist "x");
        "a file ending on a closing quote reads that field quoted - no byte past the buffer decides it"]};

testLambdaTrailingRejectsStillDelivered:{
    f:`:csvTestLamTail.csv 1: "a,b\n1,2\n3\n";
    .csvTest.TailCalls::();
    s:.csv.read[f;{[tblData;errData;misc] .csvTest.TailCalls,:enlist (tblData;errData;misc)};::;
        `ignore_errors`buffer_size`sample_size!(1b;4;2)];
    .qunit.assertEquals[sum {count x 1} each .csvTest.TailCalls; 1;
        "a reject arriving after the last data batch still reaches errData in a final batch"];
    .qunit.assertEquals[(s`rows;s`rejected;s`chunks); (1;1;count .csvTest.TailCalls);
        "and the summary, the audit and the batch count agree"]};

testToleranceOptionTypesChecked:{
    .qunit.assertThrows[.csv.read[.csvTest.BASIC;::;::;]; (enlist `ignore_errors)!enlist 1; "type*";
        "the tolerance levers are booleans"];
    .qunit.assertThrows[.csv.read[.csvTest.BASIC;::;::;]; (enlist `rejects_table)!enlist "r"; "type*";
        "rejects_table is a symbol"]};

/ ---- THE DIALECT OPTIONS (csv PR-4): quote / escape / comment - explicit only, NEVER auto-sniffed ------

testQuoteOptionCustomChar:{
    f:`:csvTestQc.csv 1: "a,b\n~x,y~,1\n~say ~~hi~~~,2\n";
    t:.csv.read[f;::;::;(enlist `quote)!enlist "~"];
    .qunit.assertEquals[t; ([]a:("x,y";"say ~hi~");b:1 2); "a custom quote char quotes and doubles like RFC's"];
    .qunit.assertThrows[.csv.read[;::;::;()!()]; f; "csv*"; "the same bytes under defaults still abort - the dialect is never sniffed"];
    g:`:csvTestQcB0.csv 1: "~x\ny~,1\n~z~,2\n";
    .qunit.assertEquals[.csv.read[g;::;::;`quote`header`buffer_size!("~";0b;1)]; ([]x:("x\ny";enlist "z");x1:1 2);
        "a custom quote at byte 0 of the file spans its newline, fed byte-by-byte"]};

testQuoteEmptyDisablesQuoting:{
    f:`:csvTestQoff.csv 1: "a,b\n\"unterminated,1\n2,3\n";
    t:.csv.read[f;::;::;(enlist `quote)!enlist ""];
    .qunit.assertEquals[t; ([]a:("\"unterminated";enlist "2");b:1 3); "quotes are ordinary bytes under an empty quote"];
    .qunit.assertThrows[.csv.read[;::;::;()!()]; f; "csv*"; "the same file under defaults still aborts"];
    g:`:csvTestQoffNl.csv 1: "a,b\n\"x,1\n\"y,2\n";
    .qunit.assertEquals[.csv.read[g;::;::;`quote`buffer_size!("";1)]; ([]a:("\"x";"\"y");b:1 2);
        "with quoting off every newline ends a row - no region can span a chunk boundary"]};

testEscapeOptionBackslash:{
    f:`:csvTestEsc.csv 1: "a,b\n\"say \\\"hi\\\"\",1\n\"x\\\\y\",2\n\"p\\qr\",3\n";
    t:.csv.read[f;::;::;(enlist `escape)!enlist "\\"];
    .qunit.assertEquals[t; ([]a:("say \"hi\"";"x\\y";"pqr");b:1 2 3);
        "esc+quote is a literal quote, esc+esc a literal esc, esc+any drops the esc (the oracle's law)"];
    .qunit.assertThrows[.csv.read[;::;::;()!()]; f; "csv*"; "the same bytes under defaults still abort"];
    .qunit.assertEquals[.csv.read[f;::;::;`escape`buffer_size!("\\";1)]; t;
        "every escape pair split across chunk boundaries resolves the same"]};

testQuoteAndEscapeCombine:{
    f:`:csvTestQe.csv 1: "a\n~x\\~y~\n";
    .qunit.assertEquals[.csv.read[f;::;::;`quote`escape!("~";"\\")]; ([]a:enlist "x~y");
        "a custom quote and a custom escape compose"]};

testCommentSkipsAndNeverCounts:{
    f:`:csvTestCm.csv 1: "# leading\na,b\n1,2\n# mid\n3,4\n";
    @[{![`.csvTest;();0b;enlist x]};`GCM;::];
    s:.csv.read[f;`.csvTest.GCM;::;`comment`rejects_table!("#";`.csvTest.RCM)];
    .qunit.assertEquals[(s`rows;s`rejected); (2;0); "comment lines are requested dialect - never counted, never rejected"];
    .qunit.assertEquals[.csvTest.GCM; ([]a:1 3;b:2 4); "comments before the header and between rows both vanish"];
    .qunit.assertEquals[count .csvTest.RCM; 0; "and the stored audit is empty"];
    .qunit.assertThrows[.csv.read[;::;::;()!()]; f; "csv*";
        "at defaults a stray # line stays a loud ragged error - comment is never sniffed"]};

testCommentMidLineTruncates:{
    f:`:csvTestCmMid.csv 1: "a,b\n1,2#tail\n3,4#";
    t:.csv.read[f;::;::;(enlist `comment)!enlist "#"];
    .qunit.assertEquals[t; ([]a:1 3;b:2 4); "an unquoted comment char truncates the row - as the final file byte too"];
    g:`:csvTestCmDelim.csv 1: "a,b\n1,#x\n2,3\n";
    .qunit.assertEquals[.csv.read[g;::;::;(enlist `comment)!enlist "#"]; ([]a:1 2;b:(0N;3));
        "truncation straight after a delimiter leaves the empty final field - the empty-field null"]};

testCommentTailIsOpaque:{
    f:`:csvTestCmOp.csv 1: "a,b\n1,2# z,\"unterm\n3,4\n";
    t:.csv.read[f;::;::;(enlist `comment)!enlist "#"];
    .qunit.assertEquals[t; ([]a:1 3;b:2 4); "a comment tail's delimiters and quotes bind nothing"];
    .qunit.assertEquals[.csv.read[f;::;::;`comment`buffer_size!("#";1)]; t;
        "and a chunk boundary inside the tail changes nothing"];
    g:`:csvTestCmOq.csv 1: "a,b\n# \"odd\n1,2\n";
    .qunit.assertEquals[.csv.read[g;::;::;(enlist `comment)!enlist "#"]; ([]a:enlist 1;b:enlist 2);
        "a first-byte comment line with an unmatched quote is skipped whole"]};

testCommentInsideQuotedFieldIsData:{
    f:`:csvTestCmQ.csv 1: "a,b\n\"x\n# kept\",1\n3,4\n";
    t:.csv.read[f;::;"*j";(enlist `comment)!enlist "#"];
    .qunit.assertEquals[t; ([]a:("x\n# kept";enlist "3");b:1 4);
        "inside a quoted field the comment char is data - a line-based pre-pass would corrupt this"]};

testCommentAfterClosingQuote:{
    f:`:csvTestCmCq.csv 1: "a\n\"x\"#tail\n\"y\"\n";
    .qunit.assertEquals[.csv.read[f;::;::;(enlist `comment)!enlist "#"]; ([]a:(enlist "x";enlist "y"));
        "the comment char is a valid follower of a closing quote"]};

testCommentFirstByteOnly:{
    f:`:csvTestCmSp.csv 1: "a,b\n1,2\n  # spaced\n3,4\n";
    .qunit.assertThrows[.csv.read[;::;::;(enlist `comment)!enlist "#"]; f; "csv*";
        "only a FIRST-byte comment char skips the line - a space-preceded one stays a loud ragged error"]};

testCommentLinesKeepPhysicalLineNumbers:{
    f:`:csvTestCmLn.csv 1: "a,b\n# two\n1,2\n3\n";
    @[{![`.csvTest;();0b;enlist x]};`GCL;::];
    .csv.read[f;`.csvTest.GCL;::;`comment`ignore_errors`rejects_table!("#";1b;`.csvTest.RCL)];
    .qunit.assertEquals[(.csvTest.RCL`line;.csvTest.RCL`error); (enlist 4;enlist `toofewcolumns);
        "comment lines count in the physical line numbering, never in the rejection"]};

testCommentOnlyFileRefuses:{
    f:`:csvTestCmAll.csv 1: "# a\n# b\n";
    .qunit.assertThrows[.csv.read[;::;::;(enlist `comment)!enlist "#"]; f; "csv*";
        "a file of nothing but comments has no rows and no schema"]};

/ THE NARROWED FALLBACK LAW: when no delimiter candidate qualifies, the file loads as ONE column only if it
/ is one-column-SHAPED - for every candidate a strict majority of sample rows have field count 1, none
/ malformed.  A merely polluted candidate (modal width above one) keeps the strict refusal, so a stray junk
/ line in a comma file stays loud.
testSingleColumnFallback:{
    f:`:csvTestOneCol.csv 1: "hello, world\nfoo\nbar baz\n";
    t:.csv.read[f;::;::;()!()];
    .qunit.assertEquals[t; flip (enlist `$"hello, world")!enlist ("foo";"bar baz");
        "a one-column-shaped file loads whole lines as one column, row 0 the header by the normal law"];
    .qunit.assertEquals[.csv.read[f;::;::;(enlist `buffer_size)!enlist 1]; t; "chunking plays no part"];
    .qunit.assertEquals[.csv.info[f;()!()]; (enlist `$"hello, world")!enlist "s"; "and .csv.info sees the same shape"];
    .qunit.assertThrows[.csv.read[;::;::;(enlist `delim)!enlist ","]; f; "csv*";
        "an explicit delimiter never falls back - the strict refusal stands"]};

testFallbackRefusesPollutedFiles:{
    f:`:csvTestPoll.csv 1: "a,b\n1,2\njunk\n3,4\n";
    .qunit.assertThrows[.csv.read[;::;::;()!()]; f; "csv*";
        "a comma file with one junk line is polluted, not one-column-shaped - still 'csv"];
    g:`:csvTestBin.csv 1: "hello, world\nfo\000o\nbar baz\n";
    .qunit.assertThrows[.csv.read[;::;::;()!()]; g; "csv*";
        "NUL-laden rows are binary, not one-column text - the fallback stands down and the refusal holds"]};

testFallbackKeepsQuotedRowBounds:{
    f:`:csvTestFbQ.csv 1: "\"x,\ny\"\nfoo\nbar\n";
    t:.csv.read[f;::;::;(enlist `header)!enlist 0b];
    .qunit.assertEquals[t; ([]x:("x,\ny";"foo";"bar"));
        "a well-formed quoted field keeps its embedded newline as ONE row even when the file loads one-column"]};

testDialectOptionValuesChecked:{
    .qunit.assertThrows[.csv.read[.csvTest.BASIC;::;::;]; (enlist `quote)!enlist 1b; "type*"; "quote is a char"];
    .qunit.assertThrows[.csv.read[.csvTest.BASIC;::;::;]; (enlist `escape)!enlist `e; "type*"; "escape is a char"];
    .qunit.assertThrows[.csv.read[.csvTest.BASIC;::;::;]; (enlist `comment)!enlist "##"; "domain*";
        "a multi-char comment is out of domain"];
    .qunit.assertThrows[.csv.read[.csvTest.BASIC;::;::;]; `delim`comment!(";";";"); "domain*";
        "the comment char cannot collide with an explicit delimiter"];
    .qunit.assertThrows[.csv.read[.csvTest.BASIC;::;::;]; `delim`quote!(";";";"); "domain*";
        "nor the delimiter with the quote"]};

/ `\z` (syscmds.md) selects the field order of the slash day-order forms.  The reader DELEGATES to the
/ one date home, so a declared d column follows it; the SNIFFER never adopts the shape (its gate wants
/ a separator at index 4), which is what keeps a mixed column text instead of guessing an order.
/ the saved \z is `ord`, never `z`: a bare-parameter lambda that names x/y/z takes that many arguments,
/ and qunit runs only rank-1 tests - a local `z` would silently make this a rank-3 function it skips
testDateOrderTypedColumnFollowsZ:{
    f:`:csvTestZ.csv 0: ("a";"06/01/2010");
    ord:system"z";
    system"z 0";
    .qunit.assertEquals[.csv.read[f;::;"d";.csvTest.NOOPT]; ([]a:enlist 2010.06.01);
        "\\z 0 reads the slash form month-first"];
    system"z 1";
    .qunit.assertEquals[.csv.read[f;::;"d";.csvTest.NOOPT]; ([]a:enlist 2010.01.06);
        "\\z 1 reads the same bytes day-first"];
    system"z ",string ord};

testDateOrderNeverSniffed:{
    f:`:csvTestZS.csv 0: ("a";"2024-03-08";"03/10/2024");
    ord:system"z";
    system"z 0";
    .qunit.assertEquals[.csv.read[f;::;::;.csvTest.NOOPT]; ([]a:("2024-03-08";"03/10/2024"));
        "a day-order shape is never sniffed - only a declared d column parses it"];
    system"z ",string ord};

/ ---- THE VENDORED-CORPUS PINS (csv endgame, owner directive 2026-08-23) -----------------------------
/ These rows left csvDiffTest.q when the differential was finalized to all-green.  Each pins OUR CURRENT
/ behaviour on a real-world corpus file - regression protection, NOT a claim of kdb-truth or duck-truth.
/ The files are read-only under fixtures/csv/duckdb (provenance: qlib/test/fixtures/csv/README.md); the
/ harness plants the `fixtures` symlink in its run cwd, so the relative path is the one csvDiffTest uses.
/ Every read is at DEFAULTS - the fixture, not an option, is what each test is about.
VF:"fixtures/csv/duckdb/";
rdv:{[n] .csv.read[hsym `$.csvTest.VF,n;::;::;()!()]};

/ strict quoting (the default) refuses a field whose quotes cannot be read as quotes: a space between
/ the delimiter and the opening quote, a bare quote inside an unquoted field, quotes inside a quoted one
testVendoredMalformedQuotesRefuse:{
    {[n] .qunit.assertThrows[.csvTest.rdv;n;"csv*";"strict quoting refuses ",n]} each
        ("14512.csv";"all_quotes.csv";"empty_space_start_value.csv";"stops.csv";"bug_7578.csv")};

/ a row wider or narrower than the header is loud without a tolerance lever - including the real-world
/ shape where the wider body sits under a one-column preamble the reader never skips away
testVendoredRaggedRowsRefuse:{
    {[n] .qunit.assertThrows[.csvTest.rdv;n;"csv*";"a ragged file is loud: ",n]} each
        ("bug_12596.csv";"mixed_options.csv";"unionbyname_21248_1.csv";"17738.csv";"17738_rn.csv")};

testVendoredTzSuffixStaysText:{
    off:("2020-12-30 01:25:58.745232+01";"2020-12-30 02:25:58.745232+01";
        "2020-12-30 03:25:58.745232+01";"2020-12-30 04:25:58.745232+01");
    .qunit.assertEquals[.csvTest.rdv "timestampoffset.csv"; flip (enlist `col1)!enlist off;
        "an offset-suffixed timestamp column stays text, bytes intact"];
    .qunit.assertEquals[.csvTest.rdv "ubn2.csv";
        ([]b:88 90 91 92;a:("test";"8cb123cb8";"34fd321";"fg5391jn4");ts:off);
        "and its neighbours still type normally"];
    .qunit.assertEquals[.csvTest.rdv "timestamp_with_tz.csv";
        ([]id:1 2;timestamps:("2021-05-25 04:55:03.382494 UTC";"2021-05-25 04:55:03.382494 EST"));
        "a zone-NAME suffix leaves the column text too"];
    .qunit.assertEquals[.csvTest.rdv "timestamp_timezone.csv";
        ([]time:("2020-01-01T00:00:00";"2020-01-01T00:00:00-08";"2020-01-01T00:00:00+00";"2020-01-01T00:00:00Z";
            "2020-01-01T00:00:00+08:00";"2020-01-01T00:00:00+05:30";"2020-01-01T00:00:00+13:45";
            "2020-01-01T00:00:00-03:30");
        description:("midnight local";"midnight in San Francisco";"midnight UTC +00";"midnight UTC Z";
            "midnight in Taipei";"Asia/Kolkata";"Pacific/Chatham";"Canada/Newfoundland"));
        "one offset-bearing cell holds the whole column as text - never a per-row split"]};

/ the never-guess law as these corpus files exercise it: on each of them a slash/dash day-order column
/ arrives text, and the one yyyy-first slash file types.  The law itself lives in lib/csv.q's doc
testVendoredDayOrderStaysText:{
    .qunit.assertEquals[.csvTest.rdv "date_example_2.csv";
        flip (enlist `date_example)!enlist ("12/17/23";"12/14/23";"12/20/23"); "mm/dd/yy stays text"];
    .qunit.assertEquals[.csvTest.rdv "dates.csv"; ([]number:enlist "919 304 6161";date:enlist "10/08/2008");
        "so does a quoted one beside a quoted number"];
    .qunit.assertEquals[.csvTest.rdv "timestamp_tz.csv";
        flip (enlist `$"1/1/2020")!enlist ("1/1/2020";"1/1/2020";"5/7/1995");
        "and row 0 is still the header when the whole column is text"];
    .qunit.assertEquals[.csvTest.rdv "multi_quote.csv";
        ([]datecol:("1/1/19";"1/2/19";"1/3/19";"1/4/19";"1/5/19";"1/6/19";"1/9/19");
        textcol:("text";"text";"text";"text";"text";"text, with comma";"'text'"));
        "a quoted comma is one cell; the date column beside it is still text"];
    .qunit.assertEquals[.csvTest.rdv "15473_date_timestamp.csv";
        flip (enlist `a)!enlist (4#2024.12.12D00:00:00.000000000),2020.01.01D01:02:03.000000000;
        "yyyy/mm/dd is unambiguous, so a date-and-timestamp column promotes to timestamp"];
    .qunit.assertEquals[.csvTest.rdv "mixed_timestamps.csv";
        ([]A:1 2 3 4 5;B:("2024-03-08 01:25:58";"2024-03-09 01:25:58";"03/10/2024 01:25:58";
        "2024-03-11 01:25:58";"2024-03-12 01:25:58"));
        "and ONE day-order cell among four ISO ones holds the whole column text"]};

/ the dd-mm/mm-dd family: t types (hh:mm:ss is second), d and ts stay text - one file per field order,
/ and the reader answers the same way for all three because it declines to pick an order
testVendoredDayOrderFamily:{
    tm:12:12:12 14:15:30 15:16:17;
    .qunit.assertEquals[.csvTest.rdv "time_date_timestamp_dd-mm-yy.csv";
        ([]a:123 345 346;b:3#enlist "TEST2";t:tm;d:("01-01-2000";"02-02-2002";"13-12-2004");
        ts:("01-01-90 12:12:00";"02-02-02 14:15:00";"13-12-04 15:16:00")); "dd-mm-yy: clock types, date text"];
    .qunit.assertEquals[.csvTest.rdv "time_date_timestamp_mm-dd-yy.csv";
        ([]a:123 345 346;b:3#enlist "TEST2";t:tm;d:("01-01-90";"02-02-02";"12-13-04");
        ts:("01-01-90 12:12:00 AM";"02-02-02 02:15:00 PM";"12-13-04 03:16:00 PM")); "mm-dd-yy the same"];
    .qunit.assertEquals[.csvTest.rdv "time_date_timestamp_mm-dd-yyyy.csv";
        ([]a:123 345 346;b:3#enlist "TEST2";t:tm;d:("01-01-2000";"02-02-2002";"12-13-2004");
        ts:("01-01-2000 12:12:00 AM";"02-02-2002 02:15:00 PM";"12-13-2004 03:16:00 PM"));
        "and a four-digit year does not resolve the FIELD ORDER, so it stays text too"]};

testVendoredNumericShapes:{
    .qunit.assertEquals[.csvTest.rdv "decimal_separators.csv";
        ([]commas:("1,1";"0,25";"1,53e4";"+1,53e4";"-1,53e4");periods:1.1 0.25 15300 15300 -15300f);
        "a comma decimal is text (the comma is a delimiter, not a point); a period one is a float"];
    .qunit.assertEquals[.csvTest.rdv "phonenumbers.csv";
        flip (enlist `phone)!enlist 318855443322 552244331122 12233445567;
        "a leading + is a sign, so a +-prefixed number is a long and loses the +"];
    .qunit.assertEquals[.csvTest.rdv "leading_zeros2.csv";
        ([]comune:("030151360";"030120530";"020040580";"030150870";"190480090");
        codice_regione:("03";"03";"02";"03";"19");codice_provincia:("015";"012";"004";"015";"048");
        codice_comune:("1360";"0530";"0580";"0870";"0090");
        denominazione_comune:("POLPENAZZE DEL GARDA";"CAROBBIO DEGLI ANGELI";"SAINT-DENIS";"LOSINE";"CAPO D'ORLANDO");
        sigla_provincia:("BS";"BG";"AO";"BS";"ME");
        data_entrata_in_carica:("13/10/2021";"04/10/2021";"23/09/2020";"04/10/2021";"27/10/2021"));
        "a leading-zero numeric is text - the zeros are data, not a formatting artefact"]};

/ header conventions on corpus files: duplicates dedupe with _n, a blank header cell takes a generated
/ name, and a file of one physical line is a header with no rows
testVendoredHeaderConventions:{
    .qunit.assertEquals[.csvTest.rdv "test_header_mix.csv";
        ([]a:123 345;a_1:2#enlist "TEST2";a_2:("text1";"text2");a_3:2#enlist "";a_4:2#enlist "";a_5:2#enlist "";
        a_6:2#enlist "";a_7:2#enlist "";a_8:2#enlist "";a_9:2#enlist "";column12:2#enlist "";x11:2#enlist "";
        x12:("value1";"value2")); "ten repeats of a dedupe to a a_1..a_9; the blank headers take x11 x12"];
    t:.csvTest.rdv "many_bytes.csv";
    .qunit.assertEquals[cols t; `$("thisisaverysuberverylargestring\\";"thisisaverysuberverylargestring\\_1";
        "thisisaverysuberverylargestring\\_2";"x3"); "a trailing delimiter adds a fourth, blank-named column"];
    .qunit.assertEquals[(count t;distinct t`x3); (7;enlist ""); "which is empty in every row"];
    .qunit.assertEquals[.csvTest.rdv "trailing_delimiter.csv"; flip `AAA`x1!(();());
        "a header-only line loads as a zero-row table, its columns still untyped"];
    .qunit.assertEquals[.csvTest.rdv "versions.csv"; flip (`$("2.2.2";"1.2.20";"1@2@20"))!(();();());
        "and one physical line with no newline is exactly that: a header"]};

/ CR, LF and CRLF all end a row, mixed within one file
testVendoredLineEndings:{
    .qunit.assertEquals[.csvTest.rdv "mixed_line_endings.csv";
        ([]x:10 20 30;x1:("hello";"world";"test");x2:20 30 30);
        "a CR-ended row beside an LF-ended one; row 0 is all-typed, so it is data and the names are generated"];
    .qunit.assertEquals[.csvTest.rdv "mixed_new_line.csv"; ([]x:1 4 7;x1:2 5 8;x2:3 6 9); "likewise all-numeric"];
    mix:([]x:1 10 100 1000 10000 100000 1000000 10000000 100000000;
        x1:6370 214 2403 1564 10617 430 1904 12845 15519;x2:371 465 160 67 138 181 658 370 785;
        x3:("p1";"p2";"p3";"p4";"p5";"p6";"p7";"p8";"p9"));
    .qunit.assertEquals[.csvTest.rdv "multi_column_string_mix.csv"; mix; "a pipe file with mixed endings"];
    .qunit.assertEquals[.csvTest.rdv "multi_column_string_mix_r_n.csv"; mix; "and the same bytes CR-only"];
    .qunit.assertEquals[.csvTest.rdv "one_r_n_two.csv"; flip (enlist `one)!enlist ("two";"three");
        "a one-column CRLF file keeps row 0 as the header"]};

/ row 0 is a header only where it reads like one: an all-typed first row is data, and a text column
/ whose row 0 is no more textual than its body has no header evidence either
testVendoredNoHeaderWhenRowZeroIsData:{
    .qunit.assertEquals[.csvTest.rdv "single_column.csv";
        flip (enlist `x)!enlist ("123";"123";"123";"one";"123";"123";"123";"123";"123";"123");
        "one text value mid-column holds the whole column text - and row 0 is data, not a header"];
    .qunit.assertEquals[.csvTest.rdv "small_bad.csv";
        ([]x:(enlist "1";enlist "1";enlist "C";enlist "1");x1:(enlist "A";enlist "A";enlist "A";enlist "B"));
        "the same over two columns"];
    .qunit.assertEquals[.csvTest.rdv "repromarket.csv";
        flip (enlist `$"nemanja.krpovic@gmail.com:krlleta")!enlist
            ("vega@example.combogus";"Vega-Inject:bogus:vega";"mirkofoto@gmail.com:mirko");
        "no delimiter candidate qualifies, so whole lines load as one column - and a bare CR still ends a row"]};

/ a pipe file whose ISO timestamps carry +00:00: text by the tz posture, while an empty trailing field
/ under a typed column is that type's null
testVendoredEmptyFieldIsTheColumnNull:{
    t:.csvTest.rdv "part-2.csv";
    .qunit.assertEquals[(type t`creationDate;type t`id); (0h;7h); "the offset column is text, the id column long"];
    .qunit.assertEquals[t`ParentPostId; 549756862464 549756862464 0N; "an empty cell is the long null"];
    .qunit.assertEquals[first t`creationDate; "2012-09-30T23:48:54.208+00:00"; "and the offset bytes survive"]};

/ the wide real-world files, pinned by shape and by one row: a full literal would be unreadable, and
/ the shape is what a sniffer regression moves
testVendoredWideFilesKeepTheirShape:{
    t:.csvTest.rdv "ncvoter.csv";
    .qunit.assertEquals[(count t;count cols t;type t`x;first t`x1); (10;94;7h;"ALAMANCE");
        "a headerless 94-column quoted-tab file - quoted digits still type, quoting is dialect not a declaration"];
    s:.csvTest.rdv "soccer_kaggle.csv";
    .qunit.assertEquals[(count s;count cols s); (14;110); "blank lines between records are skipped, not rows"];
    .qunit.assertEquals[(first s`short_name;first s`dob;type s`dob); ("L. Messi";1987.06.24;14h);
        "dashed dates type; names do not"];
    .qunit.assertEquals[first s`long_name; "Lionel Andr\303\251s Messi Cuccittini";
        "UTF-8 arrives as the bytes it was written as - no transcoding, no validator"];
    w:.csvTest.rdv "weather.csv";
    .qunit.assertEquals[(count w;type w`average_temperature;type w`precipitation); (366;9h;0h);
        "one \"T\" (trace) anywhere in a column holds it text; a column without one is a float"];
    .qunit.assertEquals[first w`date; "1-1-2016"; "and d-m-yyyy is a day-order form, so it stays text"]};
