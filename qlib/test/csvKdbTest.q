/ .csv kdb-format CONTRACT suite (owner-ratified 2026-08-22, frozen under tools/frozen.manifest):
/ THE LAW - the writer's text forms define the sniffer's grammar.  Whatever `csv 0:` writes, .csv.read
/ loads back TYPE-IDENTICAL with no types argument (Tier A), and the ambiguous forms (bool 1/0, whole
/ floats, syms) round-trip under meta-derived explicit types (Tier B).  The sniffer never guesses beyond
/ a written form: bare 20260822 stays long, suffixless 2026.08 stays float.  Clock shapes promote PER
/ COLUMN up the minute < second < time < timespan lattice - widest observed form, never truncation.
/ EDITS TO THIS FILE ARE OWNER-ESCALATION; every test must be green - no reds tolerated, ever.
system "d .csvKdbTest";

NOOPT:()!();

/ ---- Tier A: sniffed round-trip, one unambiguously-written type per test ------

testDateRoundTrip:{
    t:([]d:2026.08.22 1999.01.01 2000.02.29);
    f:`:csvKdbDate.csv 0: csv 0: t;
    .qunit.assertTrue[t ~ .csv.read[f;::;::;.csvKdbTest.NOOPT]; "dashed dates load back as dates"]};

testTimestampRoundTrip:{
    t:([]p:(2026.08.22D12:34:56.123456789;1999.12.31D23:59:59.999999999;2000.01.01D00:00:00.000000000));
    f:`:csvKdbTs.csv 0: csv 0: t;
    .qunit.assertTrue[t ~ .csv.read[f;::;::;.csvKdbTest.NOOPT]; "the dotted-D written form loads back as timestamps"]};

testMonthRoundTrip:{
    t:([]m:2026.08 1999.12 2000.01m);
    f:`:csvKdbMonth.csv 0: csv 0: t;
    .qunit.assertTrue[t ~ .csv.read[f;::;::;.csvKdbTest.NOOPT]; "the m suffix marks months unambiguously"]};

testMinuteRoundTrip:{
    / durations: the hour field is uncapped (24:00, 1500:00, the 0Wu display) and the writer signs negatives
    t:([]u:(12:34;24:00;25:00;-12:34;`minute$90000;0Wu));
    f:`:csvKdbMinute.csv 0: csv 0: t;
    .qunit.assertTrue[t ~ .csv.read[f;::;::;.csvKdbTest.NOOPT]; "h:mm loads back as minutes, signs, >24h and 0Wu included"]};

testSecondRoundTrip:{
    t:([]v:(12:34:56;25:01:01;-12:34:56;00:00:00;0Wv));
    f:`:csvKdbSecond.csv 0: csv 0: t;
    .qunit.assertTrue[t ~ .csv.read[f;::;::;.csvKdbTest.NOOPT]; "h:mm:ss loads back as seconds"]};

testTimeRoundTrip:{
    t:([]tt:(12:34:56.789;-12:34:56.789;00:00:00.001;0Wt));
    f:`:csvKdbTime.csv 0: csv 0: t;
    .qunit.assertTrue[t ~ .csv.read[f;::;::;.csvKdbTest.NOOPT]; "the 3-digit fraction marks time"]};

testTimespanRoundTrip:{
    t:([]n:(0D12:34:56.123456789;1D01:00:00.000000000;-0D01:02:03.123456789;0Wn));
    f:`:csvKdbSpan.csv 0: csv 0: t;
    .qunit.assertTrue[t ~ .csv.read[f;::;::;.csvKdbTest.NOOPT]; "the D separator and 9-digit fraction mark timespan"]};

testFloatInfRoundTrip:{
    t:([]f:(1.5;-2.25;0w;-0w));
    f:`:csvKdbFloat.csv 0: csv 0: t;
    .qunit.assertTrue[t ~ .csv.read[f;::;::;.csvKdbTest.NOOPT]; "0w and -0w are written float forms"]};

testLongAndStringRoundTrip:{
    t:([]j:42 -7 0;c:("hello";"xy";"world"));
    f:`:csvKdbLong.csv 0: csv 0: t;
    .qunit.assertTrue[t ~ .csv.read[f;::;::;.csvKdbTest.NOOPT]; "longs stay long; bare text stays a string column"]};

/ nulls write EMPTY cells (owner ruling 2026-08-22 - string of a null is "") and empty reads back as
/ the typed null, so a null-bearing table round-trips
testNullRoundTrip:{
    t:([]j:1 0N 3;f:(1.5;0n;2.5);d:(2026.08.22;0Nd;2000.01.01);
       p:(2026.08.22D12:34:56.123456789;0Np;2000.01.01D00:00:00.000000000);
       u:(12:34;0Nu;00:01);v:(12:34:56;0Nv;00:00:01);tt:(12:34:56.789;0Nt;00:00:00.001);
       n:(0D12:34:56.123456789;0Nn;1D01:00:00.000000000);m:(2026.08m;0Nm;1999.12m));
    fp:`:csvKdbNulls.csv 0: csv 0: t;
    .qunit.assertTrue[t ~ .csv.read[fp;::;::;.csvKdbTest.NOOPT]; "null cells write empty and read back typed"]};

testNullTypedRoundTrip:{
    t:([]b:101b;s:(`ab;`;`cd);c:("hi";"";"zz"));
    fp:`:csvKdbNullsB.csv 0: csv 0: t;
    .qunit.assertTrue[t ~ .csv.read[fp;::;"bs*";.csvKdbTest.NOOPT];
        "the null sym and the empty string ride the typed lane"]};

/ the ratified falsifying row: dotted-D timestamp + m-month + 0w in ONE file, NO types argument
testFalsifyingRow:{
    t:([]p:enlist 2026.08.22D12:34:56.123456789;m:enlist 2026.08m;f:enlist 0w);
    f:`:csvKdbFalsify.csv 0: csv 0: t;
    .qunit.assertTrue[t ~ .csv.read[f;::;::;.csvKdbTest.NOOPT]; "the falsifying row loads type-identical unaided"];
    .qunit.assertEquals[.csv.info[f;.csvKdbTest.NOOPT]; `p`m`f!"pmf"; "and info reflects the same grammar"]};

/ ---- the promotion lattice: per column, widest observed form, never truncation

testMinuteSecondPromoteToSecond:{
    f:`:csvKdbProm1.csv 0: ("a";"12:34";"12:34:56");
    .qunit.assertTrue[([]a:(12:34:00;12:34:56)) ~ .csv.read[f;::;::;.csvKdbTest.NOOPT];
        "a second cell promotes the whole minute column to second"]};

