/ .fs conformance: the read-safe filesystem subset, plus deletion bounded to the ONE object named.  The fixture is
/ rebuilt before every test and swept away after it, so the suite is order-independent and safe to reload.  It is
/ built with `set` and `0:` and torn down with `hdel`, all standard kdb — nothing here calls .fs to test .fs.
/ Errors are asserted by CLASS through assertThrows, never by pinning a stack trace.
system "d .fsTest";

ROOT:`:fsTestTree;
FULL:`:fsTestTree/full;
EMPTY:`:fsTestTree/empty;
FILE:`:fsTestTree/full/note.txt;
OTHER:`:fsTestTree/full/other.txt;
GONE:`:fsTestTree/nosuchthing;

/ everything directly inside a directory, as full paths; anything that is not a directory holds nothing.
entries:{[dir] $[11h=type k:key dir; ` sv/: dir,/:k; ()]};

/ a BARE directory, which is the one thing this fixture cannot ask for directly: a splayed save creates it, and then
/ whatever the save wrote is stripped back out.  That is how an EMPTY directory gets made in portable q.
newdir:{[dir]
    (` sv dir,`) set ([]a:1 2);
    {@[hdel;x;::]} each .fsTest.entries dir;
    dir};

/ bottom-up removal, two levels deep — the fixture's depth.  Every hdel is trapped because a test may already have
/ deleted part of the tree, and the sweep must not care which.
wipe:{[root]
    kids:.fsTest.entries root;
    {@[hdel;x;::]} each (raze .fsTest.entries each kids),kids,root};

setUpTree:{
    .fsTest.wipe .fsTest.ROOT;
    .fsTest.newdir .fsTest.FULL;
    .fsTest.newdir .fsTest.EMPTY;
    .fsTest.FILE 0: enlist "hello";
    .fsTest.OTHER 0: enlist "hi"};

tearDownTree:{.fsTest.wipe .fsTest.ROOT};

/ THE distinction the whole namespace rests on.  An empty directory and a missing path are BOTH empty, and only their
/ TYPE tells them apart — so a `0=count key p` implementation would report the empty directory as missing.
testEmptyDirectoryIsNotMissing:{
    .qunit.assertEquals[type key .fsTest.EMPTY; 11h; "an empty directory keys to a TYPED empty"];
    .qunit.assertEquals[key .fsTest.EMPTY; `symbol$(); "and that typed empty is `symbol$()"];
    .qunit.assertEquals[type key .fsTest.GONE; 0h; "a missing path keys to a GENERAL empty"];
    .qunit.assertEquals[count each (key .fsTest.EMPTY;key .fsTest.GONE); 0 0; "both are empty, so a count cannot tell them apart"];
    .qunit.assertTrue[.fs.exists .fsTest.EMPTY; "exists reads the TYPE, so the empty directory is there"];
    .qunit.assertEquals[.fs.exists .fsTest.GONE; 0b; "and the missing path is not"];
    .qunit.assertTrue[.fs.isdir .fsTest.EMPTY; "isdir reads it the same way"];
    .qunit.assertEquals[.fs.isdir .fsTest.GONE; 0b; "so the two never collapse together"]};

testExists:{
    .qunit.assertTrue[.fs.exists .fsTest.FILE; "a file exists"];
    .qunit.assertTrue[.fs.exists .fsTest.FULL; "a populated directory exists"];
    .qunit.assertTrue[.fs.exists .fsTest.EMPTY; "an empty directory exists"];
    .qunit.assertEquals[.fs.exists .fsTest.GONE; 0b; "a missing path does not"];
    .qunit.assertEquals[type .fs.exists .fsTest.FILE; -1h; "the answer is a boolean atom"]};

testIsfile:{
    .qunit.assertTrue[.fs.isfile .fsTest.FILE; "a file is a file"];
    .qunit.assertEquals[.fs.isfile .fsTest.FULL; 0b; "a populated directory is not"];
    .qunit.assertEquals[.fs.isfile .fsTest.EMPTY; 0b; "an empty directory is not"];
    .qunit.assertEquals[.fs.isfile .fsTest.GONE; 0b; "a missing path is not"]};

testIsdir:{
    .qunit.assertTrue[.fs.isdir .fsTest.FULL; "a populated directory is a directory"];
    .qunit.assertTrue[.fs.isdir .fsTest.EMPTY; "an empty directory is a directory too"];
    .qunit.assertTrue[.fs.isdir .fsTest.ROOT; "and so is the tree above them"];
    .qunit.assertEquals[.fs.isdir .fsTest.FILE; 0b; "a file is not"];
    .qunit.assertEquals[.fs.isdir .fsTest.GONE; 0b; "a missing path is not"]};

/ The directory row pins the SHAPE and not the number: hcount answers the OS's own entry size, which is neither the
/ total of the contents nor the same value on every filesystem.
testSize:{
    .qunit.assertEquals[.fs.size .fsTest.FILE; 6; "\"hello\" and its newline are six bytes"];
    .qunit.assertEquals[.fs.size .fsTest.OTHER; 3; "and \"hi\" and its newline are three"];
    .qunit.assertEquals[type .fs.size .fsTest.FILE; -7h; "a size is a long"];
    .qunit.assertEquals[type .fs.size .fsTest.EMPTY; -7h; "a directory answers a long rather than erroring"];
    .qunit.assertThat[.fs.size .fsTest.EMPTY; >; 0; "and that long is an entry size, not a content total"];
    .qunit.assertThrows[.fs.size; .fsTest.GONE; "io*"; "a missing path is hcount's own 'io"]};

