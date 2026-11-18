/ A linter for q source.  It reads the text with .pq.syntax and runs none of it but the literal-only parts of
/ built-in operators that LITERAL_ERROR evaluates, and type checks on literals, under reval.  Rules carry KX QLint's
/ IDs, but PARAM_TYPE_MISMATCH, qStudio's, and the hazard rules (valid q you probably did not mean), SYNTAX,
/ LITERAL_ERROR and TYPED_PARAM_ERROR, which are peachq's.  A comment "@qlintsuppress
/ RULE(n)" silences RULE on its own line and the n below it, a bare RULE silences it in the whole file and the rule all
/ matches every rule.  A statement that does not parse reports only that: SYNTAX, or ASSIGN_RESERVED_WORD for an
/ assignment to a reserved word.  A string is always source and a symbol always a path: lint never guesses which.
/ @eg .lint.text "f:{[a;b] a+1}\ng:$[1b;2;0b;3]"
/ @eg .lint.rules[]

/ Every rule: its severity, whether it runs unless opts says otherwise, and what it finds.
/ @eg .lint.rules[]
.lint.rules:{[]
    ([] rule:`SYNTAX`ASSIGN_RESERVED_WORD`LITERAL_ERROR`COND_EVENARGS`TOO_MANY_ARGUMENTS`UNUSED_VAR`DEPRECATED_DATETIME,
            `UNUSED_PARAM`NOT_FILE_SYMBOL`TIME_OF_DAY_COMPARE`COND_TWO_ARG_IF`NESTED_LAMBDA_OUTER_READ`WHERE_SELF_COMPARE,
            `UNDECLARED_VAR`DUPLICATE_COLUMN`TRAP_EAGER_CALL`COND_SAME_BRANCHES`EMPTY_IF`ENGINE_UNSUPPORTED,
            `MALFORMED_SUPPRESSION`PARAM_TYPE_MISMATCH`TYPED_PARAM_ERROR`UNDEFINED_REFERENCE;
        severity:`error`error`error`error`error`warning`warning`info`warning`warning`warning`warning`warning`warning,
            `warning`warning`warning`warning`info`warning`error`error`error;
        on:11111110111110111111110b;
        summary:("code q cannot parse";
            "an assignment to a reserved word, such as cos:12";
            "literal-only code that signals an error whenever it runs";
            "cond $[] with 4, 6 or more arguments, which has no catch-all else";
            "a call with more arguments than its function takes";
            "a local assigned in a function and never read there";
            "a datetime literal, .z.z, .z.Z or a cast: datetime is deprecated in favour of timestamp";
            "a declared parameter the body never reads";
            "not applied to a file symbol, which deletes the file";
            "a timestamp or datetime compared with .z.t, a time of day, which ignores the date";
            "$[test;x] written as an if: with two arguments $ is cast or tok";
            "a nested function reading a local of the function around it, which it cannot see";
            "where c=c on a local c: the column shadows the local, so every row matches";
            "a name a function reads that is not its local and that this file never defines";
            "a column named twice in one select or by phrase";
            "a trap whose first argument is a call, which runs before the trap and is not caught";
            "cond whose every branch is the same expression";
            "if[test] with nothing to run, usually a missing ;";
            "valid q that peachq cannot parse yet, so its statement is not checked";
            "an @qlintsuppress comment that suppresses nothing, or less than it says";
            "a literal argument, or a pattern assignment's literal, that fails a type-checked parameter or pattern";
            "code over a function's typed parameters that signals 'type whatever their values";
            "a reference to a member of a known namespace that nothing defines"))};

/ What one rule finds and why it matters.  A rule .lint.rules[] does not list signals, saying whether it is a KX
/ QLint rule peachq lacks.
/ @param rule (symbol) a rule ID
/ @eg .lint.explain `UNUSED_PARAM
.lint.explain:{[rule] $[rule in key .lint.i.WHY; .lint.i.WHY rule; '.lint.i.not_a_rule rule]};

.lint.i.not_a_rule:{[rule]
    "lint: ",string[rule],$[rule in .lint.i.KX_RULES; " is a KX QLint rule peachq does not implement yet";
        " is not a rule; .lint.rules[] lists them"]};

/ A constant, not a local of .lint.explain, so the trace of an unknown rule shows one line.
.lint.i.WHY:(`SYNTAX`ASSIGN_RESERVED_WORD`LITERAL_ERROR`COND_EVENARGS`UNUSED_PARAM`UNUSED_VAR`DEPRECATED_DATETIME,
    `TOO_MANY_ARGUMENTS`NOT_FILE_SYMBOL`TIME_OF_DAY_COMPARE`COND_TWO_ARG_IF`NESTED_LAMBDA_OUTER_READ`WHERE_SELF_COMPARE,
    `UNDECLARED_VAR`DUPLICATE_COLUMN`TRAP_EAGER_CALL`COND_SAME_BRANCHES`EMPTY_IF`ENGINE_UNSUPPORTED`MALFORMED_SUPPRESSION,
    `PARAM_TYPE_MISMATCH`TYPED_PARAM_ERROR`UNDEFINED_REFERENCE)!(
    "The statement is not valid q: an unclosed bracket or string, a closing bracket with nothing to close, or a ",
        "form q does not accept, such as a k-style unary (a:+1).  The finding is where reading stopped, or for an ",
        "unclosed bracket the one left open: a closer alone on its line (a ; or a comment may follow), lined up with ",
        "an outer bracket's line rather than its own, is read as closing the outer one.  When the error has a name ('params) the ",
        "message says what it means and keeps the name.";
    "An assignment to a name q reserves, such as cos:12 or div:5, which q refuses with 'assign: the statement ",
        "cannot load.  Rename the variable.  With this rule switched off the statement is still reported, as SYNTAX.";
    "Code made only of constants and built-in operators, such as 6+\"hi\", that signals an error every time it ",
        "runs.  The finding covers the smallest such part and names the error.  To stay safe the linter only ",
        "runs operators that compute a value and change nothing, never a name, a lambda, a number or symbol used ",
        "as a function, or numbers big enough to build more than a million items.  A dictionary or table written ",
        "only with literal lists, `a`b!1 2 3 or ([]a:1 2;b:1 2 3), is reported when its lengths differ.";
    "Conditional $ should not be used with an even number of arguments.  $[if;then;elseif;then;else] must end ",
        "with a catch-all else: with 4, 6 or more arguments the last test has no else after it, so when no test ",
        "is true there is no defined result.  An empty argument changes nothing: cond cannot be projected, so ",
        "$[a;b;c;] is cond with four.  Two arguments, $[\"i\";2.3], is the dyadic $ (cast or tok), and $[;x] a ",
        "projection of it; neither is cond, so neither is reported.";
    "A parameter in the signature {[a;b] ...} that the body never names.  Off by default, and info when on: ",
        "callbacks, trap handlers, stubs and protocol signatures legitimately take parameters they ignore, and ",
        "removing one changes the function's arity, which its caller relies on.  Turn it on to hunt bugs, a body ",
        "that reads the wrong name.  A nested lambda does not see the parameters of the lambda around it (the same ",
        "name there is a global), and a dotted name such as b.c is a global path, so neither counts as a use.";
    "A local variable, a name assigned with : inside a function, that the function never reads: dead code, or a ",
        "sign that a later line reads the wrong name.  When a later statement assigns it again, the message names that line.  A ",
        "nested function cannot see it (the same name there is a global), and b.c is a global path, not b.  A ",
        "compound assignment (c,:1) or an amend (c[0]:1) reads it, so does a read before the assignment, and so ",
        "does returning it: c:1 as the last expression is the result.  A qSQL column (select c:a from t) and a ",
        "table column ([] c:1 2) are not locals.";
    "The datetime type (15) is deprecated in favour of timestamp (12).  Found: a datetime literal (0Nz, ",
        "2001.01.01T12:00:00.000), the datetimes .z.z and .z.Z (.z.p and .z.P are their timestamps), and a cast to ",
        "datetime, `datetime$, \"z\"$, \"Z\"$ or 15h$.";
    "A call with more arguments in its brackets than the function takes: it signals 'rank when it runs.  Fewer is ",
        "a projection, never reported.  Only a function whose ranks are certain is judged: a built-in, or a lambda ",
        "this file assigns once, at top level, to a name nothing else in it assigns or names as a symbol, read in ",
        "its \\d context.  A local of the same name, $ (cond), and sum, prd, max and min are never judged.";
    "not applied to a file symbol, not `:f, deletes the file f: on a file symbol not is hdel.  Write hdel when ",
        "that is meant.  not of anything after the symbol (not `:f in x) applies to that result instead.";
    ".z.t and .z.T are times of day.  Compared with a timestamp or datetime (.z.p, .z.z, a literal or a cast), ",
        "only the time of day is compared, and the date is ignored; .z.t minus a timestamp likewise.  Compare like ",
        "with like: .z.p with a timestamp.  Reported only when the other side is surely a timestamp or datetime.";
    "$[test;x] with two arguments is the dyadic $ (cast or tok), not a conditional.  Reported when it reads as ",
        "an if: the second argument signals ($[b;'err]) or the first is a test (x>0, not b, x in y, null x) or a ",
        "boolean (1b), which cast never takes.  ",
        "Write if[test;x].";
    "q has no closures: a function sees its own parameters and locals and the globals, never those of the function ",
        "it is written inside.  {[a] {x+a} each 1 2} reads a global a, not the parameter.  Pass the value in as a ",
        "parameter: {[a] {[a;x] x+a}[a] each 1 2}.  Not reported: a dotted name, a name in a qSQL query (it may be a ",
        "column, or the lambda be read as text), and a name this file defines as a global, which may be the one ",
        "meant.";
    "In a qSQL phrase a name is first a column of the table, so where c=c compares the column c with itself and ",
        "matches every row, even when the function has a parameter or local c.  Rename the local, or compare with ",
        "it another way.  Reported for =, ~ and in, when c is a local of the function.";
    "A name a function reads that is not its parameter or local, nor x, y or z, nor a built-in, and that no file ",
        "linted with it defines, read in its \\d context as q reads it: assigned at top level, with :: or as a ",
        "dotted name, or a symbol that set, upsert or insert names, that a name follows (an infix .q function such as ",
        "mock) or that @[ may amend.  ",
        "It is read as a global and signals when nothing defines it.  Off by default: most code reads globals other ",
        "projects define.  A name in a qSQL query, a namespace this file never defines into, and KX's one-letter ",
        "namespaces (.z, .Q) are not judged, nor any file that changes context in a way lint cannot follow.";
    "A column named twice in one phrase, select a,b,a or select by c,c: the second is a copy, or was meant to be ",
        "another column.  Only a bare name or name:expr is judged; an unnamed expression is not.";
    "A trap, @[f;x;h], .[f;x;h], .Q.trp or .Q.trpd, applies its first argument to the second.  Written @[f x;y;h], ",
        "the call f x runs first, outside the trap, so its error is not caught.  Pass the function and its argument: ",
        "@[f;x;h].  Reported only when f is a function of this file called with all its arguments and its result ",
        "is surely not a function (it ends with ; and returns early only constants), so the call cannot be meant.";
    "Every branch of $[test;a;b], the then and the else of each test, is the same expression (spaces and comments ",
        "aside), so the result never depends on the tests: a branch was probably meant to differ.";
    "if[test] with nothing after the test runs nothing.  A missing ; is the usual cause: if[x :1] reads :1 as ",
        "part of the test instead of a return.  Write if[test;expr].";
    "The statement is q that peachq's parser does not handle yet ('nyi), such as {:[;x]}.  It is not an error in your ",
        "code, and nothing else in the statement is checked.  Info, so it never fails q -lint; switch it off to hide it.";
    "An @qlintsuppress comment that does not do what it says: it names something that is not a rule (NO_SUCH, or a ",
        "lower-case literal_error), a list of rules (A B or A, B: a comment takes one rule, so only A, or nothing, is ",
        "suppressed), or a count that is not a number of lines.  Write one comment per rule, RULE or RULE(n).  Text after ",
        "the rule that is not a rule ID, a reason, is fine.";
    "A parameter written with a type check, {[x:`j] ...}, or a pattern assignment, (a:`j;b):..., signals 'type when ",
        "it runs on a value of another type: a lower-case letter takes an atom, an upper-case one a list, and the ",
        "check is exact, so 1i is not a long.  Reported for a literal argument, f[1i] or g 1, of a function this ",
        "file assigns once at top level (as TOO_MANY_ARGUMENTS judges it) or written in place, called with all its ",
        "arguments; and for a pattern assignment whose right side is a literal.  A list pattern of the wrong ",
        "length is reported too.  The literal is checked by q's own pattern match; the function never runs.";
    "Code inside a function with typed parameters, {[x:`j] x+\"a\"}, that signals 'type whatever values the ",
        "parameters hold: checked with each parameter replaced by an empty value of its type (a null atom, or both ",
        "an empty and a one-item list).  Only operators whose result type follows from their arguments' types, ",
        "such as + < or neg, are judged, and only 'type: an error such as 'length depends on the values.  A ",
        "parameter the function assigns, a name in a qSQL query and the names of a nested function are not judged.";
    "A reference to a member of a known namespace, .Q.gg or .my.sned, that nothing defines: it signals when it runs.  ",
        "Off by default; turn it on in lint.config or opts.  ",
        "Known is a namespace q provides (.q .Q .z .h .j, peachq's library and .lint), whose members lint lists ",
        "itself, or one the project assigns a function into at top level, .my.send:{...} or \\d .my and a function, ",
        "in any file linted with it, unless more files read members it lacks than define functions in it (a library ",
        "the project extends).  A member assigned anywhere is defined: with : or ::, set, an amend or a pattern, and so is a ",
        ".z callback a user may define (.z.pg, .z.ts).  Never judged: a namespace the project only writes into from a ",
        "function, one it also assigns whole (its members may come from the value), a name in the globals option, and a ",
        "name in a string.  A name under \\d .a is .a.name, as q reads it, never the root's; an undotted name is ",
        "UNDECLARED_VAR's.  Lint a file with the others that define its namespaces, or their members are missing.");

