/ FFI conformance: the .ffi namespace — the KX ffikdb surface over libffi.
/ Sources: user-docs/ffi.md and the published ffikdb corpus, adapted.
/ PORTABILITY: portable q SYNTAX, and a conformance spec in the #459 sense — it
/ states the contract .ffi must satisfy, so it names .ffi as freely as
/ regexpTest.q names .regexp, and the harness loads the implementation.  Tests
/ that assert a peachq DIVERGENCE say so in their names: testExtension* for the
/ four supersets beyond KX (SentinelOptional, AutoNulTerminates,
/ CallbackErrorContract, BareErrorClasses), testStricter* for the one narrowing
/ — the type letter enforced against the element type, which KX accepts — and
/ those four are guarded by isPeachFfi so kx q skips them instead of failing.
/ Foreign symbols are bound BARE wherever possible — they resolve against the
/ already-linked process, so no library name appears — and the one test of the
/ explicit `lib`fn dlopen path derives its name from .ffi.os[], dogfooding the
/ function whose return type started this work.
system "d .ffiTest";

/ glibc/msvcrt names; an OS we have no name for asserts only the bare half
/ (macOS keeps libm inside libSystem, so it is deliberately absent here)
libmOf:("lw"!(`libm.so.6;`msvcrt.dll));
isLinux:{"l"=.ffi.os[]};
/ the Windows CRT spells the POSIX name with a leading underscore
pidSym:{$["w"=.ffi.os[]; `_getpid; `getpid]};

/ The engine is a BUILD fact, not a q one: libffi links on a Linux-x86_64 host
/ only (native plus its win64 cross), and everywhere else every .ffi.i.* native
/ answers 'nyi while the pure-q .ffi.extension/os/ptrsize keep working.  So the
/ three token tests below run everywhere and every call test asks first.
/ probed through setErrno, which allocates nothing: bindings are PERMANENT in
/ the engine, so probing with bind would burn a slot on every guarded test
hasFfi:{`ok~@[{.ffi.setErrno[]; `ok};::;{[e] `no}]};
noFfi:{.qunit.assertTrue[1b; "this build has no libffi — .ffi answers 'nyi, so the call surface is not asserted"]};

/ An uppercase letter means "pointer to a vector of THAT type", and the letter is the only
/ thing that can say which: in the call interface I, J and F are all just "a pointer".  peachq
/ ENFORCES the letter against the argument's elements and answers 'type; KX does not — its
/ getvalue takes the letter's q type as the target type and never compares it to the argument,
/ so a mismatched vector crosses as its raw payload and C reads at the wrong stride.  The
/ asserts that expect 'type are therefore peachq-only; the positive ones run on both.
/ .ffi.i.bind is the marker: KX's q/ffi.q binds .ffi.bind straight through 2: and has no
/ .ffi.i.* layer at all, so the native exists only here — and it is itself an FFI question,
/ not an unrelated namespace sniff.
isPeachFfi:{`ok~@[{.ffi.i.bind; `ok};::;{[e] `no}]};
notPeachFfi:{.qunit.assertTrue[1b; "kx q accepts a letter/element mismatch — only peachq raises, so this is not asserted here"]};

/ ---- the platform tokens: three functions, three DIFFERENT return types ----