testRemoveDeletesOneFile:{
    .qunit.assertEquals[.fs.remove .fsTest.FILE; .fsTest.FILE; "a file is removed, and hdel answers its path"];
    .qunit.assertEquals[.fs.exists .fsTest.FILE; 0b; "so the file is gone"];
    .qunit.assertTrue[.fs.isfile .fsTest.OTHER; "its neighbour is untouched — remove deletes the ONE object named"];
    .qunit.assertThrows[.fs.remove; .fsTest.GONE; "io*"; "a missing path is hdel's own 'io"]};

/ THE row: this is what fails if anyone ever makes remove recursive.  A populated directory must survive intact, and
/ the engine's own refusal to recurse is what guarantees it.
testRemoveNeverDestroysADirectory:{
    .qunit.assertThrows[.fs.remove; .fsTest.FULL; "domain*"; "a POPULATED directory is refused"];
    .qunit.assertTrue[.fs.isdir .fsTest.FULL; "and it survives"];
    .qunit.assertEquals[asc key .fsTest.FULL; asc `note.txt`other.txt; "with every one of its files still in it"];
    .qunit.assertThrows[.fs.remove; .fsTest.EMPTY; "domain*"; "an EMPTY directory is refused too — remove is the file verb"];
    .qunit.assertTrue[.fs.isdir .fsTest.EMPTY; "and it survives as well"]};

testRmdir:{
    .qunit.assertEquals[.fs.rmdir .fsTest.EMPTY; .fsTest.EMPTY; "an empty directory is removed, and hdel answers its path"];
    .qunit.assertEquals[.fs.exists .fsTest.EMPTY; 0b; "so it is gone"];
    .qunit.assertThrows[.fs.rmdir; .fsTest.FULL; "io*"; "a POPULATED directory is hdel's own 'io, passed through"];
    .qunit.assertTrue[.fs.isdir .fsTest.FULL; "and it survives intact"];
    .qunit.assertEquals[asc key .fsTest.FULL; asc `note.txt`other.txt; "with its files untouched"];
    .qunit.assertThrows[.fs.rmdir; .fsTest.FILE; "domain*"; "a file is refused — rmdir is the directory verb"];
    .qunit.assertTrue[.fs.isfile .fsTest.FILE; "and the file survives"];
    .qunit.assertThrows[.fs.rmdir; .fsTest.GONE; "io*"; "a missing path is hdel's own 'io"]};

/ Deletion vectorises the same way everything else does: one named object per element, and never a level below.
testVectorisation:{
    p:(.fsTest.FILE;.fsTest.FULL;.fsTest.EMPTY;.fsTest.GONE);
    .qunit.assertEquals[.fs.exists p; 1110b; "exists maps"];
    .qunit.assertEquals[.fs.isfile p; 1000b; "isfile maps"];
    .qunit.assertEquals[.fs.isdir p; 0110b; "isdir maps"];
    .qunit.assertEquals[.fs.size (.fsTest.FILE;.fsTest.OTHER); 6 3; "size maps"];
    .qunit.assertEquals[.fs.remove (.fsTest.FILE;.fsTest.OTHER); .fsTest.FILE,.fsTest.OTHER; "remove maps"];
    .qunit.assertEquals[.fs.rmdir (.fsTest.EMPTY;.fsTest.FULL); .fsTest.EMPTY,.fsTest.FULL; "and rmdir maps once both are empty"];
    .qunit.assertEquals[.fs.exists .fsTest.ROOT; 1b; "the directory above them is not touched"]};

testEveryInputFormCoerces:{
    .qunit.assertTrue[.fs.isdir .fsTest.ROOT; "a canonical path"];
    .qunit.assertTrue[.fs.isdir `fsTestTree; "a plain symbol"];
    .qunit.assertTrue[.fs.isdir "fsTestTree"; "a string"];
    .qunit.assertEquals[.fs.isdir ("fsTestTree";`fsTestTree); 11b; "a mixed list, elementwise"];
    .qunit.assertEquals[.fs.size "fsTestTree/full/note.txt"; 6; "a string reaches the same file a path does"];
    .qunit.assertThrows[.fs.exists; 42; "type*"; "a long is not path-like — .path.path's own refusal"]};

/ Inherited from .path.path and not re-implemented here: a provider handle and a scheme URL are different KINDS of
/ object, which lexical path rules corrupt silently.
testProviderHandlesAndUrlsAreRefused:{
    .qunit.assertThrows[.fs.exists; `:pq:duckdb:mkt:/data/market.duckdb; "domain*"; "a provider connection handle"];
    .qunit.assertThrows[.fs.isdir; `:pq:duckdb:mkt:cfg:trade/; "domain*"; "a provider table handle"];
    .qunit.assertThrows[.fs.isfile; `:s3://bucket/k.csv; "domain*"; "an object-store URL"];
    .qunit.assertThrows[.fs.size; `:tcps://h:5010; "domain*"; "including an IPC handle"];
    .qunit.assertThrows[.fs.remove; `:pq:duckdb:mkt:cfg; "domain*"; "and especially the two that delete"];
    .qunit.assertThrows[.fs.rmdir; `:s3://bucket/; "domain*"; "so neither can ever reach a handle"]};