/ The findings for q source text, one row each, by position, judged by kdb's grammar whatever the session's \classic
/ (a trailing qSQL comma is SYNTAX).  line and col are 1-based, col counts bytes, and endline/endcol name the last byte
/ a finding covers.  src is always source, even text that names a file: a path is a symbol, for .lint.path.
/ @param src (string) the text of a q file, or a char atom
/ @return (table) ([] line; col; endline; endcol; rule; severity; msg)
/ @eg .lint.text "f:{x+1\ng:$[1b;2;0b;3]"
.lint.text:{[src]
    if[not type[src] in -10 10h; '"lint: .lint.text takes source text (a string); a path is a symbol, for .lint.path"];
    .lint.run[src;::]};

/ The findings for files, and for every .q file under directories, with a leading file column.  Each file takes the
/ settings of the lint.config files above it, as q -lint does.  A path that does not exist signals, naming it.
/ @param p (symbol) a file or directory handle, `:path, or a symbol list of them
/ @eg .lint.path `:src
.lint.path:{[p]
    if[not .lint.i.is_path p; '"lint: .lint.path takes a path symbol (`:src); a string is source text, for .lint.text"];
    .lint.run[p;::]};

/ The findings for source text or paths, with settings: opts maps a rule to 1b (run it), 0b (do not) or a severity,
/ `error, `warning or `info (run it at that severity), over the on and severity columns of .lint.rules[]; any other value
/ is 'domain.  An ID that is no rule this build has is ignored, with a notice on stderr, so stored settings outlive a
/ rule.  Two more keys are options: positions, `utf8 (the default, col and endcol count bytes) or `utf16 (UTF-16 code
/ units, as Java, JavaScript and LSP count, a character past U+FFFF two); and globals, a symbol list of names defined
/ elsewhere, which every rule takes as defined.  Every file one call lints is one project: a name any of them
/ defines is defined for all.  For a path, a file's lint.config settings apply first and opts over them.  A string is
/ source and a symbol a path, always: .lint.run["src/a.q";::] lints the text src/a.q.
/ @param target (any) source text (string), or paths (symbol), as for .lint.text and .lint.path
/ @param opts (dict) rule!boolean or rule!severity, and the options positions and globals
/ @eg .lint.run["f:{[a;b] a}";(enlist `UNUSED_PARAM)!enlist 1b]
/ @eg .lint.run["a:$[1b;2;0b;3]";(enlist `COND_EVENARGS)!enlist `warning]
/ @eg .lint.run["f:{g x}";`UNDECLARED_VAR`globals`positions!(1b;enlist `g;`utf16)]
.lint.run:{[target;opts] .lint.i.run[target;opts;::]};

/ .lint.run with the project's index, its globals and uses tables (.lint.i.index), or (::) for those of the texts it lints.
.lint.i.run:{[target;opts;index]
    defaults:exec rule!severity from .lint.rules[];
    apply:.lint.i.apply defaults;
    opts:$[opts~(::); ()!(); opts];
    ctx:.lint.i.options[key defaults;opts];
    opts:(key[opts] inter key defaults)#opts;
    sev:.lint.i.enabled opts;
    if[type[target] in -10 10h; :first .lint.i.texts[enlist (),target;enlist sev;ctx;index]];
    files:.lint.i.files .lint.i.existing target;
    settings:{[apply;sev;opts;c] apply[apply[sev;c];opts]}[apply;sev;opts] each .lint.i.configs files;
    found:{[f;t] update file:f from t}'[files;.lint.i.texts[{"c"$read1 x} each files;settings;ctx;index]];
    `file xcols raze (enlist update file:`symbol$() from first .lint.i.texts[enlist "";enlist sev;ctx;index]),found};

/ rule!severity of the rules opts runs: those on by default, with opts' settings (rule!setting) over them.
.lint.i.enabled:{[opts]
    rules:.lint.rules[];
    defaults:exec rule!severity from rules;
    .lint.i.apply[defaults;(exec rule from rules where on)#defaults;(key[opts] inter key defaults)#opts]};

/ sev, rule!severity, with settings o over it: 0b drops a rule, 1b runs it at its default severity, a severity runs
/ it at that; a rule defaults lacks, or any other setting, is 'domain.
.lint.i.apply:{[defaults;sev;o]
    v:value o;
    if[not all key[o] in key defaults; '`domain];
    if[not all {$[-1h=type x; 1b; -11h=type x; x in `error`warning`info; 0b]} each v; '`domain];
    (key[o] where v~\:0b) _ sev,((key[o] where v~\:1b)#defaults),(key[o] where -11h=type each v)#o};

/ The options opts sets beside its rules, checked, as positions!encoding and globals!names; a key that is neither gets
/ one notice naming it.
.lint.i.options:{[rules;opts]
    unknown:key[opts] except rules,`positions`globals;
    if[count unknown; -2 "lint: ignoring ",(", " sv string unknown),": not a rule .lint.rules[] lists"];
    o:(`positions`globals!(`utf8;`symbol$())),(key[opts] inter `positions`globals)#opts;
    if[not $[-11h=type o`positions; o[`positions] in `utf8`utf16; 0b]; '`domain];
    names:(),o`globals;
    if[not (11h=type names) or names~(); '`domain];
    @[o;`globals;:;`symbol$names]};

/ The q -lint command line, answering its exit code: args are the words after q, paths and -format text|json|sarif,
/ or -help (--help, -h) for the usage.  Findings and the asked-for usage print to stdout; a notice, or a usage error
/ naming what was wrong, to stderr.  The code is 0 for -help; 2 for a usage error or an input that is missing or
/ cannot be read, whatever else was found; else 1 for a finding of severity error; else 0.
/ @param args (list) the words, each a string
/ @return (long) the exit code
/ @eg .lint.main ("src";"-format";"json")
.lint.main:{[args]
    words:.lint.i.args args except enlist "-lint";
    if[`help~words; -1 .lint.i.USAGE; :0];
    if[10h=type words; -2 "q -lint: ",words; -2 .lint.i.USAGE; :2];
    fmt:first words;
    files:.lint.i.files hsym `$last words;
    cannot_open:{x where {@[{read1 (.path.path x;0;0); 0b};x;1b]} each x};
    unreadable:cannot_open files;
    -2 each "q -lint: cannot read ",/:.path.string unreadable;
    configs:cannot_open {x where .fs.isfile x} distinct raze .lint.i.chain files;
    if[count configs; -2 each "q -lint: cannot read ",/:configs; :2];
    files:files except unreadable;
    found:@[.lint.path;files;{-2 "q -lint: ",x; ::}];
    if[found~(::); :2];
    $[fmt=`json; -1 .j.j update file:.path.string file from found;
        fmt=`sarif; -1 .j.j .lint.i.sarif found;
        .lint.i.report[found;count files]];
    $[count unreadable; 2; `error in found`severity; 1; 0]};

.lint.i.USAGE:("usage: q -lint <path>... [-format text|json|sarif]";
    "Lints each .q file named, and every .q file under each folder named.  Exit 0 clean, 1 for an error finding,";
    "2 for an input it cannot read or a usage error.  Rules are chosen in lint.config files, not by flags:";
    "one line per rule, RULE : true|false[, severity].  In a q session, .lint.rules[] lists the rules.");

/ The words of q -lint as `help, a usage error (a string), or (format;paths).
.lint.i.args:{[args]
    if[any args in ("-help";"--help";"-h"); :`help];
    at:where args~\:"-format";
    fmt:$[count at; `$args 1+last at; `text];
    paths:args (til count args) except at,1+at;
    flags:paths where "-"=first each paths;
    $[count flags; "unknown flag ",first flags;
        not fmt in `text`json`sarif; "-format takes text, json or sarif";
        0=count paths; "no path to lint";
        (fmt;paths)]};

.lint.i.is_path:{[p] (11h=abs type p) or p~()};

/ The paths of target, which must all exist.
.lint.i.existing:{[target]
    if[not .lint.i.is_path target; '"lint: target is source text (a string) or paths (symbols)"];
    missing:{x where not .fs.exists x} (),.path.path target;
    if[count missing; '"lint: no such file or directory: ",.path.string first missing];
    target};

/ KX QLint's rule IDs (kx-vscode server/src/linter/rules.ts), so .lint.explain can tell a rule peachq lacks from a typo.
.lint.i.KX_RULES:`ASSIGN_RESERVED_WORD`COND_EVENARGS`DECLARED_AFTER_USE`GLOBAL_PEACH`INVALID_ADVERB`INVALID_ASSIGN,
    `INVALID_ESCAPE`INVALID_QUKE`OVERWRITE_ARTIFACT`STATEMENT_IN_EXPR`RESERVED_NAME`TOO_MANY_CONSTANTS,
    `TOO_MANY_GLOBALS`UNINDENTED_CODE`TOO_MANY_LOCALS`TOO_MANY_ARGUMENTS`BACKWARD_COMPATIBILITY,
    `CAST_TYPE_NUMERICAL`CONDITIONALLY_DECLARED`DEBUG_FUNCTION`DEPRECATED_DATETIME`DEPRECATED_FUNCTION`EMPTY_IF,
    `FIXED_SEED`FUNCTION_START`INSUFFICIENT_INDENT`INTERNAL`INVALID_FUNCTION`MALFORMED_SUPPRESSION,
    `MISSING_DEPENDENCY`NAME_COLLISION`NEED_EXPLICIT_RETURN`POSSIBLE_RETURN`UNDECLARED_VAR`UNUSED_INTERNAL,
    `UNUSED_PARAM`UNUSED_VAR`RANDOM_GUIDS`UNREACHABLE_CODE`UNEXPECTED_COND_NEWLINE`UNPARENTHESIZED_JOIN,
    `VAR_Q_ERROR`MISSING_SEMICOLON`MALFORMED_RULE`TODO`LINE_LENGTH`DEFAULT_QDOC`INVALID_KIND`INVALID_TYPEDEF,
    `INVALID_TAG`MISSING_OVERVIEW`MISSING_RETURNS`MISSING_TYPE`MULTIPLE_RETURNS`OUT_OF_ORDER_PARAM,
    `PARAM_NOT_IN_CODE`QDOC_TYPE`REDUNDANT_GLOBAL_ASSIGN`UNDOCUMENTED_PARAM`UNUSED_DEPENDENCY;

/ f applied to args as kdb's grammar reads: \classic 1, the session's own setting coming back however f ends.  Lint
/ judges kdb's grammar, never peachq's friendly syntax, so every parse and fold runs here.
.lint.i.classic:{[f;args]
    classic:system "classic";
    system "classic 1";
    r:.[f;args;{[classic;e] system "classic ",string classic; 'e}[classic]];
    system "classic ",string classic;
    r};

/ Per source, its findings.  The sources one call lints are one project, or index is one (.lint.i.index): a global any
/ file defines is defined for all (.lint.i.project).
.lint.i.texts:{[srcs;sevs;ctx;index]
    .lint.i.classic[{[srcs;sevs;ctx;index]
        if[index~(::); index:.lint.i.facts srcs];
        ctx,:.lint.i.project[index;ctx`globals];
        .lint.i.judge[;;ctx]'[srcs;sevs]};(srcs;sevs;ctx;index)]};

/ The globals and uses tables of the index of srcs (.lint.i.index), file the place of each source, without the
/ outline the editor's queries need, and uses only its dotted names, all .lint.i.project reads: a dotted name is
/ never a local, so no lambda's locals are read.  No sources (an empty folder) are a project of one empty text.
.lint.i.facts:{[srcs]
    srcs:$[count srcs; srcs; enlist ""];
    rows:{[at;src]
        n:(.lint.i.parsed src)`nodes;
        dotted:where n[`kind]=`name;
        dotted@:where ("." in/: n[`text] dotted) and not n[`phrase][dotted] or n[`column] dotted;
        (update file:at from .lint.i.globals n;([] name:.lint.i.full_names[n;dotted]; file:at))}'[til count srcs;srcs];
    `globals`uses!(raze rows[;0];raze rows[;1])};

/ What the project defines, for every rule that resolves a name: defined, each name of globals (a .lint.i.globals row
/ per name a file defines) and of names, the globals option; known, those and every namespace above one
/ (.lint.i.known); members, the names of members of namespaces lint cannot name; spaces, the namespaces a missing
/ member is judged in, each one a file assigns a lambda into at top level, unless it or a namespace above it is itself
/ defined, so a value may hold members lint cannot list, or more files read a member of it nothing defines than assign
/ a lambda into it, so it is a library the project extends (a test framework's); provided, the namespaces q provides
/ that are judged, those of peachq's library the project never writes into, as peachq loads none into a namespace its
/ user wrote first.  Kept for the last index and names asked about, as an editor lints every open document against
/ one index.
.lint.i.project:{[index;names]
    held:@[get;`.lint.i.Project;{[e] (::;::;::)}];
    if[(index~first held) and names~held 1; :last held];
    facts:.lint.i.project_of[index;names];
    `.lint.i.Project set (index;names;facts);
    facts};

/ What .lint.i.project answers, computed.
.lint.i.project_of:{[index;names]
    globals:index`globals;
    defined:distinct (exec name from globals where how<>`member),names;
    known:.lint.i.known defined;
    space:{`$"." sv -1_"." vs string x};
    fn:select from globals where how=`fn, {("."=first x) and "." in 1_x} each string name;
    spaces:distinct space each fn`name;
    spaces@:where not {[defined;s] any (s,first .lint.i.prefixes enlist s) in defined}[defined] each spaces;
    lacking:{x where not x[`name] in y}[index`uses;known];
    lacking@:where {("."=first x) and "." in 1_x} each string lacking`name;
    files_in:{[space;t] g:group space each t`name; (`symbol$key g)!`long${count distinct x} each t[`file] value g}[space];
    spaces@:where (0^files_in[lacking] spaces)<=files_in[fn] spaces;
    roots:distinct {`$"." sv 2#"." vs string x} each defined where "."=first each string defined;
    `defined`known`members`spaces`provided!(defined;known;exec distinct name from globals where how=`member;
        spaces;.lint.i.namespaces[] except roots except .lint.i.KDB_SPACES)};

/ Per name, the global it resolves to, as q binds it: full, its name in full by the \d context it is written in
/ (.lint.i.full_names), word as written, known the project's names and the namespaces above them (.lint.i.project).  A
/ name is its full name: under \d .a an undotted g is .a.g, never the root's (owner ruling D34), unless it is a .q
/ member, which every namespace sees (basics/namespaces.md), and the namespace does not hold it.
.lint.i.resolve:{[full;word;known]
    if[not count word; :full];
    plain:not "." in/: string word;
    dotq:` sv/: `.q,/:word;
    ?[plain and (dotq in known) and not full in known; dotq; full]};

/ Per name node of ids, the global it resolves to (.lint.i.resolve).
.lint.i.resolved:{[nodes;ids;known] .lint.i.resolve[.lint.i.full_names[nodes;ids];`$nodes[`text] ids;known]};

/ The project index, built from paths and texts rather than read from disk, so an editor buffer can stand for its file:
/ a dictionary of tables, each led by a file column.  texts ([] file; text; open; broken), open set while an editor
/ holds the file (.lint.ws.set), broken while its text has a syntax error; globals ([] file; name; how), a row per
/ global it defines (.lint.i.globals, the one definition law); defs, its .pq.outline definitions, at and to the byte span of
/ each name; uses ([] file; name; word; start; end), every name it reads or writes as a global (.lint.i.uses).
/ .lint.i.index_put replaces each path's rows; .lint.i.index_drop deletes them.
.lint.i.index:{[paths;texts] .lint.i.index_put[0#/:.lint.i.index_rows[enlist `;enlist ""];paths;texts]};
.lint.i.index_put:{[index;paths;texts] $[count paths; .lint.i.index_drop[index;paths],'.lint.i.index_rows[paths;texts]; index]};
.lint.i.index_drop:{[index;paths] {[paths;t] $[any t[`file] in paths; delete from t where file in paths; t]}[paths] each index};

/ The index rows of paths, one text each, as .lint.i.index describes them.
.lint.i.index_rows:{[paths;texts]
    rows:.lint.i.classic[{{[src]
        parsed:.lint.i.parsed src;
        syn:parsed`syntax;
        nodes:parsed`nodes;
        uses:.lint.i.uses nodes;
        defs:(.pq.i.outline_of[src;syn`nodes])`defs;
        at:(0,1+where src="\n")[defs[`nameline]-1]+defs[`namecol]-1;
        `texts`globals`defs`uses!(([] text:enlist src; open:0b; broken:0<count syn`errors); .lint.i.globals nodes;
            update at, to:at^(exec start!end from nodes where kind=`name) at from defs; uses)} each x};enlist texts];
    keyed:{[path;r] {[path;t] `file xcols update file:path from t}[path] each r}'[paths;rows];
    key[first keyed]!raze each flip value each keyed};

/ The names a file names as globals, a row each: name in full by \d (.lint.i.full_names), word as written, and the
/ name's byte span.  A local of its lambda (.lint.i.locals), a name in a qSQL phrase, where it may be a column, a
/ column and a built-in are none.
.lint.i.uses:{[nodes]
    k:nodes`kind;
    t:nodes`text;
    names:where (k=`name) and not nodes[`phrase] or nodes`column;
    names@:where not (`$t names) in .lint.i.builtins[];
    names@:where not .lint.i.is_local[nodes;.lint.i.locals nodes;names];
    ([] name:.lint.i.full_names[nodes;names]; word:`$t names; start:nodes[`start] names; end:nodes[`end] names)};

/ Per name node of ids, whether it is a local of its lambda as the closure and undeclared rules read one: a
/ parameter, a name the lambda assigns any way, or x y z it names with no signature (loc, .lint.i.locals).
.lint.i.is_local:{[nodes;loc;ids]
    scope:nodes[`scope] ids;
    lambda:?[`params=nodes[`kind] scope; nodes[`parent] scope; scope];
    (scope>=0) and .lint.i.scoped[lambda;nodes[`text] ids] in loc[`params],loc[`any],loc[`implicit]};

/ The nodes of .pq.syntax with the columns every rule reads: left, right and seq (.lint.i.siblings), phrase and query
/ (.lint.i.qsql), line, target (.lint.i.targets) and column, whether a node is one of .lint.i.columns.
.lint.i.nodes:{[src;syn]
    ls:0,1+where src="\n";
    nodes:update line:1+ls bin start from .lint.i.qsql .lint.i.siblings syn`nodes;
    nodes:update target:.lint.i.targets nodes from nodes;
    update column:@[count[nodes]#0b;.lint.i.columns nodes;:;1b] from nodes};

/ `syntax`nodes!(.pq.syntax src; .lint.i.nodes) as kdb's grammar reads src, kept for the last src asked about under
/ the parse state it was read in (.pq.i.parse_state), so the index, the rules and the requests about one version of a
/ document parse it once.
.lint.i.parsed:{[src]
    .lint.i.classic[{[src]
        state:.pq.i.parse_state[];
        held:@[get;`.lint.i.Parsed;{[e] (::;::;::)}];
        if[(src~first held) and state~held 1; :last held];
        syn:.pq.syntax src;
        parsed:`syntax`nodes!(syn;.lint.i.nodes[src;syn]);
        `.lint.i.Parsed set (src;state;parsed);
        parsed};enlist src]};

.lint.i.judge:{[src;sev;ctx]
    on:key sev;
    parsed:.lint.i.parsed src;
    syn:parsed`syntax;
    ls:0,1+where src="\n";
    nodes:parsed`nodes;
    reserved:$[`ASSIGN_RESERVED_WORD in on; `ASSIGN_RESERVED_WORD; `SYNTAX];
    errs:select start, end, rule:`SYNTAX, msg from syn`errors;
    errs:update rule:reserved from errs where msg~\:"'assign";
    errs:update msg:.lint.i.syntax_msg[src]'[start;end;msg] from errs;
    errs:update rule:`ENGINE_UNSUPPORTED, msg:.lint.i.nyi_msg[src]'[start;end;msg] from errs where msg~\:"'nyi";
    errs:update start:.lint.i.left_open[src;ls;nodes;errs] from errs;
    found:raze (enlist 0#errs),{[nodes;ctx;check] check[nodes;ctx]}[nodes;ctx] each .lint.i.checks on inter key .lint.i.checks;
    top:asc exec start from nodes where parent=-1;
    found:errs,found where not (top bin found`start) in top bin errs`start;
    span:{x[`start]+4294967296*x`end};
    found:found where not (found[`rule]=`LITERAL_ERROR) and span[found] in span select from found where rule=`TOO_MANY_ARGUMENTS;
    found:found where not (found[`rule]=`UNDECLARED_VAR) and span[found] in span select from found where rule=`UNDEFINED_REFERENCE;
    tags:.lint.i.tags[src;nodes];
    found,:.lint.i.malformed tags;
    found:found where (found[`rule] in on) and not .lint.i.suppressed[ls;tags;found];
    last_byte:found[`start]|found[`end]-1;
    units:"j"$$[`utf16=ctx`positions; .lint.i.utf16 src; til 1+count src];
    from:ls ls bin found`start;
    to:ls ls bin last_byte;
    `line`col xasc ([] line:1+ls bin found`start; col:1+units[found`start]-units from; endline:1+ls bin last_byte;
        endcol:units[1+last_byte]-units to; rule:found`rule; severity:sev found`rule; msg:found`msg)};

/ ENGINE_UNSUPPORTED's message: 'nyi is peachq's own refusal, of a statement kdb's grammar may well accept.
.lint.i.nyi_msg:{[src;start;end;msg]
    at:trim (end-start)#start _ src;
    "peachq cannot parse this yet, so the statement is not checked (",msg,$[count at; " at ",at; ""],")"};

/ A SYNTAX message in words where basics/errors.md says what the error means, or for a token the parser refused at
/ its position; the error itself stays after the words.
.lint.i.syntax_msg:{[src;start;end;msg]
    if[not "'"=first msg; :msg];
    if[(start=0) and "\357\273\277"~3#src; :"file starts with a UTF-8 byte-order mark; q may not load it (",msg,")"];
    at:(end-start)#start _ src;
    before:start#src;
    name:$[count[at] and all at in .Q.an,"."; at; neg[(reverse before in .Q.an,".")?0b]#before];
    words:`assign`params`globals`locals`limit`branch`char!(
        $[count name; name," is a reserved word and cannot be assigned"; "a reserved word cannot be assigned"];
        "too many parameters, 8 at most";
        "too many global variables in one function";
        "too many local variables in one function";
        "too many constants in one function, or another limit";
        "a branch (if, do, while or $[]) jumps too far";
        "an invalid character, often a non-breaking space");
    token:1_msg;
    phrase:$[(`$token) in key words; words `$token; token~at; at," is not valid here"; ""];
    $[count phrase; phrase," (",msg,")"; msg]};

/ Per error, where it starts.  A "missing ]" names the bracket the parser left open, but a closer alone on its line (a
/ ; or a comment may follow), lined up with that bracket's line and not with its own opener's, was matched to the
/ inner bracket while the indentation says the outer: then the first such inner opener is the one left open.
.lint.i.left_open:{[src;ls;nodes;errs]
    lead:{[src;ls;at] s:ls ls bin at; ((src s+til 256) in " \t")?0b}[src;ls];
    alone:{[src;ls;lead;at]
        rest:(at+1;-1+((ls,1+count src) 1+ls bin at)-at+1) sublist src;
        rest:(min count[rest],ss[rest;" /"],ss[rest;"\t/"])#rest;
        (lead[at]=at-ls ls bin at) and all rest in " \t\r;"}[src;ls;lead];
    {[src;nodes;lead;alone;opened;o;e;msg]
        if[not msg like "missing ?"; :o];
        kinds:("])}"!(`args`block`params;enlist `paren;enlist `lambda)) last msg;
        inner:exec id from nodes where kind in kinds, start>o, end<=e, not start in opened;
        closer:nodes[`end][inner]-1;
        aligned:(src[closer]=last msg) and (alone each closer) and (lead each closer)=lead o;
        inner:inner where aligned and (lead each nodes[`start] inner)<>lead o;
        $[count inner; min nodes[`start] inner; o]}[src;nodes;lead;alone;errs`start]'[errs`start;errs`end;errs`msg]};

/ Every rule but SYNTAX and ASSIGN_RESERVED_WORD, a query over .pq.syntax's nodes (with the columns .lint.i.siblings
/ and .lint.i.qsql add) and the project (.lint.i.project's keys in ctx) answering ([] start; end; rule; msg).
/ COND_EVENARGS: $ then [ is cond in any position (`a$[t;b;c;d] is `a applied to a cond), a comment between them too, unless it has two arguments (the dyadic $, owner
/ ruling 2026-11-17); an empty slot does not make 4 or more a projection, which cond cannot be (ref/cond.md:43).
/ UNUSED_PARAM: a parameter's scope is its params node and a body name's is its lambda, so a nested lambda's names
/ never match, and only the whole name is a use: b.c is a global path.
.lint.i.checks:(`LITERAL_ERROR`COND_EVENARGS`UNUSED_PARAM`UNUSED_VAR`DEPRECATED_DATETIME`TOO_MANY_ARGUMENTS,
    `NOT_FILE_SYMBOL`TIME_OF_DAY_COMPARE`COND_TWO_ARG_IF`NESTED_LAMBDA_OUTER_READ`WHERE_SELF_COMPARE`UNDECLARED_VAR,
    `DUPLICATE_COLUMN`TRAP_EAGER_CALL`COND_SAME_BRANCHES`EMPTY_IF`PARAM_TYPE_MISMATCH`TYPED_PARAM_ERROR`UNDEFINED_REFERENCE)!(
    {[nodes;ctx]
        pieces:.lint.i.pieces nodes;
        text:.lint.i.span_text[nodes;pieces];
        err:.lint.i.innermost[.lint.i.fold;pieces;text];
        failed:where 0<count each err;
        shapes:.lint.i.shapes nodes;
        shape_text:.lint.i.span_text[nodes;shapes];
        short:where {[r] (not first r) and "length"~last r} each .lint.i.fold each shape_text;
        ([] start:pieces[`start][failed],shapes[`start] short; end:pieces[`end][failed],shapes[`end] short;
            rule:`LITERAL_ERROR; msg:(text[failed],'" signals '",/:err failed),shape_text[short],\:" signals 'length")};
    {[nodes;ctx]
        code:exec id from nodes where kind<>`comment;
        verbs:exec id from nodes where kind=`verb, text~\:enlist "$";
        brackets:code 1+code?verbs;
        is_cond:((nodes[`kind] brackets)=`args) and (nodes[`parent] brackets)=nodes[`parent] verbs;
        verbs:verbs where is_cond;
        brackets:brackets where is_cond;
        slots:exec id from nodes where kind=`expr, parent in brackets;
        c:(count each group nodes[`parent] slots) brackets;
        even:(c>=4) and 0=c mod 2;
        ([] start:nodes[`start] verbs where even; end:nodes[`end] brackets where even; rule:`COND_EVENARGS;
            msg:{"$[] with ",string[x]," arguments has no catch-all else: when no test is true there is no defined ",
                "result"} each c where even)};
    {[nodes;ctx]
        names:where nodes[`kind]=`name;
        word:nodes[`text] names;
        scope:nodes[`scope] names;
        decl:where `params=nodes[`kind] scope;
        use:where `lambda=nodes[`kind] scope;
        decl:names decl where not .lint.i.scoped[nodes[`parent] scope decl;word decl] in .lint.i.scoped[scope use;word use];
        ([] start:nodes[`start] decl; end:nodes[`end] decl; rule:`UNUSED_PARAM;
            msg:{"parameter ",x," is never used (callbacks need it to keep their arity; if so, suppress)"}
                each nodes[`text] decl)};
    {[nodes;ctx] .lint.i.unused_var nodes};
    {[nodes;ctx] .lint.i.datetime nodes};
    {[nodes;ctx] .lint.i.too_many[nodes;ctx]};
    {[nodes;ctx] .lint.i.not_file nodes};
    {[nodes;ctx] .lint.i.time_of_day nodes};
    {[nodes;ctx] .lint.i.cond_if nodes};
    {[nodes;ctx] .lint.i.outer_read[nodes;ctx]};
    {[nodes;ctx] .lint.i.self_compare nodes};
    {[nodes;ctx] .lint.i.undeclared[nodes;ctx]};
    {[nodes;ctx] .lint.i.duplicate_column nodes};
    {[nodes;ctx] .lint.i.trap_eager[nodes;ctx]};
    {[nodes;ctx] .lint.i.same_branches nodes};
    {[nodes;ctx] .lint.i.empty_if nodes};
    {[nodes;ctx] .lint.i.param_type[nodes;ctx]};
    {[nodes;ctx] .lint.i.typed_param_error nodes};
    {[nodes;ctx] .lint.i.undefined[nodes;ctx]});

/ Per piece of pieces, the error folding its text signals, "" for none, innermost first: a piece holding one that
/ failed or answered past a million items or in magnitude never runs, so a finding is the innermost one and til floor
/ 10 xexp 9 is never built.  fold answers (1b;value) or (0b;error), as .lint.i.fold does.
.lint.i.innermost:{[fold;pieces;texts]
    stop:pieces[`start] binr pieces`end;
    big:@[{v:raze/[x]; $[1e6<count v; 1b; (abs type v) in 1 4 5 6 7 8 9h; 1e6<max 1f,abs "f"$v; 0b]};;1b];
    run:{[fold;big;texts;stop;s;i]
        if[any s[1] (i+1)+til stop[i]-i+1; :.[s;(1;i);:;1b]];
        r:fold texts i;
        $[first r; .[s;(1;i);:;big last r]; .[.[s;(0;i);:;last r];(1;i);:;1b]]};
    first run[fold;big;texts;stop]/[(count[texts]#enlist "";count[texts]#0b);reverse til count texts]};

/ Which nodes LITERAL_ERROR may evaluate, one boolean each: a node folds when it and every node under it is a literal
/ or a verb .Q.ops[] calls deterministic and free of side effects.  Where recall and safety trade, it does not fold.
/ Never: a ``text literal (it reads names); a token applying a noun on its left (-1 "x" and -1 ,["a";"b"] write, `a 1
/ reads a global); a keyword in brackets, whose arguments may be applied (ssr["a";"a";-1], each[-1;x]); an iterator
/ whose function is not a verb or keyword (ref/iterators.md:155 names each over peach prior scan; fby applies its
/ paren's first item); Over or Scan after anything but a verb ((1+) over 1 never stops); and a verb that can grow its
/ result past its arguments (til where # cross the joins mmu lsq, Scan, each-left and each-right) unless its sizes
/ are literals, so # under an iterator never folds (0 #': (31*31 31 31;0) takes a computed shape).
.lint.i.foldable:{[nodes;left_sibling;right_sibling;bracketed]
    k:nodes`kind;
    word:`$@[count[k]#enlist "";w;:;nodes[`text] w:where k in `verb`adverb`name];
    left:k left_sibling;
    right:k right_sibling;
    literal:`num`sym`str;
    left_literal:left in literal;
    lone:(right in literal) and null right_sibling right_sibling;
    ops:.Q.ops[];
    ok:(k in `num`sym`expr`paren`args`block`comment) or ((k=`str) and not nodes[`text] like "``*") or
        word in exec name from ops where deterministic, not sideeffect;
    ok&:(not left in `num`sym`paren`block`args) or (k=`verb) and not bracketed;
    ok&:not bracketed and not word in exec name from ops where lexclass=`glyph;
    converges:word in `over`scan,`$enlist each "/\\";
    applies:(k=`adverb) or word in `each`peach`prior`fby;
    ok&:?[converges; left=`verb; (not applies) or left in `verb`name`adverb];
    ok&:not (word in `til`where`scan,`$("\\";"/:";"\\:")) and not lone;
    ok&:not (word in `cross`ej`ij`ijf`uj`ujf`asof`mmu`lsq) and not lone and left_literal;
    iterated:(right=`adverb) or word[right_sibling] in `each`peach`prior`over`scan;
    ok&:not (word=`$"#") and not left_literal and not iterated;
    not .lint.i.spread[nodes`parent;not ok]};

/ Per node, whether it or any node under it is one of b.
.lint.i.spread:{[parent;b] {[p;b] @[b;p where b and p>=0;:;1b]}[parent]/[b]};

/ Per node, whether it is a literal token: a number, symbol or string, never a ``text literal (it reads names).
.lint.i.literal_tokens:{[nodes] (nodes[`kind] in `num`sym`str) and not nodes[`text] like "``*"};

/ The spans LITERAL_ERROR evaluates, ([] start; end) by start then outermost first: each foldable tail of an expression
/ that starts at a noun, a keyword or a glyph its brackets apply, and each foldable paren and block.  Two tokens at
/ least, one a verb, and sizes that multiply to a million at most (til 1000000000 is never built).
.lint.i.pieces:{[nodes]
    k:nodes`kind;
    p:nodes`parent;
    starts:nodes`start;
    sib:.lint.i.order nodes;
    left_sibling:nodes`left;
    right_sibling:nodes`right;
    head:{[k;left_sibling;x] ?[k[x]=`adverb; left_sibling x; x]}[k;left_sibling]/[left_sibling where k=`args];
    bracketed:@[count[k]#0b;head where k[head] in `verb`name;:;1b];
    bad:not .lint.i.foldable[nodes;left_sibling;right_sibling;bracketed];
    c:sib where `expr=k p sib;
    c@:iasc p c;
    g:p c;
    w:where bad c;
    after_bad:(til count c)>-1^((reverse g w)!reverse w) g;
    tail:where after_bad and bracketed[c] or not k[c] in `verb`adverb`args;
    boxes:where (k in `paren`block) and not bad;
    s:starts[c tail],starts boxes;
    e:nodes[`end][((reverse g)!reverse c) g tail],nodes[`end] boxes;
    m:1+max 0,e;  / each span one long: distinct of (start;end) pairs is quadratic
    span:{[m;k] flip (k div m;k mod m)}[m] distinct e+m*s;
    leaf:{x iasc y x}[;starts] where k in `num`sym`str`verb`adverb`name;
    tokens:{[leaf;at;s] leaf (at binr s 0)+til 1+(at bin s[1]-1)-at binr s 0}[leaf;starts leaf] each span;
    span:span where .lint.i.small[nodes;tokens] and {[k;i] (1<count i) and any k[i] in `verb`adverb`name}[k] each tokens;
    outermost_first:idesc span[;1];
    span:span outermost_first iasc span[outermost_first;0];
    ([] start:`long$span[;0]; end:`long$span[;1])};

/ The source text of each span of spans, ([] start; end), cut from its top-level node.
.lint.i.span_text:{[nodes;spans]
    roots:{x iasc y x}[;nodes`start] where -1=nodes`parent;
    {[at;t;roots;s] r:roots at[roots] bin s 0; (s[1]-s 0) sublist (s[0]-at r)_t r}[nodes`start;nodes`text;roots]
        each flip spans`start`end};

/ The literal dictionaries and tables LITERAL_ERROR evaluates whole, ([] start; end), though ! and names never fold:
/ k!v with a literal list of two items or more on each side, the whole of both operands, so ! can only be Dict (an
/ atom on its left may be enkey or an internal function); and a table whose every column, key columns too, is
/ name:literal.  Only their 'length is a finding.
.lint.i.shapes:{[nodes]
    literal:.lint.i.literal_tokens nodes;
    bang:.lint.i.dict_literals[nodes;literal];
    table:.lint.i.table_literals[nodes;literal];
    ([] start:`long$nodes[`start][nodes[`left] bang],nodes[`start] table;
        end:`long$nodes[`end][nodes[`right] bang],nodes[`end] table)};

/ The ! of each k!v whose operands are literal lists, two items or more each, with nothing more on either side.
.lint.i.dict_literals:{[nodes;literal]
    kind:nodes`kind;
    text:nodes`text;
    left:nodes`left;
    right:nodes`right;
    is_list:{[kind;text;i] $[kind[i]=`sym; 1<sum text[i]="`"; 1<@[{count parse x};text i;0]]}[kind;text];
    bang:where (kind=`verb) and text~\:enlist "!";
    bang:bang where literal[left bang] and literal[right bang] and null right right bang;
    bang where ((null left left bang) or `verb=kind left left bang) and (is_list each left bang) and is_list each right bang};

/ The parens that are table literals whose every column, key columns too, is name:literal.
.lint.i.table_literals:{[nodes;literal]
    kind:nodes`kind;
    order:.lint.i.order nodes;
    children:order group nodes[`parent] order;
    column:{[kind;text;literal;c] (3=count c) and (`name=kind first c) and (text[c 1]~enlist ":") and literal last c}
        [kind;nodes`text;literal];
    is_table:{[kind;children;column;p]
        first_slot:children first children p;
        if[not `block=kind first first_slot; :0b];
        key_columns:children each children first first_slot;
        (all {[column;c] (0=count c) or column c}[column] each key_columns) and column[1_first_slot] and
            all column each children 1_children p}[kind;children;column];
    parens:where kind=`paren;
    parens where is_table each parens};

/ Per span's tokens, whether its numbers' sizes multiply to a million at most.  A number's size is the product of its
/ items where the span holds # (2 3#0 is 6 items), else its count times its largest, more than where or til can build
/ from it, so 9 9 9 9 9 9 9 9 9 9+`a folds.
.lint.i.small:{[nodes;tokens]
    k:nodes`kind;
    nums:where k=`num;
    items:{1|abs raze @[{"f"$parse x};x;1f]} each nodes[`text] nums;
    shape:@[count[k]#1f;nums;:;prd each 1f,/:items];
    extent:@[count[k]#1f;nums;:;(count each items)*max each 1f,/:items];
    take:(k=`verb) and nodes[`text]~\:enlist "#";
    {[take;shape;extent;i] 1e6>=prd $[any take i; shape; extent] i}[take;shape;extent] each tokens};

/ Evaluates src under reval: (1b;value), or (0b;error) for the error peachq signals.  Text that does not parse, or
/ that does what reval blocks (eval.md:49: as if -b were set, so a write, hdel or hopen is 'access and a global
/ assignment 'noupdate), answers (1b;::): it is never a finding.
/ @param src (string) a piece of q source
.lint.i.fold:{[src]
    tree:@[{(1b;parse x)};src;{(0b;x)}];
    if[not first tree; :(1b;::)];
    r:@[{(1b;reval x)};last tree;{(0b;x)}];
    $[first r; r; (`$first ":" vs last r) in `access`noupdate`restricted; (1b;::); r]};

/ The nodes with three columns every rule reads: left and right, a node's neighbours among its parent's children,
/ comments skipped (0N at either end), and seq, its place in .lint.i.order.
.lint.i.siblings:{[nodes]
    k:nodes`kind;
    p:nodes`parent;
    order:{x iasc y x}[;p] (iasc nodes`start) except where k=`comment;
    left:@[count[k]#0N;order;:;?[(p order)=p prev order;prev order;0N]];
    right:@[count[k]#0N;order;:;?[(p order)=p next order;next order;0N]];
    update left:left, right:right, seq:@[count[k]#0N;order;:;til count order] from nodes};

/ Every node but comments, by parent, then by position.
.lint.i.order:{[nodes] o iasc nodes[`seq] o:where not null nodes`seq};

/ Per node of ids, how many ; slots it holds: 0 for a token.
.lint.i.slot_count:{[nodes;ids] 0^(count each group nodes[`parent] where nodes[`kind]=`expr) ids};

.lint.i.finding:{[nodes;rule;from;to;msg]
    ([] start:`long$nodes[`start] from; end:`long$nodes[`end] to; rule:count[from]#rule; msg:msg)};

/ The names that are columns, not locals: assigned in a qSQL phrase, from select, exec, update or delete to its from,
/ and in a table or dictionary literal, ([] c:1) or ([k:1] v:2).
.lint.i.columns:{[nodes]
    k:nodes`kind;
    p:nodes`parent;
    named:where k=`name;
    word:@[count[k]#`;named;:;`$nodes[`text] named];
    order:.lint.i.order nodes;
    order:order where (p order) in p where word in `select`exec`update`delete;
    phrase:raze {[word;ids]
        m:@[count[ids]#0N;where word[ids] in `select`exec`update`delete;:;1];
        ids where 1=fills @[m;where word[ids]=`from;:;0]}[word] each value order group p order;
    exprs:where k=`expr;
    exprs:exprs iasc nodes[`start] exprs;
    first_expr:exprs first each value group p exprs;
    blocks:where (k=`block) and null nodes`left;
    blocks:blocks where (p blocks) in first_expr where `paren=k p first_expr;
    names:where k=`name;
    phrase,names where (p p names) in blocks,p p blocks};

/ Per node, whether every sibling before it is in chain, a boolean per node; comments are never leading.
.lint.i.leading:{[nodes;chain]
    order:.lint.i.order nodes;
    other:sums not chain order;
    before:other-(not chain order)+fills ?[differ nodes[`parent] order; other-not chain order; 0N];
    @[count[chain]#0b;order;:;0=before]};

/ The branch slots of each cond, $[...] with three arguments or more: every second slot, and the last.
.lint.i.cond_branches:{[nodes]
    k:nodes`kind;
    p:nodes`parent;
    cond:where (k=`args) and ((k=`verb) and nodes[`text] like "$") nodes`left;
    slots:where (k=`expr) and p in cond;
    slots:slots iasc nodes[`start] slots;
    by_cond:group p slots;
    n:(count each by_cond) p slots;
    at:@[count[slots]#0;raze value by_cond;:;raze til each count each value by_cond];
    slots where (3<=n) and (1=at mod 2) or at=n-1};

/ Per node, whether it holds anything but comments.
.lint.i.filled:{[nodes]
    p:nodes`parent;
    @[count[p]#0b;p where (nodes[`kind]<>`comment) and p>=0;:;1b]};

/ lambda!its last body expression, for every lambda.
.lint.i.last_body:{[nodes]
    body:where (nodes[`kind]=`expr) and `lambda=nodes[`kind] nodes`parent;
    body@:iasc nodes[`start] body;
    by_lambda:group nodes[`parent] body;
    key[by_lambda]!body last each value by_lambda};

/ Per node, whether its value is a function's result, read by the caller: the leading c:d: chain of a lambda's last
/ expression, of an explicit return (:c:1), and of each branch of a cond that is itself such a value.
.lint.i.result:{[nodes;assigned]
    k:nodes`kind;
    p:nodes`parent;
    left:nodes`left;
    colon:(k=`verb) and nodes[`text] like ":";
    leading:.lint.i.leading[nodes;colon or @[count[k]#0b;assigned;:;1b]];
    filled:.lint.i.filled nodes;
    last_body:value .lint.i.last_body nodes;
    base:@[count[k]#0b;(last_body where filled last_body),p where colon and null left;:;1b];
    branch:.lint.i.cond_branches nodes;
    is_branch:@[count[k]#0b;branch;:;1b];
    dollar:@[count[k]#0N;branch;:;left p branch];
    outer:@[count[k]#0N;branch;:;p p branch];
    value_of:{[base;is_branch;dollar;outer;leading;v] base or is_branch and v[outer] and leading dollar}
        [base;is_branch;dollar;outer;leading]/[base];
    leading and value_of p};

/ Never one symbol of scope and name: the scope is a node id that an edit above it shifts, and an interned symbol is
/ never freed, so a language server would grow on every edit.
.lint.i.scoped:{[scope;name] flip `scope`name!(`long$scope;count[scope]#`$name)};

/ UNUSED_VAR: a name assigned with : in a lambda's own body (scope) and named nowhere else there.  An amend, a
/ compound assignment (c,:1) and a symbol (a functional qSQL tree may name it) name it; a dotted name, one :: assigns
/ and a column are not locals.
.lint.i.unused_var:{[nodes]
    k:nodes`kind;
    t:nodes`text;
    scope:nodes`scope;
    names:where (k=`name) and `lambda=k scope;
    after:nodes[`right] names;
    plain:names where (t like ":") after;
    syms:where (k=`sym) and `lambda=k scope;
    items:{1_"`" vs x} each t syms;
    named:.lint.i.scoped[raze (count each items)#'scope syms;raze items];
    scoped_of:{[scope;t;i] .lint.i.scoped[scope i;t i]}[scope;t];
    dead:plain where not ("." in/: t plain) or (scoped_of plain) in named,scoped_of names except plain;
    if[not count dead; :.lint.i.finding[nodes;`UNUSED_VAR;`long$();`long$();()]];
    columns:where nodes`column;
    dead:dead where not (dead in columns) or .lint.i.result[nodes;plain] dead;
    dead:dead first each value group scoped_of dead;
    .lint.i.finding[nodes;`UNUSED_VAR;dead;dead;.lint.i.unused_msg[nodes;plain except columns;scoped_of;dead]]};

/ Per dead local, its message: naming its next assignment in a later statement of the same lambda body, which surely
/ runs after it; one in the same statement or another branch of a cond may not.
.lint.i.unused_msg:{[nodes;plain;scoped_of;dead]
    p:nodes`parent;
    k:nodes`kind;
    in_body:(`expr=k p) and `lambda=k p p;
    again:{[p;in_body;plain;local;d]
        later:plain where (local~\:local plain?d) and (p[plain]>p d) and in_body plain;
        $[in_body[d] and count later; first later; 0N]}[p;in_body;plain;scoped_of plain] each dead;
    {[t;line;d;a] t[d],$[null a; " is assigned but never read in this function";
        " is reassigned at line ",string[line a]," and never read"]}[nodes`text;nodes`line]'[dead;again]};

/ The num tokens whose literal is of one of types, temporal types only: a timestamp or datetime literal always holds
/ D, T, z or p (2001.01.01D0, 2001.01.01T0, 0Nz, 0Np), so only those are parsed.
.lint.i.temporal_literals:{[nodes;types]
    nums:where (nodes[`kind]=`num) and nodes[`text] like "*[DTzp]*";
    nums where ({abs type @[parse;x;::]} each nodes[`text] nums) in types};

/ How a cast names datetime and timestamp, as written before $.
.lint.i.DATETIME:("`datetime";"\"z\"";"\"Z\"";"15h");
.lint.i.TIMESTAMP:("`timestamp";"\"p\"";"\"P\"";"12h");

/ The ids of the tokens naming one of types (strings as written), and of those the casts, the ones $ follows.
.lint.i.type_names:{[nodes;types]
    tokens:where nodes[`kind] in `sym`str`num;
    tokens where {[types;text] any types~\:text}[types] each nodes[`text] tokens};
.lint.i.casts:{[nodes;types]
    named:.lint.i.type_names[nodes;types];
    named where ((nodes[`kind]=`verb) and nodes[`text] like "$") nodes[`right] named};

/ DEPRECATED_DATETIME: a datetime literal or .z.z .z.Z (ref/dotz.md:1132, :1147), and a type naming datetime before $
/ or as the first argument of $[...].
.lint.i.datetime:{[nodes]
    k:nodes`kind;
    t:nodes`text;
    literal:.lint.i.temporal_literals[nodes;15h],where (k=`name) and t like ".z.[zZ]";
    typed:.lint.i.type_names[nodes;.lint.i.DATETIME];
    cast:.lint.i.casts[nodes;.lint.i.DATETIME];
    alone:typed where (null nodes[`left] typed) and null nodes[`right] typed;
    slot:nodes[`parent] alone;
    args:nodes[`parent] slot;
    is_dollar:(k=`verb) and t like "$";
    bracket:alone where (k[args]=`args) and (null nodes[`left] slot) and is_dollar nodes[`left] args;
    word:{[t;i] t[i],\:" is a datetime, which is deprecated in favour of timestamp"}[t];
    cast_word:{[t;i] t[i],\:"$ casts to datetime, which is deprecated in favour of timestamp"}[t];
    .lint.i.finding[nodes;`DEPRECATED_DATETIME;literal,cast,bracket;literal,(nodes[`right] cast),bracket;
        word[literal],cast_word[cast],word bracket]};

/ The highest rank .pq.valence gives the value src parses to, 0N for none.  Parsing builds a lambda, never runs it.
.lint.i.most_args:{[src] r:@[{.pq.valence parse x};src;`long$()]; $[count r; max r; 0N]};

/ Per lambda node of lambdas, its .lint.i.most_args, each lambda parsed once however often it is called.
.lint.i.ranks:{[nodes;lambdas] u:distinct lambdas; (u!"j"$.lint.i.most_args each nodes[`text] u) lambdas};

/ Per node of ids, the name it gives in full, by the \d context of its top-level statement: f under \d .a is .a.f.
.lint.i.full_names:{[nodes;ids]
    p:nodes`parent;
    cmds:where nodes[`kind]=`cmd;
    if[count cmds; cmds@:where nodes[`text][cmds] like "\\d *"];
    if[not count cmds; :`$nodes[`text] ids];
    top:{[p;r] ?[-1=p r; r; p r]}[p]/[ids];
    ctx:{$[x~enlist "."; ""; x]} each {first " " vs trim 2_x} each nodes[`text] cmds;
    at:nodes[`start][cmds] bin nodes[`start] top;
    ctx:(ctx,enlist "") ?[at<0; count cmds; at];
    `${$[("."=first y) or 0=count x; y; x,".",y]}'[ctx;nodes[`text] ids]};

/ Per head, whether it names a local of its lambda: a parameter, x y z, or a name the lambda assigns (.lint.i.targets).
.lint.i.local_calls:{[nodes;head]
    k:nodes`kind;
    t:nodes`text;
    scope:nodes`scope;
    params:where (k=`name) and `params=k scope;
    assigns:where (`lambda=k scope) and nodes`target;
    locals:.lint.i.scoped[nodes[`parent] scope params;t params],.lint.i.scoped[scope assigns;t assigns];
    (.lint.i.scoped[scope head;t head] in locals) or (`lambda=k scope head) and (`$t head) in `x`y`z};

/ TOO_MANY_ARGUMENTS: a name or glyph followed by [ with more arguments than the most .pq.valence gives it, its
/ lambda's (.lint.i.callee) or a built-in's, whose name is reserved so never a local; and a lambda applied where it
/ is written.  The k primitives sum prd max min are not judged: their two-argument bracket form is unproven
/ (eval-architecture.md).
.lint.i.too_many:{[nodes;ctx]
    k:nodes`kind;
    t:nodes`text;
    inline:.lint.i.inline_calls nodes;
    head:where (k in `name`verb) and `args=k nodes`right;
    head:head where not (`$t head) in `sum`prd`max`min,`$enlist "$";
    if[not count head; :inline];
    word:`$t head;
    ops:(exec name from .Q.ops[] where lexclass<>`adverb) inter word;
    builtin:ops!"j"$.lint.i.most_args each string ops;
    lambda:.lint.i.callee[nodes;head;ctx`known];
    defined:where not null lambda;
    most:@[builtin word;defined;:;.lint.i.ranks[nodes;lambda defined]];
    given:.lint.i.slot_count[nodes] nodes[`right] head;
    bad:where (not null most) and given>most;
    callee:{$[x~enlist ":"; "assign (:)"; x]} each t head bad;
    msg:{[w;r;g] w," takes ",.lint.i.arguments[r],"; this call passes ",string g}'[callee;most bad;given bad];
    inline,.lint.i.finding[nodes;`TOO_MANY_ARGUMENTS;head bad;nodes[`right] head bad;msg]};

.lint.i.arguments:{[n] string[n]," argument",$[n=1;"";"s"]};

/ TOO_MANY_ARGUMENTS on {...}[...] with every slot filled: an empty slot ({x}[;]) reads as a projection, never judged.
.lint.i.inline_calls:{[nodes]
    k:nodes`kind;
    lambdas:where (k=`lambda) and `args=k nodes`right;
    slots:where (k=`expr) and nodes[`parent] in nodes[`right] lambdas;
    gaps:distinct nodes[`parent] slots where not .lint.i.filled[nodes] slots;
    lambdas@:where not nodes[`right][lambdas] in gaps;
    most:"j"$.lint.i.most_args each nodes[`text] lambdas;
    given:.lint.i.slot_count[nodes] nodes[`right] lambdas;
    bad:where (not null most) and given>most;
    msg:{[r;g] "this lambda takes ",.lint.i.arguments[r],"; this call passes ",string g}'[most bad;given bad];
    .lint.i.finding[nodes;`TOO_MANY_ARGUMENTS;lambdas bad;nodes[`right] lambdas bad;msg]};

/ Per call head of heads, the lambda node lint is certain it applies, else 0N: the one .lint.i.lambda_defs finds for
/ the name it resolves to (.lint.i.resolve over known), unless the name is a local of its lambda (.lint.i.local_calls).
.lint.i.callee:{[nodes;heads;known]
    full:.lint.i.resolved[nodes;heads;known];
    lambda:.lint.i.lambda_defs[nodes;full] full;
    ?[.lint.i.local_calls[nodes;heads]; 0N; lambda]};

/ Whether lint can follow the file's \d context: system only of a literal string that is not "d ...", applied directly
/ or as @[system;"l f.q";h], and no string holding \d (value "\\d .a").
.lint.i.follows:{[nodes]
    k:nodes`kind;
    t:nodes`text;
    str:where k=`str;
    no_cd:$[count str; not any t[str] like "\"\\\\d*"; 1b];
    sys:where k=`name;
    sys@:where t[sys]~\:"system";
    if[not count sys; :no_cd];
    arg:nodes[`right] sys;
    slot:nodes[`parent] sys;
    head:t nodes[`left] nodes[`parent] slot;
    trapped:(null arg) and (null nodes[`left] sys) and (`args=k nodes[`parent] slot) and (head~\:enlist "@") or head~\:enlist ".";
    only:{[nodes;e] c:where (nodes[`parent]=e) and nodes[`kind]<>`comment; $[1=count c; first c; 0N]}[nodes] each nodes[`right] slot;
    arg:?[trapped; only; arg];
    no_cd and all (`str=k arg) and not t[arg] like "\"d*"};

/ name!lambda node of each lambda the file assigns at top level (f:{...}) to a name of called, in full, that it assigns
/ nowhere else (.lint.i.targets) and never names as a symbol (`f set, @[`f;...]; a type check {[x:`f]} names none).  None when lint
/ cannot follow the file's context.
.lint.i.lambda_defs:{[nodes;called]
    k:nodes`kind;
    p:nodes`parent;
    t:nodes`text;
    right:nodes`right;
    after:right right;
    none:(`symbol$())!`long$();
    if[not .lint.i.follows nodes; :none];
    assigns:where nodes`target;
    once:where 1=count each group .lint.i.full_names[nodes;assigns];
    syms:raze {`$1_"`" vs x} each t where (k=`sym) and not `params=k nodes`scope;
    defs:where (k=`name) and (`stmt=k p p) and (null nodes`left) and (((t like ":") or t like "::") right) and
        (`lambda=k after) and null right after;
    full:.lint.i.full_names[nodes;defs];
    keep:(full in called inter once) and not (full in syms) or (`$t defs) in syms;
    none,(full where keep)!after defs where keep};

/ NOT_FILE_SYMBOL: not of a file symbol alone, not `:f or not[`:f], which deletes the file.
.lint.i.not_file:{[nodes]
    k:nodes`kind;
    p:nodes`parent;
    t:nodes`text;
    is_not:{[k;t;i] (`name=k i) and t[i]~\:"not"}[k;t];
    files:where (k=`sym) and (t like "*`:*") and null nodes`right;
    bare:files where is_not nodes[`left] files;
    alone:files where null nodes[`left] files;
    args:p p alone;
    bracket:alone where (`args=k args) and 1=.lint.i.slot_count[nodes] args;
    bracket:bracket where is_not nodes[`left] p p bracket;
    .lint.i.finding[nodes;`NOT_FILE_SYMBOL;(nodes[`left] bare),nodes[`left] p p bracket;bare,p p bracket;
        {"not ",x," deletes the file: on a file symbol not is hdel"} each t bare,bracket]};

/ Per node, a boolean each, the operands TIME_OF_DAY_COMPARE is sure of: time (.z.t .z.T); stamp (.z.p .z.P .z.z
/ .z.Z, a timestamp or datetime literal); cast (a type naming either, before $); whole (a time or stamp its expression,
/ a qSQL comma or from where by ends); paren (a paren of one expression, starting with a cast or a whole stamp).
.lint.i.dated:{[nodes]
    k:nodes`kind;
    p:nodes`parent;
    t:nodes`text;
    left:nodes`left;
    time:(k=`name) and t like ".z.[tT]";
    stamp:((k=`name) and t like ".z.[pPzZ]") or @[count[k]#0b;.lint.i.temporal_literals[nodes;12 15h];:;1b];
    cast:@[count[k]#0b;.lint.i.casts[nodes;.lint.i.DATETIME,.lint.i.TIMESTAMP];:;1b];
    ends:((k=`verb) and t like ",") or (k=`name) and (t like "from") or (t like "where") or t like "by";
    whole:(stamp or time) and (null nodes`right) or ends nodes`right;
    leftmost:where (k<>`comment) and (null left) and p>=0;
    first_child:@[count[k]#0N;p leftmost;:;leftmost];
    inner:first_child first_child;
    paren:(k=`paren) and (1=.lint.i.slot_count[nodes;til count k]) and (cast inner) or (stamp inner) and whole inner;
    `time`stamp`cast`whole`paren!(time;stamp;cast;whole;paren)};

/ TIME_OF_DAY_COMPARE: a comparison of .z.t or .z.T with an operand surely a timestamp or datetime, or .z.t minus one.
/ Each side is one token or one paren: an operand of any other shape is never judged.
.lint.i.time_of_day:{[nodes]
    k:nodes`kind;
    t:nodes`text;
    if[not any (k=`name) and t like ".z.[tT]"; :.lint.i.finding[nodes;`TIME_OF_DAY_COMPARE;`long$();`long$();()]];
    operand:.lint.i.dated nodes;
    verbs:where (k=`verb) and not null nodes`left;
    on_left:nodes[`left] verbs;
    on_right:nodes[`right] verbs;
    dated_left:(operand[`stamp] or operand`paren) on_left;
    dated_right:((operand[`whole] and operand`stamp) or operand`cast) on_right;
    time_left:operand[`time] on_left;
    time_right:(operand[`time] and operand`whole) on_right;
    compare:(`$t verbs) in `$("<";">";"<=";">=";"=";"<>");
    hit:where (compare and (dated_left and time_right) or time_left and dated_right) or
        (t[verbs] like "-") and time_left and dated_right;
    msg:{[t;a;b] t[a]," against ",t[b],": .z.t and .z.T are times of day, so the date is ignored"}[t]'[on_left hit;
        on_right hit];
    .lint.i.finding[nodes;`TIME_OF_DAY_COMPARE;on_left hit;on_right hit;msg]};

/ Whether a slot's tokens c (in order) surely answer a boolean: a boolean literal, never Cast's x (ref/cast.md:14-16);
/ they start with not null all any; or their first verb is a comparison, in, like or within with one noun (and its
/ brackets) on its left, so it is the outermost operation (f x>0 applies f).
.lint.i.is_test:{[k;t;c]
    if[(1=count c) and (`num=k first c) and {(1<count x) and ("b"=last x) and all -1_x in "01"} t first c; :1b];
    if[(`name=k first c) and (`$t first c) in `not`null`all`any; :1b];
    v:first where k[c]=`verb;
    if[null v; :0b];
    tests:`$("<";">";"<=";">=";"=";"<>";"~";"in";"like";"within");
    ((`$t c v) in tests) and (v>0) and (k[first c] in `name`num`sym`str`paren`lambda) and all `args=k 1_v#c};

/ COND_TWO_ARG_IF: $[a;b], both filled, read as an if: b starts with a signal (not '[, a composition), or a is a test.
.lint.i.cond_if:{[nodes]
    k:nodes`kind;
    p:nodes`parent;
    t:nodes`text;
    right:nodes`right;
    dollar:where (k=`verb) and (t like "$") and `args=k right;
    brackets:right dollar;
    slots:{x iasc y x}[;nodes`start] where (k=`expr) and p in brackets;
    by_bracket:slots group p slots;
    pair:where 2=count each by_bracket brackets;
    code:{[o;g;e] o where (g o) in e}[.lint.i.order nodes;p];
    test:code each by_bracket[brackets pair][;0];
    then:code each by_bracket[brackets pair][;1];
    filled:(0<count each test) and 0<count each then;
    signal:{[k;t;right;c] f:first c; (k[f]=`adverb) and (t[f]~enlist "'") and not `args=k right f}[k;t;right] each then;
    hit:pair where filled and signal or .lint.i.is_test[k;t] each test;
    msg:"$[] with two arguments is the dyadic $ (cast or tok), not an if: write if[test;x]";
    .lint.i.finding[nodes;`COND_TWO_ARG_IF;dollar hit;brackets hit;count[hit]#enlist msg]};

/ Per node, a name's text as a symbol, else null.
.lint.i.words:{[nodes] k:nodes`kind; n:where k=`name; @[count[k]#`;n;:;`$nodes[`text] n]};

/ The nodes with two more columns: phrase, whether a node is in a select, by or where phrase, where a name may be a
/ column (basics/qsql.md:105) and x y z are not the implicit parameters (:183), from select, exec, update or delete to
/ from and from where on; and query, whether it is anywhere in a query, its table expression too.
.lint.i.qsql:{[nodes]
    word:.lint.i.words nodes;
    opens:word in `select`exec`update`delete;
    code:?[opens or word=`where; 1; ?[word=`from; 0; 0N]];
    marked:{[opens;code;g] seen:maxs opens g; (g where seen and 1=fills code g;g where seen)}[opens;code] each
        .lint.i.qsql_groups nodes;
    covered:{[nodes;ids]
        ids@:iasc neg nodes[`end] ids;
        ids@:iasc nodes[`start] ids;
        s:nodes[`start] ids;
        e:nodes[`end] ids;
        outermost:e>prev maxs e;
        s@:where outermost;
        e@:where outermost;
        i:s bin nodes`start;
        (i>=0) and nodes[`end]<=e i}[nodes];
    update phrase:covered raze marked[;0], query:covered raze marked[;1] from nodes};

/ Per name node, whether it is assigned or amended: a verb ending in : follows it, or brackets and then one, or it is
/ in a pattern so assigned, (a;b):1 2 (basics/pattern.md:12), in the pattern's own function.
.lint.i.targets:{[nodes]
    k:nodes`kind;
    t:nodes`text;
    right:nodes`right;
    verbs:where k=`verb;
    assigning:@[count[k]#0b;verbs;:;t[verbs] like "*:"];
    named:(k=`name) and assigning[right] or (`args=k right) and assigning right right;
    patterns:where (k=`paren) and assigning right;
    if[not count patterns; :named];
    patterns@:iasc nodes[`start] patterns;
    patterns@:where {x>prev maxs x} nodes[`end] patterns;
    names:where k=`name;
    at:nodes[`start][patterns] bin nodes[`start] names;
    inside:(at>=0) and (nodes[`start][names]<nodes[`end] patterns at) and nodes[`scope][names]=nodes[`scope] patterns at;
    @[named;names where inside;:;1b]};

/ The locals of each lambda as rows of lambda and name (.lint.i.scoped): params, its parameters; plain, a name it
/ assigns with a lone :; any, a name it assigns or amends any way (:: and c+: too), a qSQL or table column never;
/ implicit, x y z it names with no signature, outside a qSQL phrase.  Kept for the last nodes asked about, as the index,
/ the rules and the editor's requests read them for one parse.
.lint.i.locals:{[nodes]
    held:@[get;`.lint.i.Locals;{[e] (::;::)}];
    if[nodes~first held; :last held];
    k:nodes`kind;
    t:nodes`text;
    scope:nodes`scope;
    params:where (k=`name) and `params=k scope;
    named:where (k=`name) and `lambda=k scope;
    assigned:named where not nodes[`column] named;
    plain:assigned where (`verb=k nodes[`right] assigned) and t[nodes[`right] assigned] like ":";
    target:assigned where nodes[`target] assigned;
    implicit:named where ((`$t named) in `x`y`z) and not nodes[`phrase] named;
    implicit:implicit where not (scope implicit) in nodes[`parent] where k=`params;
    scoped:{[scope;t;i] .lint.i.scoped[scope i;t i]}[scope;t];
    loc:`params`plain`any`implicit!(.lint.i.scoped[nodes[`parent] scope params;t params];scoped plain;scoped target;
        scoped implicit);
    `.lint.i.Locals set (nodes;loc);
    loc};

/ The globals the file defines, ([] name; how), a row per name in full by \d: assigned outside any lambda, assigned
/ with :: or as a dotted name anywhere (a qSQL or table column is not one), a symbol set, upsert or insert names (`a
/ set 1, set[`a;1]), a symbol a name follows, as an infix .q function takes it (`a mock 1, qspec's; lint assumes any
/ name there may be one), a symbol alone in a slot of @[ .[ or ![, which may amend it, and what .lint.i.dynamic finds.
/ A symbol is taken in every \d context the file names, as lint does not place it.  how is fn for a lambda assigned
/ outside any lambda, so its namespace's code is here, member for a .lint.i.dynamic member or, in a file whose \d context
/ lint cannot follow (.lint.i.follows), an undotted name, which may be in any namespace; dyn for a name only
/ .lint.i.dynamic finds, which silences a finding but never makes a namespace judged; else def.
.lint.i.globals:{[nodes]
    k:nodes`kind;
    t:nodes`text;
    right:nodes`right;
    target:where nodes`target;
    named:target where (nodes[`scope][target]<0) or (t[right target]~\:"::") or "." in/: t target;
    named:named where not nodes[`column] named;
    full:.lint.i.full_names[nodes;named];
    fn:full where (nodes[`scope][named]<0) and `lambda=k right right named;
    dynamic:.lint.i.dynamic nodes;
    names:distinct full,.lint.i.symbol_defs nodes;
    dyn:(distinct dynamic`names) except names;
    members:distinct dynamic[`members],$[.lint.i.follows nodes; `symbol$(); names where not "." in/: string names];
    ([] name:names,dyn,members; how:(`def`fn names in fn),(count[dyn]#`dyn),count[members]#`member)};

/ The symbols a file defines with set, upsert or insert, an amend, or an infix function (.lint.i.globals), in the root
/ and in every \d context it names.
.lint.i.symbol_defs:{[nodes]
    k:nodes`kind;
    t:nodes`text;
    right:nodes`right;
    set_syms:where (k=`sym) and ((`verb=k right) and t[right] in ("set";"upsert";"insert")) or `name=k right;
    alone:where (k=`sym) and (null right) and null nodes`left;
    args:nodes[`parent] nodes[`parent] alone;
    amend_syms:alone where (`args=k args) and t[nodes[`left] args] in (enlist "@";enlist ".";enlist "!";"set";"upsert";"insert");
    syms:distinct raze {`$1_"`" vs x} each t set_syms,amend_syms;
    contexts:{first " " vs trim 2_x} each t where (k=`cmd) and t like "\\d *";
    contexts:contexts except enlist enlist ".";
    syms,`$raze {[s;c] (c,"."),/:string s}[syms] each contexts};

/ The names a file may define where lint cannot see the assignment: names, every symbol naming a member (`.a.b, which
/ set, an amend, a functional form or a message may define), a namespace's symbol a name is built from (`.a,x or
/ .Q.dd[`.a;x], so its members are computed), a member a test asks for (`m in key `.a) and every member written in a
/ string (value may run it); members, .lint.i.amended.
.lint.i.dynamic:{[nodes]
    k:nodes`kind;
    t:nodes`text;
    member_of:{x where {("."=first x) and "." in 1_x} each x};
    sym:where k=`sym;
    if[count sym; sym@:where t[sym] like "*`.[a-zA-Z]*"];
    names:raze member_of each {1_"`" vs x} each t sym;
    spaces:sym where {not any ".`" in 2_x} each t sym;
    spaces@:where (t[nodes[`right] spaces]~\:enlist ",") or t[nodes[`left] nodes[`parent] nodes[`parent] spaces]~\:".Q.dd";
    asked:sym where (t[nodes[`left] sym]~\:"key") and t[nodes[`left] nodes[`left] sym]~\:"in";
    tested:nodes[`left] nodes[`left] nodes[`left] asked;
    asked@:where `sym=k tested;
    tested@:where `sym=k tested;
    names,:(1_'t spaces),raze {[t;ns;m] (1_t ns),/:".",/:1_"`" vs t m}[t]'[asked;tested];
    str:where k=`str;
    str@:where "." in/: t str;
    names,:raze {[member_of;s] member_of " " vs @[s;where not s in .Q.an,".";:;" "]}[member_of] each t str;
    `names`members!(`$names;.lint.i.amended nodes)};

/ The members a file may amend into a namespace lint cannot name: each symbol alone in the second slot of @[ or .[
/ (three slots or more) whose first slot is not a symbol alone.
.lint.i.amended:{[nodes]
    k:nodes`kind;
    verbs:where k=`verb;
    args:nodes[`right] verbs where nodes[`text][verbs] in (enlist "@";enlist ".");
    args@:where `args=k args;
    if[not count args; :`symbol$()];
    first_slot:where (k=`expr) and (nodes[`parent] in args) and null nodes`left;
    slots:count each group nodes[`parent] where (k=`expr) and nodes[`parent] in args;
    first_slot@:where 3<=slots nodes[`parent] first_slot;
    sole:where (k=`sym) and (null nodes`left) and (null nodes`right) and `expr=k nodes`parent;
    sole:(nodes[`parent] sole)!sole;
    member:sole nodes[`right] first_slot where null sole first_slot;
    `$raze {1_"`" vs x} each nodes[`text] member where not null member};

/ The names a lambda reads, as the closure and undeclared rules judge them: in its own body, outside a qSQL query (a
/ phrase name may be a column, and a translator may read the lambda as text), and not assigned there.
.lint.i.reads:{[nodes]
    k:nodes`kind;
    where (k=`name) and (`lambda=k nodes`scope) and not nodes[`query] or nodes`target};

/ The first of ids for each lambda and name: one finding per function, however often it reads the name.
.lint.i.first_per_scope:{[nodes;ids] ids first each value group .lint.i.scoped[nodes[`scope] ids;nodes[`text] ids]};

/ The lambdas around lambda l, innermost first: the scope chain up from l, without l itself and the -1 that ends it.
.lint.i.enclosing:{[scope;l]
    chain:{x>=0} {[scope;i] scope i}[scope]\ l;
    1_-1_chain};

/ NESTED_LAMBDA_OUTER_READ: a name a nested lambda reads (.lint.i.reads) that a lambda around it declares (a parameter,
/ a plain local, x y z) and it does not: q has no closures (basics/function-notation.md:144), so there it is a global.
/ Never judged: a dotted name, and one resolving (.lint.i.resolve) to a global this file defines, which may be the
/ one meant; a global another file defines does not silence it (owner ruling on #1343).  One per nested lambda and name.
.lint.i.outer_read:{[nodes;ctx]
    k:nodes`kind;
    t:nodes`text;
    scope:nodes`scope;
    none:.lint.i.finding[nodes;`NESTED_LAMBDA_OUTER_READ;`long$();`long$();()];
    if[not any (k=`lambda) and `lambda=k scope; :none];
    loc:.lint.i.locals nodes;
    seen:loc[`params],loc[`plain],loc[`implicit];
    names:.lint.i.reads nodes;
    names:names where ((`$t names) in seen`name) and not "." in/: t names;
    names:names where not .lint.i.is_local[nodes;loc;names];
    lambdas:distinct scope names;
    chains:(lambdas!.lint.i.enclosing[scope] each lambdas) scope names;
    at:raze (count each chains)#'til count names;
    if[not count at; :none];
    hit:names distinct at where .lint.i.scoped[raze chains;t names at] in seen;
    known:.lint.i.known exec name from .lint.i.globals[nodes] where how<>`member;
    hit:.lint.i.first_per_scope[nodes;hit where not .lint.i.resolved[nodes;hit;known] in known];
    .lint.i.finding[nodes;`NESTED_LAMBDA_OUTER_READ;hit;hit;
        {x," belongs to the enclosing function, which a nested function cannot see: here ",x," is a global"} each t hit]};

/ Per expression holding a qSQL keyword, its tokens in order: the phrases rules cut by keyword and comma.
.lint.i.qsql_groups:{[nodes]
    order:.lint.i.order nodes;
    opens:where (nodes[`kind]=`name) and nodes[`text] in ("select";"exec";"update";"delete");
    order@:where nodes[`parent][order] in nodes[`parent] opens;
    value order group nodes[`parent] order};

/ The tokens of a phrase cut at its top-level commas, the commas dropped.
.lint.i.subphrases:{[nodes;r]
    comma:(nodes[`kind][r]=`verb) and nodes[`text][r]~\:enlist ",";
    {[comma;r;p] r p where not comma p}[comma;r] each (0,1+where comma) _ til count r};

/ WHERE_SELF_COMPARE: a where subphrase c=c, c~c or c in c, one name on both sides that is a local of its lambda: the
/ column shadows the local (basics/qsql.md:105), so it compares the column with itself.
.lint.i.self_compare:{[nodes]
    k:nodes`kind;
    t:nodes`text;
    word:.lint.i.words nodes;
    ends:`select`exec`update`delete`from`by`where;
    phrases:raze {[word;ends;g]
        {[word;ends;g;w] r:(1+w)_g; (r?first r where word[r] in ends)#r}[word;ends;g] each where word[g]=`where}[word;ends]
        each .lint.i.qsql_groups nodes;
    pieces:raze .lint.i.subphrases[nodes] each phrases;
    pieces:pieces where 3=count each pieces;
    same:{[k;t;p] (all `name`verb`name=k p) and (t[p 0]~t p 2) and ((`$t p 1) in `$("=";"~";"in")) and not "." in t p 0}[k;t];
    pieces:pieces where same each pieces;
    if[not count pieces; :.lint.i.finding[nodes;`WHERE_SELF_COMPARE;`long$();`long$();()]];
    loc:.lint.i.locals nodes;
    names:first each pieces;
    pieces:pieces where .lint.i.is_local[nodes;loc;names];
    msg:{[t;p] n:t p 0; ($["in"~t p 1; " " sv t p; raze t p])," compares the column ",n,
        " with itself, true on every row: the local ",n," is not seen here"}[t];
    .lint.i.finding[nodes;`WHERE_SELF_COMPARE;first each pieces;last each pieces;msg each pieces]};

/ The names defs defines, each with its prefixes: .a.b.c defines .a.b and .a too.
.lint.i.known:{[defs] distinct defs,raze .lint.i.prefixes defs where "." in/: string defs};

/ Per symbol of names, every prefix of it that names a namespace or dictionary: .a.b.c gives .a and .a.b.
.lint.i.prefixes:{[names]
    {[s]
        parts:"." vs string s;
        heads:(1+til count[parts]-1)#\:parts;
        `${x where 0<count each x} "." sv/: heads} each names};

/ The root names a file may read without defining them: the keywords, the .q functions, the control words and the
/ qSQL keywords.
.lint.i.builtins:{[] (exec name from .Q.ops[]),key[`.q],`if`do`while`select`exec`update`delete`from`by`where`x`y`z};

/ Per read of names (full, the global each resolves to), whether it is this file's to judge: a root name written without a
/ dot, or a name in a namespace the file defines into or names in \d, never KX's one-letter ones
/ (basics/namespaces.md:36).
.lint.i.own_names:{[nodes;names;full;defs]
    t:nodes`text;
    space:{[s] `$"." sv 2#"." vs string s};
    contexts:`${first " " vs trim 2_x} each t where (nodes[`kind]=`cmd) and t like "\\d *";
    spaces:distinct space each (defs where (string defs) like ".*"),contexts except `.;
    unique:distinct full;
    of:(unique!space each unique) full;
    ?[(string full) like ".*"; (of in spaces) and 2<count each string of; not "." in/: t names]};

/ UNDECLARED_VAR: a name a lambda reads (.lint.i.reads) that is not its local, not x y z, not a built-in, and resolves
/ (.lint.i.resolve) to no name the project defines nor any namespace above one (ctx`known): .a.b.c defines it, .a.b
/ and .a.  The project only ever removes a finding: which namespaces are judged stays the file's.
/ Never judged: a name .lint.i.own_names leaves to another file, and a file whose context lint cannot follow.  One per
/ lambda and name.
.lint.i.undeclared:{[nodes;ctx]
    none:.lint.i.finding[nodes;`UNDECLARED_VAR;`long$();`long$();()];
    if[not .lint.i.follows nodes; :none];
    t:nodes`text;
    loc:.lint.i.locals nodes;
    names:.lint.i.reads nodes;
    names:names where not ((`$t names) in .lint.i.builtins[]) or .lint.i.is_local[nodes;loc;names];
    if[not count names; :none];
    full:.lint.i.resolved[nodes;names;ctx`known];
    at:where .lint.i.own_names[nodes;names;full;exec name from .lint.i.globals[nodes] where how in `def`fn] and not full in ctx`known;
    pre:.lint.i.prefixes full at;
    member:$[count pre; any each (0,-1_sums count each pre) _ (raze pre) in ctx`defined; `boolean$()];
    hit:.lint.i.first_per_scope[nodes;names at where not member];
    .lint.i.finding[nodes;`UNDECLARED_VAR;hit;hit;
        {x," is neither a local nor defined in this file: it is read as a global"} each t hit]};

/ UNDEFINED_REFERENCE: a name resolving (.lint.i.resolve) to a member of a known namespace that nothing defines: one
/ of ctx`spaces, which the project defines into, or one q provides (.lint.i.provided).  Every occurrence, at top level
/ or in a function; a name without a leading dot only where lint follows the \d context and never in a qSQL query,
/ whose phrases read it at the session's \d.  An assignment's target is a definition, a string never a name.
.lint.i.undefined:{[nodes;ctx]
    t:nodes`text;
    names:where (nodes[`kind]=`name) and not nodes`target;
    names@:where {("." in x) and not x like "..*"} each t names;
    names@:where not `params=nodes[`kind] nodes[`scope] names;
    names@:where ("."=first each t names) or (not nodes[`query] names) and .lint.i.follows nodes;
    if[not count names; :.lint.i.finding[nodes;`UNDEFINED_REFERENCE;`long$();`long$();()]];
    word:`$t names;
    full:.lint.i.resolved[nodes;names;ctx`known];
    space:{`$"." sv -1_"." vs string x} each full;
    q:.lint.i.provided[space;full;ctx`provided];
    member:{`$last "." vs string x} each full;
    at:where ((space in ctx`spaces) or q`space) and not (full in ctx`known) or (member in ctx`members) or q`member;
    msg:{[w;f;s;q] $[w=f; string w; string[w]," is ",string f],$[w=f; " is not defined in "; ", not defined in "],
        string[s],", a namespace ",$[q; "q provides"; "this project defines into"]}'[word at;full at;space at;q[`space] at];
    .lint.i.finding[nodes;`UNDEFINED_REFERENCE;names at;names at;msg]};

/ Per name of full and its namespace space, whether that namespace is in one of provided (.lint.i.project) and the
/ lint process holds it, and whether the process holds the name, or it is a .z name a session sets (.lint.i.DOTZ).
/ Reading a namespace loads peachq's own library file for it; nothing of the project runs.
.lint.i.provided:{[space;full;provided]
    u:distinct full;
    s:space full?u;
    ours:({`$"." sv 2#"." vs string x} each s) in provided;
    held:@[ours;where ours;:;{@[{99h=type get x};x;0b]} each s where ours];
    has:@[held;where held;:;{@[{get x; 1b};x;0b]} each u where held];
    at:u?full;
    `space`member!(held at;(has or u in .lint.i.DOTZ) at)};

/ The namespaces q provides, whose members the lint process lists itself: kdb's (.lint.i.KDB_SPACES), peachq's
/ library and .lint.
.lint.i.namespaces:{[] .lint.i.KDB_SPACES,`.lint,`$".",/:string .pq.i.files};
.lint.i.KDB_SPACES:`.q`.Q`.z`.h`.j;

/ The .z names a session sets, which the lint process may not hold (ref/dotz.md): the callbacks and settings a user
/ defines, and those q sets in a debugger (.z.ex .z.ey) or while loading a module (.z.m .z.M).
.lint.i.DOTZ:`.z.ac`.z.bm`.z.exit`.z.pc`.z.pd`.z.pg`.z.ph`.z.pi`.z.pm`.z.po`.z.pp`.z.pq`.z.ps`.z.pw`.z.ts`.z.vs`.z.wc,
    `.z.wo`.z.ws`.z.zd`.z.ex`.z.ey`.z.m`.z.M;

/ The column lists and by lists of every select and exec, each its tokens: from the keyword (and its [n]) to from.  A
/ phrase holding another qSQL keyword is left out.
.lint.i.select_phrases:{[nodes]
    k:nodes`kind;
    word:.lint.i.words nodes;
    parts:{[k;word;g;h]
        r:(1+h)_g;
        r:$[`args=k first r; 1_r; r];
        if[not `from in word r; :()];
        r:(r?first r where word[r]=`from)#r;
        if[any word[r] in `select`exec`update`delete; :()];
        at:r?first r where word[r]=`by;
        (at#r;(1+at)_r)}[k;word];
    raze {[word;parts;g] raze parts[g] each where word[g] in `select`exec}[word;parts] each .lint.i.qsql_groups nodes};

/ DUPLICATE_COLUMN: a column named twice in one select or exec phrase (.lint.i.select_phrases), as a bare name or
/ name:expr.  An unnamed expression and a dotted name are never judged.
.lint.i.duplicate_column:{[nodes]
    k:nodes`kind;
    t:nodes`text;
    column:{[k;t;p]
        bare:(1=count p) and `name=k p 0;
        named:(2<count p) and (`name=k p 0) and (`verb=k p 1) and t[p 1]~enlist ":";
        $[(bare or named) and not "." in t p 0; p 0; 0N]}[k;t];
    named:{[column;subs] c:column each subs; c where not null c}[column] each
        .lint.i.subphrases[nodes] each .lint.i.select_phrases nodes;
    dup:raze {[t;c] c where (til count c)<>(t c)?t c}[t] each named;
    .lint.i.finding[nodes;`DUPLICATE_COLUMN;dup;dup;{"column ",x," is named twice in this phrase"} each t dup]};

/ Per lambda node of lambdas, whether its result is surely no function: its last expression is empty (the result is
/ ::) and each explicit return in its own body returns a constant, :() or :1b.
.lint.i.returns_data:{[nodes;lambdas]
    k:nodes`kind;
    returns:where (k=`verb) and (nodes[`text]~\:enlist ":") and null nodes`left;
    value_of:nodes[`right] returns;
    constant:((k value_of) in `num`sym`str) or nodes[`text][value_of]~\:"()";
    constant&:null nodes[`right] value_of;
    bad:distinct nodes[`scope] returns where not constant;
    (not .lint.i.filled[nodes] .lint.i.last_body[nodes] lambdas) and not lambdas in bad};

/ The first argument of every trap with three arguments: @[f;x;h], .[f;x;h], .Q.trp or .Q.trpd.
.lint.i.trap_firsts:{[nodes]
    k:nodes`kind;
    right:nodes`right;
    heads:where (k in `verb`name) and `args=k right;
    heads:heads where nodes[`text][heads] in (enlist "@";enlist ".";".Q.trp";".Q.trpd");
    heads:heads where 3=.lint.i.slot_count[nodes] right heads;
    leftmost:where (k<>`comment) and null nodes`left;
    ({(x y)!y}[nodes`parent] leftmost where (k leftmost)=`expr) right heads};

/ The first arguments of the traps, @[f x;a;h], .[f[x];a;h], .Q.trp or .Q.trpd with three arguments, that apply a name
/ the file assigns a lambda to, f x or f[...] with no empty slot: first_slot, the argument; tokens, its tokens; and
/ argc, how many arguments the call passes.
.lint.i.trap_calls:{[nodes;known]
    k:nodes`kind;
    t:nodes`text;
    right:nodes`right;
    none:`first_slot`tokens`argc!(`long$();();`long$());
    first_slot:.lint.i.trap_firsts nodes;
    if[not count first_slot; :none];
    leftmost:where (k<>`comment) and null nodes`left;
    callee:({(x y)!y}[nodes`parent] leftmost) first_slot;
    named:where (`name=k callee) and not null callee;
    if[not count named; :none];
    assigned:where (k=`name) and (`lambda=k right right) and t[right] in (enlist ":";"::");
    first_slot@:named where .lint.i.resolved[nodes;callee named;known] in .lint.i.full_names[nodes;assigned];
    if[not count first_slot; :none];
    kids:.pq.i.kids nodes;
    tokens:kids first_slot;
    bracket:(2=count each tokens) and `args=k tokens[;1];
    applied:(2<=count each tokens) and not (k tokens[;1]) in `verb`adverb`args;
    complete:{[kids;a] e:kids a; $[1=count e; 1b; all 0<count each kids e]}[kids];
    keep:where (bracket and complete each last each tokens) or applied;
    argc:?[bracket keep; .lint.i.slot_count[nodes] last each tokens keep; 1];
    `first_slot`tokens`argc!(first_slot keep;tokens keep;argc)};

/ TRAP_EAGER_CALL: a trap's first argument (.lint.i.trap_calls) that calls a lambda of the file (.lint.i.callee)
/ with all its arguments, whose result is surely no function (.lint.i.returns_data): the call runs before the trap,
/ which only applies its result.
.lint.i.trap_eager:{[nodes;ctx]
    t:nodes`text;
    calls:.lint.i.trap_calls[nodes;ctx`known];
    if[not count calls`tokens; :.lint.i.finding[nodes;`TRAP_EAGER_CALL;`long$();`long$();()]];
    callee:first each calls`tokens;
    lambda:.lint.i.callee[nodes;callee;ctx`known];
    keep:where not null lambda;
    keep@:where (calls[`argc][keep]=.lint.i.ranks[nodes;lambda keep]) and .lint.i.returns_data[nodes;lambda keep];
    msg:{[t;f;e] (trim t f)," runs before the trap does, so an error in it is not caught: pass ",t[e],
        " and its argument"}[t]'[calls[`first_slot] keep;callee keep];
    .lint.i.finding[nodes;`TRAP_EAGER_CALL;callee keep;last each calls[`tokens] keep;msg]};

/ Per node of ids, the nodes inside it in order (comments skipped) as (kinds;texts), a container's text empty: the
/ expression it is, whatever its spacing.
.lint.i.signature:{[nodes;ids]
    k:nodes`kind;
    code:where k<>`comment;
    code@:iasc neg nodes[`end] code;
    code@:iasc nodes[`start] code;
    starts:nodes[`start] code;
    kinds:k code;
    texts:?[kinds in `name`verb`adverb`num`sym`str`cmd; nodes[`text] code; count[code]#enlist ""];
    {[code;starts;kinds;texts;nodes;s] at:1+code?s; r:at+til 0|(starts binr nodes[`end] s)-at; (kinds r;texts r)}
        [code;starts;kinds;texts;nodes] each ids};

/ COND_SAME_BRANCHES: cond with 3, 5 or more arguments whose branches, every second slot and the last, are one
/ expression token for token.
.lint.i.same_branches:{[nodes]
    branch:.lint.i.cond_branches nodes;
    cond:nodes[`parent] branch;
    odd:1=(.lint.i.slot_count[nodes] cond) mod 2;
    by_cond:group cond where odd;
    if[not count by_cond; :.lint.i.finding[nodes;`COND_SAME_BRANCHES;`long$();`long$();()]];
    ids:(branch where odd) value by_cond;
    sigs:(0,sums -1_count each ids) _ .lint.i.signature[nodes;raze ids];
    hit:key[by_cond] where {(0<count first first x) and 1=count distinct x} each sigs;
    .lint.i.finding[nodes;`COND_SAME_BRANCHES;nodes[`left] hit;hit;
        count[hit]#enlist "every branch of this $[] is the same, so its result does not depend on the test"]};

/ EMPTY_IF: if[test] with nothing after the test, so it runs nothing.
.lint.i.empty_if:{[nodes]
    ifs:where (nodes[`kind]=`name) and `args=nodes[`kind] nodes`right;
    ifs:ifs where nodes[`text][ifs]~\:"if";
    ifs:ifs where 1=.lint.i.slot_count[nodes] nodes[`right] ifs;
    .lint.i.finding[nodes;`EMPTY_IF;ifs;nodes[`right] ifs;
        count[ifs]#enlist "if[] with only a test runs nothing: a ; after the test is probably missing"]};

/ Per node, whether it is a literal value: a literal token (.lint.i.literal_tokens), or a paren of literal values, ()
/ too, holding no comment.
.lint.i.literals:{[nodes;kids]
    k:nodes`kind;
    n:count each kids;
    token:.lint.i.literal_tokens nodes;
    empty:(k=`paren) and (1=n) and 0=n first each kids;
    ok:token or (k=`paren) or (k=`expr) and (1=n) or empty nodes`parent;
    (token or k=`paren) and not .lint.i.spread[nodes`parent;not ok]};

/ Per node of ids, whether a comment lies inside it.
.lint.i.commented:{[nodes;ids]
    comments:nodes[`start] where nodes[`kind]=`comment;
    {[c;s;e] any c within (s;e-1)}[comments]'[nodes[`start] ids;nodes[`end] ids]};

/ Whether the file may hold a type check: a type symbol (.pq.i.type_check) after : that ends its expression, in a
/ signature or an item of a paren.
.lint.i.has_checks:{[nodes]
    k:nodes`kind;
    syms:where (k=`sym) and null nodes`right;
    syms@:where (.pq.i.type_check each nodes[`text] syms) and nodes[`text][nodes[`left] syms]~\:enlist ":";
    any (`params=k nodes[`scope] syms) or `paren=k nodes[`parent] nodes[`parent] syms};

/ Whether slot e is a pattern lint can read (basics/pattern.md): a name, nothing, a list pattern (.lint.i.list_pattern),
/ or one of those then :`c, a type check.  A constant, a dictionary, an index or a filter function, code, is not.
.lint.i.pattern:{[nodes;kids;e]
    c:kids e;
    k:nodes[`kind] c;
    t:nodes[`text] c;
    checked:not null .pq.i.slot_type[k;t];
    c:$[checked; -2_c; c];
    $[0=count c; 1b; 1<count c; 0b; `name=nodes[`kind] first c; 1b;
        `paren=nodes[`kind] first c; .lint.i.list_pattern[nodes;kids;first c]; 0b]};

/ Whether paren p is a list pattern: each item a pattern, and not () (a constant).
.lint.i.list_pattern:{[nodes;kids;p]
    slots:kids p;
    ((1<count slots) or 0<count kids first slots) and all .lint.i.pattern[nodes;kids] each slots};

/ The params nodes whose every slot is a pattern lint can read (.lint.i.pattern), with no comment inside.
.lint.i.readable:{[nodes;kids]
    params:where nodes[`kind]=`params;
    params@:where not .lint.i.commented[nodes;params];
    params where {[nodes;kids;p] all .lint.i.pattern[nodes;kids] each kids p}[nodes;kids] each params};

/ How the engine's own pattern check judges literal source arg against pattern source p, {[p] 1b} applied to it under
/ reval (.lint.i.fold): `ok for a match, its error for a mismatch, and ` when lint cannot tell.
.lint.i.match:{[p;arg]
    r:.lint.i.fold "{[",p,"] 1b}[",arg,"]";
    $[not first r; `$last r; 1b~last r; `ok; `]};

/ PARAM_TYPE_MISMATCH: a literal that fails a pattern holding a type check (.lint.i.match): an argument against its
/ parameter (.lint.i.typed_calls), and the right side of a pattern assignment (.lint.i.pattern_assigns).  A list
/ pattern's 'length is as certain as 'type.
.lint.i.param_type:{[nodes;ctx]
    t:nodes`text;
    none:.lint.i.finding[nodes;`PARAM_TYPE_MISMATCH;`long$();`long$();()];
    if[not .lint.i.has_checks nodes; :none];
    kids:.pq.i.kids nodes;
    literal:.lint.i.literals[nodes;kids];
    calls:.lint.i.typed_calls[nodes;kids;literal;ctx`known];
    assigns:.lint.i.pattern_assigns[nodes;kids;literal];
    n:count calls`arg;
    pattern:trim t calls[`param],assigns`pattern;
    literals:calls[`arg],assigns`value;
    verdict:.lint.i.match'[pattern;t literals];
    hit:where verdict in `type`length;
    against:({"the parameter ",x," of ",y}'[n#pattern;calls`callee],"the pattern ",/:n _pattern) hit;
    what:((n#enlist "the call"),count[assigns`value]#enlist "the assignment") hit;
    .lint.i.finding[nodes;`PARAM_TYPE_MISMATCH;(calls[`arg],assigns`pattern) hit;literals hit;
        {[v;a;w;e] v," does not match ",a,", so ",w," signals '",string e}'[t literals hit;against;what;verdict hit]]};

/ The arguments PARAM_TYPE_MISMATCH judges: param, a slot of a readable signature (.lint.i.readable) holding a type
/ check; arg, the literal passed to it; callee, the name called or "this lambda".  The call applies a name to the
/ lambda .lint.i.callee is certain of, or a lambda written in place, with [...] filling every parameter (an empty
/ slot or fewer is a projection), or by juxtaposition to a literal when it takes one; never in a query, where a name
/ may be a column.
.lint.i.typed_calls:{[nodes;kids;literal;known]
    k:nodes`kind;
    t:nodes`text;
    right:nodes`right;
    readable:.lint.i.readable[nodes;kids];
    heads:where (k in `name`lambda) and not nodes`query;
    heads@:where (`args=k right heads) or literal[right heads] and null right right heads;
    named:where `name=k heads;
    params:((nodes[`parent] readable)!readable) @[heads;named;:;.lint.i.callee[nodes;heads named;known]];
    heads@:where not null params;
    params@:where not null params;
    slot:{[kids;literal;e] c:kids e; $[0=count c; -1; (1=count c) and literal first c; first c; 0N]}[kids;literal];
    args:{[kids;slot;bracket;r] $[bracket; slot each kids r; enlist r]}[kids;slot]'[`args=k right heads;right heads];
    full:where ((count each args)=count each kids params) and not -1 in/: args;
    param:raze kids params full;
    arg:raze args full;
    callee:raze (count each args full)#'enlist each {[k;t;h] $[`lambda=k h; "this lambda"; t h]}[k;t] each heads full;
    keep:where (not null arg) and "`" in/: t param;
    `param`arg`callee!(param keep;arg keep;callee keep)};

/ The pattern assignments PARAM_TYPE_MISMATCH judges: pattern, a list pattern (.lint.i.list_pattern) holding a type
/ check and no comment, first in its expression and assigned with : from value, a literal that ends it.
.lint.i.pattern_assigns:{[nodes;kids;literal]
    t:nodes`text;
    right:nodes`right;
    parens:where (nodes[`kind]=`paren) and null nodes`left;
    parens@:where t[right parens]~\:enlist ":";
    parens@:where literal[right right parens] and null right right right parens;
    parens@:where "`" in/: t parens;
    parens@:where not .lint.i.commented[nodes;parens];
    parens@:where .lint.i.list_pattern[nodes;kids] each parens;
    `pattern`value!(parens;right right parens)};

/ The prototypes of the type a check name:sym names, as source, when the engine's own check (.lint.i.match) passes
/ each, else none: for a lower-case letter c an atom, its null (first "c"$()); for an upper-case one a list, empty and
/ of one item ("c"$() and 1#"c"$()).
.lint.i.prototypes:{[name;sym]
    cast:"(\"",lower[1_sym],"\"$())";
    protos:$[last[sym] in .Q.a; enlist "(first ",cast,")"; (cast;"(1#",cast,")")];
    $[all `ok=.lint.i.match[name,":",sym] each protos; protos; ()]};

/ The typed names of readable signatures (.lint.i.readable), each a name:`c slot or item of one: scoped, its
/ "lambda/name" (.lint.i.scoped); decl, its check as written; protos, .lint.i.prototypes.  A name its signature binds
/ twice ({[x:`j;x]} is legal), that its function assigns, or whose prototypes the engine refuses, is not one.
.lint.i.typed_names:{[nodes;kids]
    k:nodes`kind;
    t:nodes`text;
    decl:where (k=`expr) and nodes[`scope] in .lint.i.readable[nodes;kids];
    decl@:where {[k;t;c] (`name`verb`sym~k c) and enlist[":"]~t c 1}[k;t] each kids decl;
    name:first each kids decl;
    scoped:.lint.i.scoped[nodes[`parent] nodes[`scope] decl;t name];
    bound:where (k=`name) and nodes[`scope] in nodes[`scope] decl;
    twice:{where 1<count each group x} .lint.i.scoped[nodes[`parent] nodes[`scope] bound;t bound];
    keep:where not scoped in twice,.lint.i.locals[nodes]`any;
    protos:.lint.i.prototypes'[t name keep;t last each kids decl keep];
    typed:where 0<count each protos;
    `scoped`decl`protos!(scoped keep typed;trim t decl keep typed;protos typed)};

/ Text with each run of len bytes at at replaced by its with; the runs never overlap.
.lint.i.splice:{[text;at;len;with] o:idesc at; {[s;a;l;w] (a#s),w,(a+l)_s}/[text;at o;len o;with o]};

/ .lint.i.fold of every variant as one: the first's answer when all run or all signal the same error, else (0b;""), a
/ failure that is not certain.
.lint.i.fold_all:{[variants]
    r:.lint.i.fold each variants;
    $[all first each r; first r; (not any first each r) and 1=count distinct last each r; first r; (0b;"")]};

/ The pieces (.lint.i.pieces) with the typed reads taken for literals that hold a read, and the pieces inside them,
/ whose every verb and keyword is atomic, so its arguments' types alone decide its type: never an iterator.
.lint.i.typed_pieces:{[nodes;reads]
    marked:@[nodes`kind;reads;:;`num];
    pieces:.lint.i.pieces @[nodes;`kind;:;marked];
    at:nodes[`start] reads;
    stop:pieces[`start] binr pieces`end;
    held:where (at binr pieces`end)>at binr pieces`start;
    region:where @[count[stop]#0b;raze {x+til y-x}'[held;stop held];:;1b];
    leaf:where marked in `verb`adverb`name;
    leaf@:iasc nodes[`start] leaf;
    atomic:(marked[leaf] in `verb`name) and (`$nodes[`text] leaf) in exec name from .Q.ops[] where family=`atomic;
    pure:{[at;atomic;s;e] all atomic (at binr s)+til (at binr e)-at binr s}[nodes[`start] leaf;atomic];
    pieces region where pure'[pieces[`start] region;pieces[`end] region]};

/ Per piece, its text with each typed read in it (held) replaced by a prototype: a variant per prototype, the v-th
/ of each read's, or its last.
.lint.i.variants:{[nodes;reads;protos;pieces;text;held]
    at:nodes[`start] reads;
    len:nodes[`end][reads]-at;
    {[at;len;protos;text;s;i]
        {[at;len;protos;text;s;i;v] .lint.i.splice[text;at[i]-s;len i;{x y&-1+count x}[;v] each protos i]}
            [at;len;protos;text;s;i] each til max 1,count each protos i}[at;len;protos]'[text;pieces`start;held]};

/ TYPED_PARAM_ERROR: a piece over a function's typed parameters (.lint.i.typed_names) and literals, atomic verbs only
/ (.lint.i.typed_pieces), that signals 'type with the parameters replaced by each of their prototypes.  A name in a
/ query or a nested function is not the parameter; a piece holding a literal piece that fails is never one.
.lint.i.typed_param_error:{[nodes]
    k:nodes`kind;
    t:nodes`text;
    none:([] start:`long$(); end:`long$(); rule:`symbol$(); msg:());
    if[not .lint.i.has_checks nodes; :none];
    typed:.lint.i.typed_names[nodes;.pq.i.kids nodes];
    reads:.lint.i.reads nodes;
    reads@:where .lint.i.scoped[nodes[`scope] reads;t reads] in typed`scoped;
    if[not count reads; :none];
    reads@:iasc nodes[`start] reads;
    of:typed[`scoped]?.lint.i.scoped[nodes[`scope] reads;t reads];
    pieces:.lint.i.typed_pieces[nodes;reads];
    text:.lint.i.span_text[nodes;pieces];
    held:{[at;s;e] (at binr s)+til (at binr e)-at binr s}[nodes[`start] reads]'[pieces`start;pieces`end];
    err:.lint.i.innermost[.lint.i.fold_all;pieces;.lint.i.variants[nodes;reads;typed[`protos] of;pieces;text;held]];
    hit:where (0<count each held) and err~\:"type";
    names:{distinct x y}[typed[`decl] of] each held hit;
    msg:{[s;n] s," signals 'type whatever the value",$[1<count n; "s"; ""]," of ",", " sv n}'[text hit;names];
    ([] start:pieces[`start] hit; end:pieces[`end] hit; rule:`TYPED_PARAM_ERROR; msg:msg)};

/ Every @qlintsuppress in a comment: at, the byte of its @; word, the word after it, and then, the word after that; and
/ the byte after each, word_end and then_end.
.lint.i.tags:{[src;nodes]
    c:select start, text from nodes where kind=`comment, text like "*@qlintsuppress*";
    at:raze c[`start]+ss[;"@qlintsuppress"] each c`text;
    next_word:{[src;from] w:(from;256) sublist src; gap:(w in " \t")?0b; w:gap _ w; (from+gap;(min w?" \t\r\n")#w)};
    word:next_word[src] each at+14;
    then:next_word[src] each word[;0]+count each word[;1];
    ([] at:`long$at; word:word[;1]; word_end:word[;0]+count each word[;1]; then:then[;1];
        then_end:then[;0]+count each then[;1])};

/ One rule per @qlintsuppress, so any text after it is the reader's.  RULE(n) covers the tag's own line and the n after it;
/ a ( with no closing ) suppresses nothing, as MALFORMED_SUPPRESSION says.
.lint.i.suppressed:{[ls;tags;found]
    if[not count tags; :count[found]#0b];
    word:tags`word;
    counted:("(" in/: word) and ")"=last each word;
    rule:?[counted or not "(" in/: word; `${x til x?"("} each word; `];
    span:?[counted; 1+"J"${-1_(1+x?"(")_x} each word; 0W];
    first_line:?[counted; ls bin tags`at; 0];
    covers:{[rule;first_line;span;r;l] any (rule in (r;`all)) and (l>=first_line) and l<first_line+span};
    covers[rule;first_line;span]'[found`rule;ls bin found`start]};

/ MALFORMED_SUPPRESSION: a tag whose word reads as a rule ID (letters, digits and _, holding a _ or in upper case) but
/ suppresses nothing or less than it says: no rule, lower case, a comma list, a count that is no number, or a second
/ rule after it.  Prose after the tag is never judged, nor a KX rule peachq lacks.
.lint.i.malformed:{[tags]
    ids:(exec rule from .lint.rules[]),.lint.i.KX_RULES;
    is_id:{(0<count x) and (all x in .Q.an) and ("_" in x) or x~upper x};
    judge:{[ids;is_id;w;t]
        b:(min w?"(,")#w;
        n:-1_(1+w?"(")_w;
        if[not (is_id b) or b~"all"; :("";0b)];
        if[not (`$b) in ids,`all;
            :(b," is not a rule",$[(`$upper b) in ids; ": rule ids are upper case, ",upper b;
                ", so this @qlintsuppress suppresses nothing"];0b)];
        if["," in w; :(w," is not a rule: @qlintsuppress takes one rule per comment, so it suppresses nothing";0b)];
        if[("(" in w) and not (")"=last w) and (0<count n) and all n in .Q.n;
            :(w," is not RULE(n) with n a number of lines, so it suppresses nothing";0b)];
        tb:(min t?"(,")#t;
        $[(is_id tb) and (`$tb) in ids; ("@qlintsuppress takes one rule per comment: ",t," is not suppressed";1b); ("";0b)]};
    if[not count tags; :([] start:`long$(); end:`long$(); rule:`symbol$(); msg:())];
    r:judge[ids;is_id]'[tags`word;tags`then];
    bad:where 0<count each r[;0];
    ([] start:tags[`at] bad; end:?["b"$r[bad;1]; tags[`then_end] bad; tags[`word_end] bad]; rule:`MALFORMED_SUPPRESSION;
        msg:r[bad;0])};

.lint.i.files:{[paths] raze {$[.fs.isdir x; {.path.join (x;y)}[x] each .fs.walk[x;"*.q"]; enlist x]} each (),.path.path paths};

/ Per file, the lint.config paths that apply to it, outermost first: its folder and each folder above, up to the nearest
/ holding .git (a repository or worktree root), else the filesystem root.
.lint.i.chain:{[files]
    folder:{[cwd;f]
        f:.path.string f;
        parts:"/" vs $[(f like "/*") or f like "?:*"; f; cwd,"/",f];
        {$[y~".."; -1_x; (y~enlist ".") or 0=count y; x; x,enlist y]}/[1#parts;-1_1_parts]}[system "cd"] each files;
    climb:{[dirs]
        above:"/" sv/: (1+til count dirs)#\:dirs;
        (0|last where .fs.exists above,\:"/.git")_above,\:"/lint.config"};
    folders:distinct folder;
    (climb each folders) folders?/:folder};

/ Per file, rule!setting from every lint.config .lint.i.chain names.
.lint.i.configs:{[files]
    chain:.lint.i.chain files;
    present:{x where .fs.isfile x} distinct raze chain;
    .lint.i.merge[present!.lint.i.config each present;chain]};

/ Per chain of lint.config paths, outermost first, rule!setting from the settings (path!rule!setting) of each path in
/ it, a nearer file's line winning for a rule both set.  A path settings lacks is skipped; a rule .lint.rules[] does not
/ list is dropped.
.lint.i.merge:{[settings;chains]
    known:exec rule from .lint.rules[];
    {[known;settings;c]
        merged:(()!()),/settings c where c in key settings;
        (key[merged] inter known)#merged}[known;settings] each chains};

/ One lint.config as rule!setting, each line KX's "RULE : true|false[, severity][ {params}]".  A line it cannot read,
/ and a {params} object, which it does not apply, get a notice on stderr naming the file and line.
.lint.i.config:{[f]
    setting:{[f;n;l]
        l:trim @[l;where l in "\t\r";:;" "];
        brace:l?"{";
        head:brace#l;
        colon:head?":";
        v:trim each "," vs (1+colon)_head;
        level:`$1_v;
        if[not count l; :()!()];
        if[(colon=count head) or (not (`$first v) in `true`false) or (1<count level) or not all level in `error`warning`info;
            -2 f,":",string[n],": not a lint.config setting; ignored";
            :()!()];
        if[brace<count l; -2 f,":",string[n],": rule parameters are not supported; ignored"];
        (enlist `$trim colon#head)!enlist $["false"~first v; 0b; count level; first level; 1b]}[f];
    lines:read0 hsym `$f;
    (()!()),/setting'[1+til count lines;lines]};

/ The text report: file:line:col: severity RULE msg per finding, then the count by severity.
.lint.i.report:{[found;file_count]
    line:{[f] (":" sv (.path.string f`file;string f`line;string f`col)),": ",(" " sv string f`severity`rule)," ",f`msg};
    plural:{[n;word] string[n]," ",word,$[(n=1) or word~"info"; ""; "s"]};
    by_severity:{(where x>0)#x} `error`warning`info!sum each found[`severity]=/:`error`warning`info;
    breakdown:$[count by_severity; " (",(", " sv plural'[value by_severity;string key by_severity]),")"; ""];
    -1 each line each found;
    -1 plural[count found;"finding"],breakdown," in ",plural[file_count;"file"]};

/ Per byte of src and one past its end, the UTF-16 code units before it: a unit per character's first byte, two past
/ U+FFFF.
.lint.i.utf16:{[src] 0,sums (not src within "\200\277")+src>="\360"};

/ The file: URI of path p, its bytes percent-encoded but for letters, digits and -./_~; a relative path stays relative.
.lint.i.file_uri:{[p]
    f:.path.string p;
    escape:{raze .h.hug[.Q.an,"-./_~"] x};
    $["/"=first f; "file://",escape f; f like "?:*"; "file:///",(2#f),escape 2_f; escape f]};

/ SARIF 2.1.0 for findings with a file column: one run, every rule in its driver.  A region's columns count UTF-16
/ code units, SARIF's default, and its end is exclusive.
.lint.i.sarif:{[found]
    text:{enlist[`text]!enlist x};
    rules:{[text;r] `id`shortDescription`fullDescription!(r`rule;text r`summary;text .lint.explain r`rule)}[text]
        each .lint.rules[];
    sources:{x!"c"$read1 each x} distinct found`file;
    utf16:{[sources;f;line;col]
        at:(0,1+where sources[f]="\n")[line-1];
        units:.lint.i.utf16 sources f;
        1+units[at+col-1]-units at}[sources];
    result:{[text;utf16;r]
        region:`startLine`startColumn`endLine`endColumn!(r`line;utf16[r`file;r`line;r`col];r`endline;
            utf16[r`file;r`endline;1+r`endcol]);
        location:enlist[`physicalLocation]!enlist `artifactLocation`region!(enlist[`uri]!enlist .lint.i.file_uri r`file;region);
        `ruleId`level`message`locations!(r`rule;(`error`warning`info!`error`warning`note) r`severity;text r`msg;
            enlist location)}[text;utf16];
    driver:`name`informationUri`rules!("qlint";"https://peachq.org/";rules);
    (`version,(`$"$schema"),`runs)!("2.1.0";"https://json.schemastore.org/sarif-2.1.0.json";
        enlist `tool`results!(enlist[`driver]!enlist driver;result each found))};

/ The workspace index (.lint.ws): the project index (.lint.i.index) of every file an editor holds and every file of
/ the workspace on disk, kept between calls, and the queries an editor asks of it.  An update replaces or deletes one
/ file's rows; a query reads the tables and never runs the code they hold.  Positions are byte offsets into a file's
/ text, end exclusive.

/ An editor's buffer for path: text replaces path's rows and stands for the file until .lint.ws.drop, which
/ .lint.ws.read leaves alone.  While text has a syntax error, a definition the last text had that no statement now
/ gives keeps its last row, so an outline holds still while a line is typed.
/ @param path (symbol) the file, `:path, or the name an editor gives an unsaved buffer
/ @param text (string) the buffer
/ @return (symbol list) the globals whose definition came or went, and the dotted names read that did
/ @eg .lint.ws.set[`:a.q;"f:{[x:`j] x+1}"]
.lint.ws.set:{[path;text]
    if[not (-11h=type path) and 10h=abs type text; '`type];
    p:hsym path;
    .lint.ws.i.update[enlist p;{[p;text;index]
        old:select from index[`defs] where file=p;
        index:.lint.i.index_put[index;enlist p;enlist (),text];
        index[`texts]:update open:1b from index`texts where file=p;
        if[first exec broken from index[`texts] where file=p;
            index[`defs],:select from old where not name in exec name from index[`defs] where file=p];
        index}[p;text]]};

/ Files from disk: each one no editor holds (.lint.ws.set) is indexed again when its text differs from the index's,
/ and its rows are deleted when it is gone or cannot be read.
/ @param paths (symbol list) the files, `:path each
/ @return (symbol list) the globals whose definition came or went, and the dotted names read that did
/ @eg .lint.ws.read `:a.q`:b.q
.lint.ws.read:{[paths]
    disk:.lint.ws.i.disk[.lint.ws.i.index[];hsym (),paths];
    .lint.ws.i.update[disk[`gone],disk`changed;{[d;index] .lint.i.index_put[.lint.i.index_drop[index;d`gone];d`changed;d`texts]}[disk]]};

/ Deletes the rows of paths: an editor closing a buffer, or a file leaving the workspace.  Call .lint.ws.read after it
/ for a closed buffer to fall back to the file on disk.
/ @param paths (symbol list) the files, `:path each
/ @return (symbol list) the globals whose definition came or went, and the dotted names read that did
/ @eg .lint.ws.drop `:a.q
.lint.ws.drop:{[paths] p:hsym (),paths; .lint.ws.i.update[p;.lint.i.index_drop[;p]]};

/ The findings for path's text in the workspace index, as .lint.run gives them with opts: the workspace is the project,
/ so a global any indexed file defines is defined, and a namespace any defines into is known.
/ @param path (symbol) a file the index holds
/ @param opts (dict) as for .lint.run, or (::)
/ @eg .lint.ws.lint[`:a.q;::]
.lint.ws.lint:{[path;opts]
    index:.lint.ws.i.index[];
    .lint.i.run[.lint.ws.i.text[index;hsym path];opts;index]};

/ path's definitions: the rows of .pq.outline's defs, with the byte span of each definition (start, end) and of its
/ name (at, to), and its signature, f[x:`j; y] for a function, else name / kind.  While its text has a syntax error
/ they include the last rows .lint.ws.set keeps.
/ @param path (symbol) a file the index holds
/ @return (table) ([] name; ns; kind; params; types; doc; tags; start; end; at; to; signature)
/ @eg .lint.ws.symbols `:a.q
.lint.ws.symbols:{[path]
    p:hsym path;
    index:.lint.ws.i.index[];
    n:count text:.lint.ws.i.text[index;p];
    ls:0,1+where text="\n";
    rows:select name, ns, kind, params, types, doc, tags, start:n&n^ls[line-1]+col-1, end:n&n^ls[endline-1]+endcol,
        at:n&at, to:n&to from index`defs where file=p;
    `start xasc update signature:.lint.ws.i.signature each rows from rows};

/ The text the workspace index holds for path, "" for none.
/ @param path (symbol) a file
/ @eg .lint.ws.text `:a.q
.lint.ws.text:{[path] .lint.ws.i.text[.lint.ws.i.index[];hsym path]};

/ The files the workspace index holds, open where an editor's buffer stands for the file (.lint.ws.set).
/ @return (table) ([] file; open)
/ @eg .lint.ws.files[]
.lint.ws.files:{[] select file, open from .lint.ws.i.index[]`texts};

/ Where the name at byte at of path is defined: a local's parameter or first assignment in its lambda; a global's
/ definitions by the name it resolves to (.lint.i.resolve, its \d namespace's), this file's first, then open files',
/ then the workspace's.  A built-in has none.
/ @param path (symbol) a file the index holds
/ @param at (long) a byte offset into its text
/ @return (table) ([] file; start; end; name; kind), the span of each definition's name
/ @eg .lint.ws.defs[`:a.q;5]
.lint.ws.defs:{[path;at]
    p:hsym path;
    c:.lint.ws.i.point[p;at];
    index:.lint.ws.i.index[];
    none:([] file:`symbol$(); start:`long$(); end:`long$(); name:`symbol$(); kind:`symbol$());
    if[null c`id; :none];
    if[c`local; :1#select file:p, start, end, name:`$text, kind:`local from (c`nodes) .lint.ws.i.local_sites[c;1b]];
    select file, start:at, end:to, name, kind from .lint.ws.i.definitions[index;p;c]};

/ Every use of the name at byte at of path: a local's inside its own lambda, a global's in every indexed file, each
/ use resolved as the name is (.lint.i.resolve).  def marks a definition.  A name
/ built from a string is never found.
/ @param path (symbol) a file the index holds
/ @param at (long) a byte offset into its text
/ @return (table) ([] file; start; end; def)
/ @eg .lint.ws.refs[`:a.q;5]
.lint.ws.refs:{[path;at]
    p:hsym path;
    c:.lint.ws.i.point[p;at];
    index:.lint.ws.i.index[];
    if[null c`id; :([] file:`symbol$(); start:`long$(); end:`long$(); def:`boolean$())];
    if[c`local; :select file:p, start, end, def:id in .lint.ws.i.local_sites[c;1b] from (c`nodes) .lint.ws.i.local_sites[c;0b]];
    target:(.lint.ws.i.global[index;c])`name;
    uses:select file, start, end from index`uses where target=.lint.i.resolve[name;word;.lint.ws.i.known index];
    rows:update def:(flip (file;start)) in flip index[`defs]`file`at from .lint.ws.i.order[index;p] uses;
    $[.lint.ws.i.fresh[index;rows`file]; .z.s[path;at]; rows]};

/ The hover text, markdown, of the name at byte at of path: a global definition's signature with its parameters'
/ type checks, its doc and its tags; a built-in's help (.help.get).  "" for anything else.
/ @param path (symbol) a file the index holds
/ @param at (long) a byte offset into its text
/ @eg .lint.ws.hover[`:a.q;5]
.lint.ws.hover:{[path;at]
    p:hsym path;
    c:.lint.ws.i.point[p;at];
    index:.lint.ws.i.index[];
    if[(null c`id) or c`local; :""];
    defs:.lint.ws.i.definitions[index;p;c];
    tag:{"*@",string[x`tag],"*",$[null x`param; ""; " `",string[x`param],"`"],$[count x`description; " - ",x`description; ""]};
    if[count defs; r:first defs; :"\n\n" sv (enlist "```q\n",.lint.ws.i.signature[r],"\n```"),$[count r`doc; enlist r`doc; ()],tag each r`tags];
    word:c[`nodes;`text] c`id;
    if[not .lint.ws.i.builtin[index;word]; :""];
    "\n\n" sv {x where 0<count each x} ("```q\n",.lint.ws.i.builtin_sig[word],"\n```";.lint.ws.i.help word)};

/ The call around byte at of path, for signature help: the innermost [ ] after a name or glyph that at is inside (after
/ its [, before its ] or with none yet), label its signature, params its parameters' labels and docs their @param
/ text, active the parameter at is in.  (::) when at is in no call to a global function or built-in.
/ @param path (symbol) a file the index holds
/ @param at (long) a byte offset into its text
/ @return (dict) `label`params`docs`doc`active
/ @eg .lint.ws.sig[`:a.q;5]
.lint.ws.sig:{[path;at]
    p:hsym path;
    c:.lint.ws.i.point[p;at];
    index:.lint.ws.i.index[];
    nodes:c`nodes;
    k:nodes`kind;
    args:where (k=`args) and k[nodes`left] in `name`verb;
    args@:where (nodes[`start][args]<at) and (at<nodes[`end] args) or (at=nodes[`end] args) and not "]"=last each nodes[`text] args;
    args:last args iasc nodes[`start] args;
    if[null args; :(::)];
    c[`id]:nodes[`left] args;
    if[.lint.i.is_local[nodes;c`loc;enlist c`id] 0; :(::)];
    active:0|-1+sum nodes[`start][where (nodes[`parent]=args) and nodes[`kind]=`expr]<=at;
    defs:.lint.ws.i.definitions[index;p;c];
    word:nodes[`text] c`id;
    if[count defs; r:first defs; :`label`params`docs`doc`active!(.lint.ws.i.signature r;.lint.ws.i.params r;
        {[tags;n] first (exec description from tags where param=n),enlist ""}[r`tags] each r`params;r`doc;active)];
    if[not .lint.ws.i.builtin[index;word]; :(::)];
    params:.lint.ws.i.builtin_params word;
    `label`params`docs`doc`active!(word,"[",(";" sv params),"]";params;count[params]#enlist "";.lint.ws.i.help word;active)};

/ The completions at byte at of path, for the name being typed there (from start to at): a local or parameter of the
/ lambda at is in; a definition of the workspace (only this file's when nothing is typed yet), and one of the \d
/ namespace by its short name; a built-in keyword that is a name, never an operator glyph; after a dot, a namespace or
/ its members (the workspace's, and those of .q .Q .z .h .j and peachq's own); in a qSQL query from a table this file
/ defines as a literal, its columns.  Inside a comment or a string, nothing.  Nothing is evaluated.
/ An item's tier is its scope: 0 a local, a parameter or a column, 1 this file's definition, 2 another file's, 3 a
/ built-in; a namespace takes the tier of the closest file that defines into it.  Items come by tier, then by label.
/ @param path (symbol) a file the index holds
/ @param at (long) a byte offset into its text
/ @return (dict) `start`end`items`incomplete: the span of the name at the cursor, which an item replaces; ([] label;
/ kind; detail; doc; tier), at most 500, those that start with the name up to at; and whether a longer name could be
/ offered more than these, as when nothing is typed yet or more than 500 match
/ @eg .lint.ws.complete[`:a.q;5]
.lint.ws.complete:{[path;at]
    p:hsym path;
    c:.lint.ws.i.point[p;at];
    index:.lint.ws.i.index[];
    nodes:c`nodes;
    typed:last {x iasc y x}[;nodes`start] where ((nodes[`kind]=`name) and (nodes[`start]<at) and at<=nodes`end) or
        (nodes[`end]=at) and nodes[`text]~\:enlist ".";
    start:$[null typed; at; nodes[`start] typed];
    end:$[null typed; at; nodes[`end] typed];
    prefix:(at-start)#start _ c`text;
    items:$[not .lint.ws.i.on_code[c`text;at]; 0#.lint.ws.i.locals[c;at];
        "."=first prefix; .lint.ws.i.members[index;p;prefix];
        .lint.ws.i.locals[c;at],.lint.ws.i.columns[index;p;nodes;at],.lint.ws.i.names[index;p;nodes;typed;prefix]];
    items:`tier`label xasc distinct select from items where label like (prefix,"*");
    incomplete:(500<count items) or (0<count items) and 0=count prefix;
    `start`end`items`incomplete!(start;end;500 sublist items;incomplete)};

/ Whether byte at of text is code: a letter typed there is, as the parser reads the text with it, in no comment and
/ no string, one never closed being the parser's unterminated string.
.lint.ws.i.on_code:{[text;at]
    syn:.lint.i.classic[.pq.syntax;enlist (at#text),"x",at _ text];
    nodes:syn`nodes;
    inside:(nodes[`kind] in `comment`str) and (nodes[`start]<=at) and at<nodes`end;
    unclosed:exec (start<at) and (at<end) and msg like "*unterminated string" from syn`errors;
    not any inside,unclosed};

/ The names the index's project defines and the namespaces above them, and the namespaces q provides it leaves to q
/ (.lint.i.project).
.lint.ws.i.known:{[index] .lint.i.project[index;`symbol$()]`known};
.lint.ws.i.provided:{[index] .lint.i.project[index;`symbol$()]`provided};

/ The index, kept between calls.
.lint.ws.i.index:{[] @[get;`.lint.ws.i.Index;{[e] .lint.i.index[`symbol$();()]}]};

/ The index's text of p, "" when it holds none.
.lint.ws.i.text:{[index;p] first (exec text from index`texts where file=p),enlist ""};

/ The index with paths' rows changed by f (an index to an index), kept; the globals of paths that came or went, or
/ changed how they are defined, and the dotted names they read that came or went: each decides the namespaces the
/ project judges (.lint.i.project), so another document's findings may move.
.lint.ws.i.update:{[paths;f]
    index:.lint.ws.i.index[];
    facts:{[paths;index]
        g:select file, name, how from index[`globals] where file in paths;
        read:distinct select file, name from index[`uses] where file in paths;
        g,update how:`read from read where "."=first each string name}[paths];
    before:facts index;
    `.lint.ws.i.Index set index:f index;
    after:facts index;
    exec distinct name from (before except after),after except before};

/ Of paths, those no editor holds as they are on disk: gone, missing or unreadable; changed, readable with a text the
/ index does not hold, texts each one's.
.lint.ws.i.disk:{[index;paths]
    paths@:where not paths in exec file from index[`texts] where open;
    texts:{@[{"c"$read1 x};x;{[e] ::}]} each paths;
    gone:texts~\:(::);
    held:exec file!text from index`texts;
    changed:where not gone or (paths in key held) and texts~'held paths;
    `gone`changed`texts!(paths where gone;paths changed;texts changed)};

/ Whether a file of paths changed on disk since it was indexed, which reindexes it: the check a query makes of the
/ files its answer points into, should the editor not have said.
.lint.ws.i.fresh:{[index;paths]
    disk:.lint.ws.i.disk[index;distinct paths];
    $[count disk[`gone],disk`changed; [.lint.ws.read disk[`gone],disk`changed; 1b]; 0b]};

/ The definitions (index's defs rows) of the global token c`id refers to (.lint.ws.i.global), p's first, then an open
/ file's, then the rest; read again from disk first when a file they are in changed there.
.lint.ws.i.definitions:{[index;p;c]
    defs:.lint.ws.i.order[index;p] (.lint.ws.i.global[index;c])`defs;
    if[not .lint.ws.i.fresh[index;defs`file]; :defs];
    index:.lint.ws.i.index[];
    .lint.ws.i.order[index;p] (.lint.ws.i.global[index;c])`defs};

/ rows by file: p's, then an open file's, then the rest.
.lint.ws.i.order:{[index;p;rows] rows iasc (2*not rows[`file]=p)+not rows[`file] in exec file from index[`texts] where open};

/ The text of p, read again first if the disk changed it, its nodes (.lint.i.parsed) and locals (.lint.i.locals), and at
/ byte at: id, the name token (else the verb) that starts at or before it and ends at or after it, the later of two, 0N
/ for none; local, whether id is a local of its lambda (.lint.i.is_local).
.lint.ws.i.point:{[p;at]
    .lint.ws.i.fresh[.lint.ws.i.index[];enlist p];
    text:.lint.ws.i.text[.lint.ws.i.index[];p];
    nodes:(.lint.i.parsed text)`nodes;
    loc:.lint.i.locals nodes;
    k:nodes`kind;
    hit:{x iasc y x}[;nodes`start] where (k in `name`verb) and (nodes[`start]<=at) and at<=nodes`end;
    id:last $[any b:k[hit]=`name; hit where b; hit];
    local:$[null id; 0b; first .lint.i.is_local[nodes;loc;enlist id]];
    `text`nodes`loc`id`local!(text;nodes;loc;id;local)};

/ The name tokens of local c`id's lambda that name it, its signature's and its body's, not a nested lambda's, by
/ position; with def, only those that define it, a parameter or an assignment.
.lint.ws.i.local_sites:{[c;def]
    nodes:c`nodes;
    k:nodes`kind;
    scope:nodes`scope;
    l:{[k;p;s] $[`params=k s; p s; s]}[k;nodes`parent] scope c`id;
    own:(scope=l) or (`params=k scope) and l=nodes[`parent] scope;
    sites:where own and (k=`name) and nodes[`text]~\:nodes[`text] c`id;
    sites@:iasc nodes[`start] sites;
    $[def; sites where (`params=k scope sites) or nodes[`target] sites; sites]};

/ The global name token c`id refers to (.lint.i.resolve over the index's globals), and its definitions (index's defs
/ rows).
.lint.ws.i.global:{[index;c]
    target:first .lint.i.resolved[c`nodes;enlist c`id;.lint.ws.i.known index];
    `name`defs!(target;select from index`defs where name=target)};

/ A definition's signature: f[x:`j; y] for a function, name / kind for anything else.
.lint.ws.i.signature:{[r]
    $[r[`kind] in `function`projection; string[r`name],"[",("; " sv .lint.ws.i.params r),"]"; string[r`name]," / ",string r`kind]};

/ A definition's parameters, each with its type check: x:`j, y.
.lint.ws.i.params:{[r] {string[x],$[null y; ""; ":`",string y]}'[r`params;r`types]};

/ Whether word names a built-in: a keyword, a .q function, a control or qSQL word (.lint.i.builtins), a glyph, or a
/ member of a namespace q provides that the index's project leaves to q (.lint.i.project).
.lint.ws.i.builtin:{[index;word]
    w:`$word;
    (w in (.lint.i.builtins[] except `x`y`z),exec name from .Q.ops[]) or (`$"." sv 2#"." vs word) in .lint.ws.i.provided index};

/ A built-in's parameter labels, x y z and on, as many as its highest rank: a root word's from parsing it
/ (.lint.i.most_args), a namespace member's from its value, which is peachq's own.
.lint.ws.i.builtin_params:{[word]
    most:$["."=first word; max 0,@[{.pq.valence get x};`$word;`long$()]; 0^.lint.i.most_args word];
    enlist each most#"xyzuvwabcdefgh"};

/ A built-in's help (.help.get) as a markdown code block, "" when it has none.
.lint.ws.i.help:{[word] h:.help.get word; $[h like "no documentation for *"; ""; "```\n",h,"\n```"]};

/ A built-in's signature: word[x;y].
.lint.ws.i.builtin_sig:{[word] p:.lint.ws.i.builtin_params word; $[count p; word,"[",(";" sv p),"]"; word]};

/ Completion rows of the locals and parameters of the lambda at is in (the innermost; q has no closures).
.lint.ws.i.locals:{[c;at]
    nodes:c`nodes;
    lambdas:where nodes[`kind]=`lambda;
    lambdas@:where (nodes[`start][lambdas]<at) and (at<nodes[`end] lambdas) or not "}"=last each nodes[`text] lambdas;
    l:last lambdas iasc nodes[`start] lambdas;
    loc:c`loc;
    names:distinct exec name from (loc[`params],loc[`any],loc[`implicit]) where scope=l;
    ([] label:string names; kind:`local; detail:count[names]#enlist "local"; doc:count[names]#enlist ""; tier:0)};

/ Completion rows of the columns of the table a qSQL query at is in reads from, when this file defines it as a table
/ literal: the names the literal assigns.
.lint.ws.i.columns:{[index;p;nodes;at]
    none:([] label:(); kind:`symbol$(); detail:(); doc:(); tier:`long$());
    groups:.lint.i.qsql_groups nodes;
    expr:nodes[`parent] first each groups;
    inside:where (nodes[`start][expr]<=at) and at<=nodes[`end] expr;
    if[not count inside; :none];
    g:groups last inside iasc nodes[`start] expr inside;
    source:g 1+first where nodes[`text][g]~\:"from";
    if[null source; :none];
    full:first .lint.i.resolved[nodes;enlist source;.lint.ws.i.known index];
    literal:select from .lint.ws.symbols[p] where name=full, kind=`table;
    if[not count literal; :none];
    t:last literal;
    names:where (nodes[`start] within (t`start;t[`end]-1)) and nodes[`column] and nodes`target;
    names:distinct nodes[`text] names;
    ([] label:names; kind:`column; detail:count[names]#enlist "column of ",string full; doc:count[names]#enlist ""; tier:0)};

/ Completion rows of the names for an undotted prefix: built-in keywords that are names (a glyph is not), and the
/ workspace's definitions (only p's when nothing is typed yet), those of the \d namespace of the name typed (typed)
/ also by their short name.  The keywords' rows are made once: they are the server's own and never change.
.lint.ws.i.names:{[index;p;nodes;typed;prefix]
    defs:index`defs;
    mine:select from defs where (file=p) or 0<count prefix, (string name) like (prefix,"*");
    space:$[null typed; `; `$"." sv -1_"." vs string first .lint.i.full_names[nodes;enlist typed]];
    short:$[null[space] or space=`.; 0#defs; select from defs where ns=space, ((1+count string space)_'string name) like (prefix,"*")];
    rows:.lint.ws.i.def_items[p;mine],update label:(1+count string space)_'label from .lint.ws.i.def_items[p;short];
    keywords:@[get;`.lint.ws.i.Keywords;{[e]
        words:string distinct .lint.i.builtins[] except `x`y`z;
        words@:where (first each words) in .Q.a,.Q.A;
        `.lint.ws.i.Keywords set ([] label:words; kind:`keyword; detail:.lint.ws.i.builtin_sig each words;
            doc:.help.oneline each words; tier:3)}];
    rows,select from keywords where label like (prefix,"*")};

/ Completion rows of definitions, tier 1 where p defines them and 2 elsewhere.
.lint.ws.i.def_items:{[p;defs]
    ([] label:string defs`name; kind:defs`kind; detail:.lint.ws.i.signature each defs; doc:defs`doc; tier:1+not defs[`file]=p)};

/ Completion rows after a dot: the namespaces whose name starts with prefix, and the members of the namespace prefix
/ names up to its last dot, the workspace's definitions and the built-in namespaces' own.
.lint.ws.i.members:{[index;p;prefix]
    ns:`$"." sv -1_"." vs prefix;
    builtin:.lint.ws.i.provided index;
    defined:exec ns from index`defs;
    spaces:distinct defined,builtin;
    spaces@:where (spaces like (prefix,"*")) and not spaces=`.;
    tier:1+(not spaces in exec ns from index[`defs] where file=p)+not spaces in defined;
    own:$[ns in builtin; {x where not null x} @[key;ns;`symbol$()]; `symbol$()];
    own:` sv/: ns,/:own;
    own@:where (string own) like (prefix,"*");
    ([] label:string spaces; kind:`namespace; detail:count[spaces]#enlist "namespace"; doc:count[spaces]#enlist ""; tier:tier),
        .lint.ws.i.def_items[p;select from index`defs where name like (prefix,"*")],
        ([] label:string own; kind:`builtin; detail:.lint.ws.i.builtin_sig each string own; doc:.help.oneline each own; tier:3)};
