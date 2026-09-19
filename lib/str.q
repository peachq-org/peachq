/ Python's string helpers with Python's spellings and semantics: strip lstrip rstrip, startswith endswith,
/ removeprefix removesuffix, isalpha isdigit isalnum isspace isupper islower, printf and format.
/ Every function takes one string or a list of strings; a symbol reads as its string.  The is* predicates answer
/ 0b for "" as Python does.  lower, upper, vs, sv, ss and ssr are q's own and have no twin here.
/ .
/ @eg .str.strip "  a b  "
/ @eg .str.isdigit ("123";"12a";`45)
/ @eg .str.format ("{} costs {:.2f}";"tea";1.5)

/ @ignore
.str.i.text:{[s] $[type[s] in -11 11h; string s; -10h=type s; enlist s; type[s] in 0 10h; s; '`type]};

/ @ignore
.str.i.all_in:{[s;alphabet] $[0=count s; 0b; all s in alphabet]};

/ @ignore
.str.i.cased_in:{[s;alphabet] .str.i.all_in[s where s in .Q.a,.Q.A; alphabet]};

/ Leading and trailing whitespace (" \t\n\r") removed; q's trim strips only spaces.
.str.strip:{[text]
    text:.str.i.text text;
    $[0h=type text; .z.s each text; .str.i.strip text]};

/ Leading whitespace removed.
.str.lstrip:{[text]
    text:.str.i.text text;
    $[0h=type text; .z.s each text; .str.i.lstrip text]};

/ Trailing whitespace removed.
.str.rstrip:{[text]
    text:.str.i.text text;
    $[0h=type text; .z.s each text; .str.i.rstrip text]};

/ Does text begin with prefix?  An empty prefix always does.
/ @eg .str.startswith[("apple";"banana");"a"]
.str.startswith:{[text;prefix]
    text:.str.i.text text;
    prefix:.str.i.text prefix;
    $[0h=type text; .z.s[;prefix] each text; prefix~(count prefix) sublist text]};

/ Does text end with suffix?
.str.endswith:{[text;suffix]
    text:.str.i.text text;
    suffix:.str.i.text suffix;
    $[0h=type text; .z.s[;suffix] each text; suffix~(neg count suffix) sublist text]};

/ text without one leading prefix, unchanged when it does not start with it.
/ @eg .str.removeprefix["v1.2";"v"]
.str.removeprefix:{[text;prefix]
    text:.str.i.text text;
    prefix:.str.i.text prefix;
    $[0h=type text; .z.s[;prefix] each text; .str.startswith[text;prefix]; (count prefix)_ text; text]};

/ text without one trailing suffix, unchanged when it does not end with it.
.str.removesuffix:{[text;suffix]
    text:.str.i.text text;
    suffix:.str.i.text suffix;
    $[0h=type text; .z.s[;suffix] each text; .str.endswith[text;suffix]; (neg count suffix)_ text; text]};

/ @ignore
.str.i.msg:{[msg;engine;unescape]
    $[10h=type msg; unescape msg;
      (0h=type msg) and 10h=type first msg; engine msg;
      '`type]};

/ C-style formatting (DuckDB's printf grammar): %d %s %.2f %'d groups thousands, %,d spells the comma; %r renders
/ any q value; a plain string only has %% unescaped.
/ @param msg a format string, or a list of one plus its arguments
/ @eg .str.printf ("%s: %'d rows in %.1fs";"trade";1234567;0.25)
.str.printf:{[msg] .str.i.msg[msg; .str.i.printf; ssr[;"%%";"%"]]};

/ Python-style formatting (DuckDB's format grammar): {} {0} {:>8.2f} {:,}; a plain string only has {{ }} unescaped.
/ @param msg a format string, or a list of one plus its arguments
/ @eg .str.format ("{:>6}|{:,}";"ab";1234567)
.str.format:{[msg] .str.i.msg[msg; .str.i.format; {ssr[ssr[x;"{{";"{"];"}}";"}"]}]};

/ Is every character a letter, and is there at least one?
.str.isalpha:{[text]
    text:.str.i.text text;
    $[0h=type text; .z.s each text; .str.i.all_in[text;.Q.a,.Q.A]]};

/ Is every character a digit, and is there at least one?
.str.isdigit:{[text]
    text:.str.i.text text;
    $[0h=type text; .z.s each text; .str.i.all_in[text;.Q.n]]};

/ Is every character a letter or a digit, and is there at least one?  "_" is neither.
.str.isalnum:{[text]
    text:.str.i.text text;
    $[0h=type text; .z.s each text; .str.i.all_in[text;.Q.a,.Q.A,.Q.n]]};

/ Is every character whitespace, and is there at least one?
.str.isspace:{[text]
    text:.str.i.text text;
    $[0h=type text; .z.s each text; .str.i.all_in[text;" \t\n\r"]]};

/ Is every cased character upper case, and is there at least one?  "ABC1" is 1b.
.str.isupper:{[text]
    text:.str.i.text text;
    $[0h=type text; .z.s each text; .str.i.cased_in[text;.Q.A]]};

/ Is every cased character lower case, and is there at least one?  "abc1" is 1b.
.str.islower:{[text]
    text:.str.i.text text;
    $[0h=type text; .z.s each text; .str.i.cased_in[text;.Q.a]]};