testPlatformTokenTypes:{
    .qunit.assertEquals[type .ffi.extension[]; -11h; "extension answers a SYMBOL"];
    .qunit.assertEquals[type .ffi.os[]; -10h; "os answers a CHAR, not a symbol"];
    .qunit.assertEquals[type .ffi.ptrsize[]; -6h; "ptrsize answers an INT"];
    .qunit.assertTrue[.ffi.extension[] in `so`dll`dylib; "extension names this platform's shared-library suffix"];
    .qunit.assertTrue[.ffi.os[] in "lwm"; "os is one letter of l, w or m"];
    .qunit.assertTrue[.ffi.ptrsize[] in 4 8i; "ptrsize is a pointer width in bytes"]};

/ the reported bug: ` sv composes with extension and NOT with os, because sv
/ joins symbols and os hands back a char
testExtensionComposesWithSv:{
    .qunit.assertEquals[` sv `qr,.ffi.extension[]; `$"qr.",string .ffi.extension[]; "extension is a symbol, so sv builds a library name"];
    .qunit.assertThrows[{` sv `qr,.ffi.os[]}; ::; "type"; "os is a char, so the same idiom is 'type"];
    .qunit.assertEquals[type `$"qr",.ffi.os[]; -11h; "`$ is how the OS letter reaches a symbol"]};

/ ---- the calling convention: ONE argument list, and what collapse does ----

testBindTakesOneArgList:{
    if[not hasFfi[]; :noFfi[]];
    f:.ffi.bind[`pow;"ff";"f"];
    .qunit.assertEquals[f (2f;10f;::); 1024f; "the trailing :: keeps the argument list general"];
    .qunit.assertThrows[f; (2f;10f); "rank"; "(2f;10f) collapsed to ONE float vector before the call"];
    .qunit.assertThrows[f; 2f; "rank"; "a single atom is not the two arguments pow was bound for"]};

/ the collapse is ordinary q and happens before .ffi sees anything
testCollapseIsAQFact:{
    .qunit.assertEquals[type (2f;10f); 9h; "same-type items written in a list ARE a vector"];
    .qunit.assertEquals[count (2f;10f); 2; "one value of count 2, not two values"];
    .qunit.assertEquals[type (2f;10f;::); 0h; ":: makes the list general, which is all the sentinel does"]};

/ the ONE test of the explicit `lib`fn dlopen path; an OS we have no library
/ name for asserts only the bare-symbol half
testBindExplicitLibrary:{
    if[not hasFfi[]; :noFfi[]];
    if[.ffi.os[] in key libmOf;
        .qunit.assertEquals[.ffi.bind[(libmOf .ffi.os[]),`pow;"ff";"f"] (2f;10f;::); 1024f; "an explicit `lib`fn pair takes the dlopen path"]];
    .qunit.assertEquals[.ffi.bind[`pow;"ff";"f"] (2f;10f;::); 1024f; "and the same function resolves BARE, against the already-linked process"]};

testBindZeroArgs:{
    if[not hasFfi[]; :noFfi[]];
    b0:.ffi.bind[pidSym[];"";"i"];
    .qunit.assertEquals[b0 (::); "i"$.z.i; "empty argtypes still takes an argument list — the sentinel alone"];
    .qunit.assertEquals[b0 (::;::); "i"$.z.i; "the KX (::;::) spelling is tolerated"];
    .qunit.assertEquals[.ffi.callFunction[pidSym[]] (::;::); "i"$.z.i; "callFunction reaches a zero-arg function the same way"]};

testCallFunctionSpellings:{
    if[not hasFfi[]; :noFfi[]];
    .qunit.assertEquals[.ffi.callFunction[("f";`sqrt)] (16f;::); 4f; "(returnletter;`fn) names the return type"];
    .qunit.assertEquals[.ffi.callFunction[("i";`abs)] (-7i;::); 7i; "argument types are inferred from the values"];
    .qunit.assertEquals[.ffi.callFunction[("i";`strlen)] (`abcdef;::); 6i; "a symbol marshals as char*"];
    .qunit.assertEquals[.ffi.callFunction[("i";`strlen)] ("abc\000";::); 3i; "an explicit NUL tail marshals in place"]};

/ ---- integer and float letters ----

