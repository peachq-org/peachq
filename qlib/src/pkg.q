/ PacKaGe Loading Functions
/ The only public function intended to be called is .pkg.loadq[`:dirlist] 
/ That first loads all .q files in
/ Then runs init* from every namespace available in kdb
system "d .pkg";

/ Question should errors be thrown or logged while loading

lg:{-1 string[.z.t],$[type[x]=98h; "\r\n"; "  "],$[type[x] in 10 -10h; x; .Q.s x]; x};

/ Given a list of directories search full depth and load all .q/.k files in those directories
/ @param dirList (Symbol List) - Paths ot load
loadQFilesIn:{ [dirList]
    getFiles:{raze {{` sv x} each x,/:key x} each x};
    qFiles:{x where x like "*.[kq]"} getFiles dirList;
    {-1 "Loading ",s:string x; @[system;"l ",s;{-1 "Failed to load ",x," Error:",y; 'y}[s;]]} each qFiles;
    qFiles };

loadq:{ [dirList]
    .pkg.loadQFilesIn dirList;
    / find all namespaces
    nsList:`$".",/:string (key `) except `q`Q`h`j`o;
    run each raze findFuncs[;"init*";0b] each nsList; };


/ find functions with a certain name pattern within the selected namespace
/ @param logEmpty (boolean) If set to true write to log that no funcs found otherwise stay silent
findFuncs:{ [ns; pattern; logEmpty]
        fl:{x where x like y}[system "f ",string ns; pattern];
        if[logEmpty or 0<count fl; lg string[ns]," ",pattern," found: `","`" sv string fl];
        $[ns~`.; fl; `${"." sv x} each string ns,/:fl]};


/ attempt to run 0-arg function or throw an error
run:{ [func]
    lg "Running func:",string func;
    @[value func;::;{'lg "setUpError",x}]};