testFractionPromotesToTime:{
    f:`:csvKdbProm2.csv 0: ("a";"12:34:56";"12:34:56.789");
    .qunit.assertTrue[([]a:(12:34:56.000;12:34:56.789)) ~ .csv.read[f;::;::;.csvKdbTest.NOOPT];
        "any fractional cell promotes the column to time"]};

testClockJoinsTimespan:{
    f:`:csvKdbProm3.csv 0: ("a";"12:34";"0D00:00:01.000000000");
    .qunit.assertTrue[([]a:(0D12:34:00.000000000;0D00:00:01.000000000)) ~ .csv.read[f;::;::;.csvKdbTest.NOOPT];
        "a timespan cell absorbs narrower clock forms"]};

testDateJoinsTimestamp:{
    f:`:csvKdbProm4.csv 0: ("a";"2026-08-22";"2026.08.22D12:00:00.000000000");
    .qunit.assertTrue[([]a:(2026.08.22D00:00:00.000000000;2026.08.22D12:00:00.000000000)) ~ .csv.read[f;::;::;.csvKdbTest.NOOPT];
        "a date under a timestamp column reads as midnight"]};

/ a post-sample cell WIDER than the frozen type is the frozen-type-miss law, never a truncation
testWiderPostSampleCellAborts:{
    f:`:csvKdbWider.csv 0: ("a";"12:34";"12:34:56");
    .qunit.assertThrows[.csv.read[f;::;::;]; (enlist `sample_size)!enlist 2; "csv*";
        "the sample froze minute; a later second cell aborts"]};

/ ---- the ISO alternates the reader already accepted stay in ------------------

/ the tz-suffixed alternate is NOT one of them (owner ruling 2026-08-23, re-recorded): kdb parses no
/ timezone at all - the corpus handles offsets by table arithmetic, and neither tok nor cast reads one -
/ so a UTC-converting sniff was our invention.  At defaults the offset cell is text; `timestampformat`
/ with %z is where a caller asks for one.
testIsoAlternatesStay:{
    f:`:csvKdbIso.csv 0: ("a,b,c";"2026-08-22T12:34:56.5,2026-08-22 06:00:00,2026-08-22T12:34:56+02:00");
    t:([]a:enlist 2026.08.22D12:34:56.500000000;b:enlist 2026.08.22D06:00:00.000000000;c:enlist "2026-08-22T12:34:56+02:00");
    .qunit.assertTrue[t ~ .csv.read[f;::;::;.csvKdbTest.NOOPT]; "T- and space-separated timestamps load; a tz-suffixed one stays text"]};

/ ---- never guess beyond a written form ---------------------------------------

testNeverGuess:{
    f:`:csvKdbGuess.csv 0: ("a,b";"20260822,2026.08");
    .qunit.assertTrue[([]a:enlist 20260822;b:enlist 2026.08) ~ .csv.read[f;::;::;.csvKdbTest.NOOPT];
        "bare digits stay long, a suffixless yyyy.mm stays float"]};

testNearMissStaysText:{
    f:`:csvKdbNearMiss.csv 0: ("a,b,c,d,e";"2026.08.22D,2026.13m,12:60:00,0Dgarbage,0ww");
    t:.csv.read[f;::;::;.csvKdbTest.NOOPT];
    .qunit.assertTrue[t ~ flip `a`b`c`d`e!(enlist each ("2026.08.22D";"2026.13m";"12:60:00";"0Dgarbage";"0ww"));
        "a bare D, an invalid month, an out-of-range clock and near-miss specials are all text"]};

/ ---- Tier B: the ambiguous forms round-trip under meta-derived types ---------

testTypedRoundTripFullRoster:{
    t:([]b:101b;j:2 7 4;f:1 2 3f;s:`ab`cd`ab;c:("hi";"yo";"zz");
       d:2026.08.22 1999.01.01 2000.02.29;
       u:(12:34;25:00;-12:34);
       v:(12:34:56;25:01:01;-00:00:01);
       tt:(12:34:56.789;00:00:00.001;-01:00:00.000);
       p:(2026.08.22D12:34:56.123456789;1999.12.31D23:59:59.999999999;2000.01.01D00:00:00.000000000);
       m:2026.08 1999.12 2000.01m;
       n:(0D12:34:56.123456789;1D01:00:00.000000000;-0D01:02:03.123456789));
    f:`:csvKdbTierB.csv 0: csv 0: t;
    ts:`char$exec t from meta t;
    ts[where ts="C"]:"*";
    .qunit.assertTrue[t ~ .csv.read[f;::;ts;.csvKdbTest.NOOPT];
        "every supported type - bools, whole floats and syms included - round-trips under meta-derived types"]};