testIntegerAndFloatLetters:{
    if[not hasFfi[]; :noFfi[]];
    .qunit.assertEquals[.ffi.bind[`lround;"f";"j"] (2.6;::); 3; "f in, j out"];
    .qunit.assertEquals[.ffi.bind[`labs;"j";"j"] (-7;::); 7; "j in, j out"];
    .qunit.assertEquals[.ffi.bind[`pow;"ff";"f"] (3f;2f;::); 9f; "two f arguments"]};

/ ---- strings and pointers ----

testStringArguments:{
    if[not hasFfi[]; :noFfi[]];
    strlen:.ffi.bind[`strlen;"C";"i"];
    .qunit.assertEquals[strlen ("12345";::); 5i; "a char vector reaches C as char*"];
    .qunit.assertEquals[strlen (`abcdef;::); 6i; "so does a symbol"];
    .qunit.assertEquals[strlen ("123\000";::); 3i; "an explicit NUL tail stops the count where C does"]};

testPointerReturnsAString:{
    if[not hasFfi[]; :noFfi[]];
    sr:.ffi.bind[`strerror;"i";"C"];
    .qunit.assertEquals[sr (2i;::); "No such file or directory"; "C dereferences a char* return into a string"];
    .qunit.assertEquals[type sr (2i;::); 10h; "and the result is a string, not a symbol"]};

testOutputBufferWrittenInPlace:{
    if[not hasFfi[]; :noFfi[]];
    buf:80#"\000";
    n:.ffi.callFunction[("i";`sprintf)] (buf;"%s %f %hd\000";"test\000";2f;0h;::);
    .qunit.assertEquals[n; 15i; "sprintf answers the character count it wrote"];
    .qunit.assertEquals[buf til "j"$n; "test 2.000000 0"; "a vector crosses as an in-place pointer, so C's write is visible"]};

/ the s return letter, on strerror rather than strerror_r: strerror is C89 and
/ so present everywhere, where strerror_r is absent on Windows and returns an
/ int under musl's POSIX signature, which "s" would misread
testSymbolReturn:{
    if[not hasFfi[]; :noFfi[]];
    r:.ffi.bind[`strerror;"i";"s"] (2i;::);
    .qunit.assertEquals[type r; -11h; "an s return reads the char* back as a symbol"];
    .qunit.assertTrue[0<count string r; "and the symbol carries the text, not an empty interning"]};

/ an int vector as an out-parameter.  fd 0 is a VALID descriptor, so the
/ assertion is >=0 and the vector starts at -1 to prove the call wrote it.
testIntVectorOutParameter:{
    if[not hasFfi[]; :noFfi[]];
    if[not isLinux[]; :.qunit.assertTrue[1b; "pipe/close are POSIX — not asserted off Linux"]];
    p:-1 -1i;
    .qunit.assertEquals[.ffi.callFunction[("i";`pipe)] (p;::); 0i; "pipe answers 0 on success"];
    .qunit.assertTrue[all p>=0; "and wrote both descriptors back into the q vector"];
    .qunit.assertEquals[(.ffi.callFunction[("i";`close)] (p 0;::)),.ffi.callFunction[("i";`close)] (p 1;::); 0 0i; "both descriptors close"]};

testCvarReadsAGlobal:{
    if[not hasFfi[]; :noFfi[]];
    $[isLinux[];
      .qunit.assertTrue[0<.ffi.cvar ("r";`stdout); "cvar reads a C global as a raw pointer"];
      .qunit.assertTrue[1b; "stdout is spelled __stdoutp on macOS and absent on Windows — not asserted here"]]};

testSetErrno:{
    if[not hasFfi[]; :noFfi[]];
    .qunit.assertEquals[{.ffi.setErrno 7i; .ffi.setErrno[]} []; 7i; "setErrno sets, and reads back with no argument"];
    .qunit.assertEquals[{.ffi.setErrno 7i; .ffi.setErrno 9i} []; 7i; "and answers the PREVIOUS value, not the new one"]};

/ ---- callbacks: a q function crossing to C under the k letter ----

/ locals are never named x/y/z: they are q's implicit parameters, and qunit
/ rejects a test whose valence is not 1
testCallbackSortsAVector:{
    if[not hasFfi[]; :noFfi[]];
    cmp:{(x>y)-x<y};
    v:3 1 2i;
    .ffi.callFunction[(" ";`qsort)] (v;`int$count v;4i;(cmp;"II";"i");::);
    .qunit.assertEquals[v; 1 2 3i; "qsort calls back into q for every comparison"];
    d:2 2 -1i;
    .ffi.callFunction[(" ";`qsort)] (d;`int$count d;4i;(cmp;"II";"i");::);
    .qunit.assertEquals[d; -1 2 2i; "negatives and duplicates sort correctly"]};

testCallbackOverOtherTypes:{
    if[not hasFfi[]; :noFfi[]];
    cmp:{(x>y)-x<y};
    b:101b;
    .ffi.callFunction[(" ";`qsort)] (b;`int$count b;1i;(cmp;"BB";"i");::);
    .qunit.assertEquals[b; 011b; "a boolean vector sorts at width 1"];
    s:`c`a`b;
    .ffi.callFunction[(" ";`qsort)] (s;`int$count s;.ffi.ptrsize[];(cmp;"SS";"i");::);
    .qunit.assertEquals[s; `a`b`c; "a symbol vector crosses as char*[] and its permutation is written back"]};

testCallbackThroughBind:{
    if[not hasFfi[]; :noFfi[]];
    cmp:{(x>y)-x<y};
    qs:.ffi.bind[`qsort;"liik";" "];
    u:5 4 1 3 2i;
    qs (u;`int$count u;4i;(cmp;"II";"i");::);
    .qunit.assertEquals[u; 1 2 3 4 5i; "the k letter carries a callback through bind too"]};

/ a callback may issue its OWN foreign call while C is on the stack
testCallbackReentrancy:{
    if[not hasFfi[]; :noFfi[]];
    nest:{[x;y] .ffi.callFunction[("i";`abs)] (x-y;::); (x>y)-x<y};
    qs:.ffi.bind[`qsort;"liik";" "];
    w:9 7 8i;
    qs (w;`int$count w;4i;(nest;"II";"i");::);
    .qunit.assertEquals[w; 7 8 9i; "a callback that calls C itself is fine"]};

/ PEACHQ EXTENSION, deliberately beyond KX: a q error inside a callback PARKS
/ rather than longjmping through C — the foreign call completes with neutral
/ results and the error then propagates as the call's q result.
testExtensionCallbackErrorContract:{
    if[not hasFfi[]; :noFfi[]];
    qs:.ffi.bind[`qsort;"liik";" "];
    u:5 4 1 3 2i;
    .qunit.assertThrows[qs; (u;`int$count u;4i;({[x;y] '"kaboom"};"II";"i");::); "kaboom"; "a signalled error propagates as the call's result"];
    .qunit.assertEquals[2+2; 4; "and the evaluator is healthy afterwards"];
    .qunit.assertThrows[qs; (u;`int$count u;4i;({[x;y] x+`a};"II";"i");::); "type"; "a class error propagates unmodified"];
    .qunit.assertEquals[count u; 5; "the vector survives with its shape intact"];
    .qunit.assertThrows[qs; (u;`int$count u;4i;({[x;y]`abc};"II";"i");::); "type"; "an unconvertible callback result is 'type, never a faked zero"];
    .qunit.assertThrows[.ffi.callFunction[(" ";`qsort)]; (u;`int$count u;4i;({[x;y]0i};"II";"i");()!();::); "type"; "a bad LATER argument rolls the closure back"];
    cmp:{(x>y)-x<y};
    qs (u;`int$count u;4i;(cmp;"II";"i");::);
    .qunit.assertEquals[u; 1 2 3 4 5i; "and the same binding still works after all of it"]};

/ ---- the type letter against the element type ----

/ the portable half: a correctly typed vector crosses as an in-place data pointer, and the
/ letters sharing one C type stay interchangeable — the table gives i m d u v t one C type
/ between them, so a date vector is as good an int* as an int vector is
testLetterMatchesElement:{
    if[not hasFfi[]; :noFfi[]];
    cp:.ffi.bind[`memcpy;"IIl";"I"];
    dst:4#0i;
    .qunit.assertEquals[cp (dst;1 2 3 4i;16;::); 1i; "the I return dereferences the pointer C was handed"];
    .qunit.assertEquals[dst; 1 2 3 4i; "an int vector under I is written in place"];
    dl:2#0;
    .ffi.bind[`memcpy;"JJl";"J"] (dl;3 4;16;::);
    .qunit.assertEquals[dl; 3 4; "and a long vector under J likewise"];
    dd:2#2000.01.01;
    cp (dd;2000.01.02 2000.01.03;8;::);
    .qunit.assertEquals[dd; 2000.01.02 2000.01.03; "a date IS an int32, so a date vector satisfies I"]};

/ (::) is the one non-vector that stays legal under an uppercase letter: a NULL pointer is what
/ an optional out-parameter is passed, and time(time_t*) accepts NULL and answers the clock
testNullUnderPointerLetter:{
    if[not hasFfi[]; :noFfi[]];
    .qunit.assertTrue[0 < .ffi.bind[`time;"J";"j"] (::;::); "(::) under J passes a NULL pointer, and time answers the epoch seconds"]};

/ both documented string spellings still reach C: a char vector and a symbol ATOM are each a
/ char*, and a symbol VECTOR is the char*[] strtol writes its endptr into
testStringLetterPositives:{
    if[not hasFfi[]; :noFfi[]];
    sl:.ffi.bind[`strlen;"C";"i"];
    .qunit.assertEquals[sl ("12345";::); 5i; "a char vector under C is a char*"];
    .qunit.assertEquals[sl (`abcdef;::); 6i; "and so is a symbol ATOM"];
    / strtol answers a C long, which the l letter reads at size_t width — the same width only
    / where long and size_t agree, so the S positive is asserted on Linux as pipe/close are
    if[isLinux[];
        .qunit.assertEquals[.ffi.bind[`strtol;"CSi";"l"] ("42abc\000";1#`x;10;::); 42; "a symbol vector under S is the char*[] endptr argument"]]};

/ lowercase is untouched, and it has to be: the published qsort idiom passes an int vector
/ under "l", and a count is a LONG atom under "i"
testLowercaseLettersUnchanged:{
    if[not hasFfi[]; :noFfi[]];
    dst:4#0i;
    .ffi.bind[`memcpy;"lll";"l"] (dst;1 2 3 4i;16;::);
    .qunit.assertEquals[dst; 1 2 3 4i; "a vector under lowercase l still crosses as its data pointer"];
    d2:4#0i;
    .ffi.bind[`memcpy;"IIi";"I"] (d2;1 2 3 4i;4*count d2;::);
    .qunit.assertEquals[d2; 1 2 3 4i; "and a long atom under lowercase i still coerces to int"]};

/ PEACHQ STRICTER: the width bug — a long vector under I was read four bytes at a time, and
/ an int vector under J read, and in the out-parameter direction WROTE, past the q object
testStricterWidthMismatch:{
    if[not hasFfi[]; :noFfi[]];
    if[not isPeachFfi[]; :notPeachFfi[]];
    cp:.ffi.bind[`memcpy;"IIl";"I"];
    lng:.ffi.bind[`memcpy;"JJl";"J"];
    .qunit.assertThrows[cp; (4#0i;1 2 3 4;16;::); "type"; "a long vector under I is 'type, not a 4-byte stride over 8-byte cells"];
    .qunit.assertThrows[lng; (2#0;1 2i;16;::); "type"; "an int vector under J is 'type, not a read past the end"];
    .qunit.assertThrows[lng; (2#0i;1 2;16;::); "type"; "and as an out-parameter it is 'type instead of a write past the end"];
    .qunit.assertThrows[cp; (4#0i;1 2 3 4h;8;::); "type"; "a short vector under I is 'type"]};

/ PEACHQ STRICTER: same width, wrong lane — long bit patterns read as doubles, and back
testStricterLaneMismatch:{
    if[not hasFfi[]; :noFfi[]];
    if[not isPeachFfi[]; :notPeachFfi[]];
    .qunit.assertThrows[.ffi.bind[`memcpy;"FFl";"F"]; (2#0f;1 2;16;::); "type"; "a long vector under F is 'type, not bit patterns read as doubles"];
    .qunit.assertThrows[.ffi.bind[`memcpy;"JJl";"J"]; (2#0;1 2f;16;::); "type"; "a float vector under J is 'type"];
    .qunit.assertThrows[.ffi.bind[`memcpy;"IIl";"I"]; (2#2000.01.01;2#0Np;8;::); "type"; "the shared-C-type leniency never crosses a width: a timestamp vector under I is 'type"]};

/ PEACHQ STRICTER: an ATOM under an uppercase letter wrote its VALUE into a slot the call
/ interface declares a pointer, and C then dereferenced it — a crash, not a wrong answer
testStricterAtomUnderPointer:{
    if[not hasFfi[]; :noFfi[]];
    if[not isPeachFfi[]; :notPeachFfi[]];
    cp:.ffi.bind[`memcpy;"IIl";"I"];
    .qunit.assertThrows[cp; (2i;1 2i;8;::); "type"; "an int atom under I is 'type, never an address to dereference"];
    .qunit.assertThrows[cp; (4#0i;2i;8;::); "type"; "in either argument position"]};

/ PEACHQ STRICTER: C is char* and S is char*[], so the two shapes are not interchangeable
testStricterStringLetterShapes:{
    if[not hasFfi[]; :noFfi[]];
    if[not isPeachFfi[]; :notPeachFfi[]];
    .qunit.assertThrows[.ffi.bind[`strlen;"C";"i"]; (2#`ab;::); "type"; "a symbol VECTOR under C is 'type — that shape is char*[]"];
    .qunit.assertThrows[.ffi.bind[`strtol;"CSi";"l"]; ("42abc\000";"zz";10;::); "type"; "a char vector under S is 'type — that shape is char*"]};

/ ---- peachq extensions, named as such ----

/ the sentinel may be dropped when the bound arity says how many arguments
/ there are AND the values cannot collapse into one.  KX always requires it.
testExtensionSentinelOptional:{
    if[not hasFfi[]; :noFfi[]];
    .qunit.assertEquals[.ffi.bind[`strlen;"C";"i"] "12345"; 5i; "a one-argument binding needs no sentinel here"]};

/ a char vector auto-NUL-terminates through a COPY unless it already ends in
/ "\000" — an explicit tail is what keeps the in-place mutation contract.
testExtensionAutoNulTerminates:{
    if[not hasFfi[]; :noFfi[]];
    .qunit.assertEquals[.ffi.bind[`strlen;"C";"i"] ("12345";::); 5i; "a string with no NUL tail is copied and terminated"]};

/ peachq answers bare q classes; KX embeds a message string in the error text.
testExtensionBareErrorClasses:{
    if[not hasFfi[]; :noFfi[]];
    .qunit.assertThrows[.ffi.bind[`no_such_fn_xyz;"";]; " "; "os"; "a symbol that does not resolve is 'os"];
    .qunit.assertThrows[.ffi.bind[`strlen;"q";]; "i"; "type"; "a letter outside the type table is 'type"]};
