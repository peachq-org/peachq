/ help.q — the .help doc store: every doc comment the engine saw, queryable.
/ PURE q by owner ruling — no C implements .help. The script seam hands
/ .help.add only DATA (fullname;ns;file;line;header).  Re-pointing the hook is
/ NOT a supported contract: override it and you are on your own.
/ Schema after qstudio's man.q; args/filetags are DERIVED from @param/@author
/ by a later PR. Loaded FIRST in the bundle (Makefile LIB_Q_SRCS) — the one
/ load order that is not moot, since capture needs .help.add already bound.
.help.funcs:([fullname:`$()] ns:`$(); file:`$(); line:`long$(); header:())
.help.args:([] fullname:`$(); tag:`$(); param:`$(); description:())
.help.files:([file:`$()] ns:`$(); header:())
.help.filetags:([] file:`$(); tag:`$(); val:())

/ THE hook, and the only public entry. An empty fullname documents the FILE.
.help.add:{[x]
  $[null x 0;
    [delete from `.help.files where file=x 2; `.help.files insert enlist each (x 2;x 1;x 4)];
    [delete from `.help.funcs where fullname=x 0; `.help.funcs insert enlist each x]]; }
