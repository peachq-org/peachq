/ help.q — the .help doc store: every doc comment the engine saw, queryable.
/ PURE q by owner ruling — no C implements .help; the script seam only calls
/ the two hooks below.  Re-pointing them is NOT a supported contract.
/ Schema after qstudio's man.q.  Headers are parsed ON INGEST — only the
/ structured rows are stored, so a changed parse rule means "reload the file".
/ Loaded FIRST in the bundle (Makefile LIB_Q_SRCS) — the one load order that
/ is not moot, since capture needs the hooks already bound.
.help.funcs:([fullname:`$()] ns:`$(); file:`$(); line:`long$())
.help.args:([] fullname:`$(); tag:`$(); param:`$(); description:())
.help.files:([file:`$()] ns:`$())
.help.filetags:([] file:`$(); tag:`$(); val:())

.help.i.str:{$[10h=abs type x;x;string x]}

/ NOT built-in trim/rtrim, deliberately: peachq's trim strips tabs where kx's
/ removes spaces only (an unpinned divergence inherited from the rayfall
/ kernel), and a fix there would silently stop tab-indented @tags parsing.
.help.i.rstrip:{$[count i:where not x in " \t\n\r";(1+last i)#x;""]}
.help.i.strip:{.help.i.rstrip $[count i:where not x in " \t\n\r";(first i)_x;""]}

.help.i.join:{[lines] $[count i:where 0<count each lines;"\n" sv lines (first i)+til 1+(last i)-first i;""]}

/ header -> rows (tag;param;description;val), the lead description first under
/ a null tag: the lead is EVERY line before the first tag, a non-tag line
/ after a tag continues THAT tag, and a bare @ is text, never a tag — so it
/ cannot collide with the lead row's null tag.  A NAME is split off for @param
/ and @exception only; every other tag, known or invented, is kept under its
/ own name with its text unread — no whitelist, so no tag is ever dropped.
.help.i.parse:{[header]
  text:{[l] l:.help.i.strip l; l:$[count i:where not l="/";(first i)_l;""]; $[(count l)and (first l)in " \t";1_l;l]};
  istag:{[l] l:.help.i.strip l;(1<count l)and"@"=first l};
  tag:{[blk]
    word:{[s] i:$[count j:where s in " \t";first j;count s];(i#s;.help.i.strip i _ s)};
    w:word 1_.help.i.strip first blk;
    v:.help.i.join (enlist w 1),1_blk;
    $[(`$w 0)in`param`exception;
      [p:word w 1;(`$w 0;`$p 0;.help.i.join (enlist p 1),1_blk;v)];
      (`$w 0;`;v;v)]};
  s:text each "\n" vs header;
  g:sums istag each s;
  d:.help.i.join s where g=0;
  n:$[count g;`long$last g;0];
  ($[count d;enlist(`;`;d;d);()]),tag each {[s;g;i] s where g=i}[s;g] each 1+til n}

.help.i.args:{[fullname;header] r:.help.i.parse header;$[count r;((count r)#fullname;r[;0];r[;1];r[;2]);(`$();`$();`$();())]}
.help.i.filetags:{[file;header] r:.help.i.parse header;$[count r;((count r)#file;r[;0];r[;3]);(`$();`$();())]}

/ begin this file — replace everything previously known about it.  C calls
/ this at every script begin with an empty header, and AGAIN with the real one
/ if the leading run resolves to a file header; the q_comment.h invariant (the
/ leading run resolves before any definition registers) makes the second
/ clear a no-op wipe of nothing.  An empty header records no file row.
/ .help.args rows carry no file, so the fullname set MUST be read out before
/ the funcs rows are deleted.
.help.register_file:{[file;ns;header]
  fl:file;
  fns:exec fullname from .help.funcs where file=fl;
  delete from `.help.args where fullname in fns;
  delete from `.help.funcs where file=fl;
  delete from `.help.filetags where file=fl;
  delete from `.help.files where file=fl;
  if[count header;
    `.help.files upsert (file;ns);
    `.help.filetags insert .help.i.filetags[file;header]]; }

/ one documented definition; re-registering replaces its own rows only.  The
/ derived args are unkeyed with many rows per name, so delete-then-insert is
/ the idiom there, not a workaround.
.help.register_definition:{[fullname;ns;file;line;header]
  nm:fullname;
  `.help.funcs upsert (fullname;ns;file;line);
  delete from `.help.args where fullname=nm;
  `.help.args insert .help.i.args[fullname;header]; }

/ print one name's documentation — its description, then its tags.
/ @param name (symbol|string) the fullname a definition was captured under
.help.get:{[name]
  render:{[n;r]
    tagline:{[t;p;d]
      h:"  @",string[t],$[null p;"";" ",string p];
      ls:"\n" vs d;
      (enlist $[count first ls;h,"  ",first ls;h]),"    ",/:1_ls};
    d:$[count i:where null r`tag;r[first i;`description];""];
    t:select from r where not null tag;
    (enlist string n),($[count d;"  ",/:"\n" vs d;()]),($[count t;enlist"";()]),
     raze tagline'[t`tag;t`param;t`description]};
  n:`$.help.i.str name;
  r:select tag,param,description from .help.args where fullname=n;
  $[count r;-1 "\n" sv render[n;r];-1 "no documentation for ",string n];}

/ the doc rows whose tag, param or description match — a glob, matched
/ anywhere and case-insensitively, so a bare word is a substring search.
/ @param pattern (string|symbol) the pattern
/ @return (table) the matching rows of .help.args
.help.find:{[pattern]
  p:"*",lower[.help.i.str pattern],"*";
  p:p where not (p="*")and"*"=next p;   / a user's own leading * would make ** — 'nyi here
  t:.help.args;
  $[count t;t where((lower each string t`tag)like p)or((lower each string t`param)like p)or(lower each t`description)like p;t]}
