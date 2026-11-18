/ A language server over .lint for editors.  q -lsp hands each JSON-RPC 2.0 message of the Language Server Protocol
/ here and sends every message this answers.  A document is linted whole on every open and change, and its findings
/ are pushed as publishDiagnostics; outline, go-to-definition, references, hover, signature help and completion are
/ queries of the workspace index (.lint.ws), which open documents and the workspace folders' q files feed.
/ @eg .lsp.on[::;"{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{\"capabilities\":{}}}"]

/ One message in, (session; the messages to send, each a JSON string) out.  initialize agrees the position encoding:
/ the first of the client's positionEncodings that is utf-8 or utf-16, else utf-16.  A request before initialize or
/ after shutdown is an error, an unknown request is MethodNotFound and an unknown notification is ignored.  exit ends
/ the process: 0 after shutdown, else 1.
/ @param session (any) the session the previous call answered
/ @param json (string) one JSON-RPC message
/ @return (list) (session; messages)
/ @eg .lsp.on[::;"{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"shutdown\"}"]
.lsp.on:{[session;json]
    error:{[id;code;text] .lsp.i.answer[id;`error;`code`message!(code;text)]};
    session:$[session~(::); .lsp.i.NEW; session];
    message:@[.j.k;json;{::}];
    if[not 99h=type message; :(session;enlist error[::;-32700;"parse error"])];
    if[not `method in key message; :(.lsp.i.reply[session;message];())];
    method:`$message`method;
    request:`id in key message;
    id:$[request; message`id; ::];
    params:$[`params in key message; message`params; ()!()];
    state:session`state;
    if[method=`exit; exit $[state=`down; 0; 1]];
    if[(state=`new) and not method=`initialize; :(session;$[request; enlist error[id;-32002;"server not initialized"]; ()])];
    if[state=`down; :(session;$[request; enlist error[id;-32600;"server is shut down"]; ()])];
    $[method=`initialize; .lsp.i.initialize[session;id;params];
      method=`shutdown; (@[session;`state;:;`down];enlist .lsp.i.answer[id;`result;::]);
      method in key .lsp.i.NOTES; .lsp.i.NOTES[method][session;params];
      request and method in key .lsp.i.REQUESTS; .lsp.i.request[session;id;.lsp.i.REQUESTS method;params];
      request; (session;enlist error[id;-32601;"method not found: ",string method]);
      (session;())]};

/ The session before initialize: configs maps each lint.config path seen to (what it held; its settings); docs each
/ open document's index key to its uri and version; roots the workspace folders, exclude the like patterns of paths
/ under them left out; queue the files still to index from disk, of total; pending the globals whose definitions
/ came or went outside an edit, for the next relint; progress `none, or `can, `asked, `ready and `on as $/progress is
/ agreed and runs; watch whether the client registers file watchers on request.  exclude adds to .lsp.i.excluded.
.lsp.i.NEW:`state`utf8`configs`docs`roots`exclude`queue`total`pending`progress`watch!
    (`new;0b;(`$())!();(`$())!();();();`$();0;`$();`none;0b);

/ The notifications the server acts on, each a function of (session; params) answering (session; messages).
.lsp.i.NOTES:(`initialized,`$("textDocument/didOpen";"textDocument/didChange";"textDocument/didClose";
    "workspace/didChangeWatchedFiles"))!(
    {[session;params] .lsp.i.initialized session};
    {[session;params] .lsp.i.edit[session;params`textDocument;params[`textDocument]`text]};
    {[session;params] .lsp.i.edit[session;params`textDocument;last[params`contentChanges]`text]};
    {[session;params] .lsp.i.close[session;params`textDocument]};
    {[session;params] .lsp.i.watched[session;params]});

/ The requests the server answers, each a function of (session; params) answering (session; result).
.lsp.i.REQUESTS:((`$"textDocument/",/:("definition";"references";"hover";"signatureHelp";"completion";"documentSymbol")),
    `$"workspace/executeCommand")!(
    {[session;params] (session;.lsp.i.locations[session;.lint.ws.defs . .lsp.i.at[session;params]])};
    {[session;params]
        rows:.lint.ws.refs . .lsp.i.at[session;params];
        (session;.lsp.i.locations[session;$[1b~.lsp.i.dig[params;`context`includeDeclaration]; rows; select from rows where not def]])};
    {[session;params] md:.lint.ws.hover . .lsp.i.at[session;params]; (session;$[count md; enlist[`contents]!enlist .lsp.i.md md; ::])};
    {[session;params] (session;.lsp.i.signature .lint.ws.sig . .lsp.i.at[session;params])};
    {[session;params] (session;.lsp.i.complete[session;params])};
    {[session;params] (session;.lsp.i.symbols[session;.lsp.i.key params[`textDocument]`uri])};
    {[session;params] $[params[`command]~"qlint.reindex"; (.lsp.i.reindex session;::); (session;::)]});

.lsp.i.answer:{[id;k;v] .j.j (`jsonrpc`id,k)!("2.0";id;v)};
.lsp.i.notify:{[method;params] .j.j `jsonrpc`method`params!("2.0";method;params)};
.lsp.i.call:{[id;method;params] .j.j `jsonrpc`id`method`params!("2.0";id;method;params)};
.lsp.i.md:{[text] `kind`value!("markdown";text)};

/ x at the path of keys, (::) where any key is missing.
.lsp.i.dig:{[x;path] {$[99h=type x; $[y in key x; x y; ::]; ::]}/[x;path]};

/ The answer to a request: handler's result, or InternalError naming the q error it signalled.
.lsp.i.request:{[session;id;handler;params]
    r:.[{[h;s;p] (1b;h[s;p])};(handler;session;params);{[e] (0b;e)}];
    $[first r; (r[1;0];enlist .lsp.i.answer[id;`result;r[1;1]]);
        (session;enlist .lsp.i.answer[id;`error;`code`message!(-32603;"internal error: ",last r)])]};

/ The session after a response from the client: the one to window/workDoneProgress/create lets $/progress start.
.lsp.i.reply:{[session;message]
    $[(`asked=session`progress) and "qlint-progress"~message`id;
        @[session;`progress;:;$[`error in key message; `none; `ready]]; session]};

/ The answer to initialize.  A client that omits capabilities.general.positionEncodings gets utf-16, the protocol's
/ default; one that lists them gets the first the server speaks, in the client's order.
.lsp.i.initialize:{[session;id;params]
    offered:.lsp.i.dig[params;`capabilities`general`positionEncodings];
    spoken:{any x~/:("utf-8";"utf-16")};
    encoding:first $[0h=type offered; offered where spoken each offered; ()],enlist "utf-16";
    progress:$[1b~.lsp.i.dig[params;`capabilities`window`workDoneProgress]; `can; `none];
    watch:1b~.lsp.i.dig[params;`capabilities`workspace`didChangeWatchedFiles`dynamicRegistration];
    capabilities:(enlist[`positionEncoding]!enlist encoding),.lsp.i.CAPABILITIES;
    result:`capabilities`serverInfo!(capabilities;`name`version!("qlint";.pq.version));
    session:@[session;`state`utf8`progress`watch;:;(`ready;encoding~"utf-8";progress;watch)],.lsp.i.folders params;
    (session;enlist .lsp.i.answer[id;`result;result])};

/ What the server offers besides the position encoding: full sync and the requests of .lsp.i.REQUESTS.
.lsp.i.CAPABILITIES:(`textDocumentSync`documentSymbolProvider`definitionProvider`referencesProvider`hoverProvider,
    `signatureHelpProvider`completionProvider`executeCommandProvider)!(1;1b;1b;1b;1b;
    enlist[`triggerCharacters]!enlist (enlist "[";enlist ";");enlist[`triggerCharacters]!enlist enlist enlist ".";
    enlist[`commands]!enlist enlist "qlint.reindex");

/ `roots`exclude from initialize's params: the workspace folders, workspaceFolders, else rootUri, else rootPath, each
/ without a trailing / but a filesystem root's (/, C:/); and initializationOptions.exclude, like patterns matched
/ against a path relative to its folder that leave a file out.
.lsp.i.folders:{[params]
    folders:.lsp.i.dig[params;enlist `workspaceFolders];
    root:.lsp.i.dig[params;enlist `rootUri];
    uris:$[98h=type folders; folders`uri; 10h=type root; enlist root; ()];
    roots:$[count uris; .lsp.i.path each uris; 10h=type p:.lsp.i.dig[params;enlist `rootPath]; enlist p; ()];
    exclude:.lsp.i.dig[params;`initializationOptions`exclude];
    exclude:$[(0h=type exclude) and all 10h=type each exclude; exclude; ()];
    unslash:{$[(x~enlist "/") or x like "?:/"; x; x like "?:"; x,"/"; "/"=last x; .z.s -1_x; x]};
    `roots`exclude!(unslash each roots where 0<count each roots;exclude)};

/ On initialized: the workspace folders' q files queued for indexing between messages (.lsp.i.idle), a watcher for
/ every q file asked of a client that registers them, and a progress token asked of one that shows progress.
.lsp.i.initialized:{[session]
    files:raze .lsp.i.walk[session] each session`roots;
    session:@[session;`queue`total;:;(files;count files)];
    watcher:enlist[`watchers]!enlist enlist enlist[`globPattern]!enlist "**/*.q";
    registration:`id`method`registerOptions!("qlint-watch";"workspace/didChangeWatchedFiles";watcher);
    register:$[session`watch; enlist .lsp.i.call["qlint-watch";"client/registerCapability";
        enlist[`registrations]!enlist enlist registration]; ()];
    ask:(`can=session`progress) and 0<count files;
    create:$[ask; enlist .lsp.i.call["qlint-progress";"window/workDoneProgress/create";enlist[`token]!enlist "qlint-index"]; ()];
    (@[session;`progress;:;$[ask; `asked; `none]];register,create)};

/ A piece of the startup indexing, run between messages: queued files are read (.lint.ws.read), eight at a time since
/ each update copies the index, until 50 ms have gone, $/progress reports how far it is, and the open documents the
/ changed globals, these and those pending, may move are linted again.  () when there is nothing to do, so q -lsp
/ waits for the next message; a token the client grants after the queue emptied still gets its begin and end.
.lsp.i.idle:{[session]
    if[(0=count session[`queue],session`pending) and not session[`progress] in `ready`on; :()];
    start:.z.p;
    done:0;
    changed:session`pending;
    session[`pending]:`symbol$();
    while[(done<count session`queue) and (done=0) or 0D00:00:00.05>.z.p-start;
        changed,:.lint.ws.read 8 sublist done _ session`queue;
        done+:8];
    session[`queue]:done _ session`queue;
    progress:.lsp.i.progress session;
    .lsp.i.relint[first progress;`;changed;last progress]};

/ (session; the $/progress to send) as indexing stands: begin once the client gave a token, a report per piece, end
/ when the queue is empty.
.lsp.i.progress:{[session]
    left:count session`queue;
    p:session`progress;
    done:session[`total]-left;
    percentage:"j"$100*done%1|session`total;
    report:$[p=`ready; `kind`title`percentage!("begin";"qlint: indexing q files";percentage);
        (p=`on) and left=0; enlist[`kind]!enlist "end";
        p=`on; `kind`message`percentage!("report";string[done]," of ",string[session`total]," files";percentage);
        ::];
    session[`progress]:$[p=`ready; `on; (p=`on) and left=0; `none; p];
    (session;$[report~(::); (); enlist .lsp.i.notify["$/progress";`token`value!("qlint-index";report)]])};

/ The q files under folder root .lsp.i.workspace keeps.  A filesystem root (/, C:/), the one root ending in /, is
/ never walked: its files are indexed as they are opened or watched.
.lsp.i.walk:{[session;root]
    if[("/"=last root) or not .fs.isdir root; :`symbol$()];
    .lsp.i.workspace[session;.lsp.i.tree hsym `$root]};

/ The files under folder dir but those .lsp.i.excluded names or that are inside such a folder, which is never listed.
/ A folder that cannot be listed holds none.
.lsp.i.tree:{[dir]
    names:@[key;dir;{[e] `symbol$()}];
    (`symbol$()),raze {[dir;name]
        path:` sv dir,name;
        $[.lsp.i.excluded string name; (); 11h=type @[key;path;{[e] ::}]; .lsp.i.tree path; enlist path]}[dir] each names};

/ Per name, whether a file or folder of that name is left out of the workspace, as qStudio leaves it out: the names
/ starting with . or ending -ve, and those listed.
.lsp.i.excluded:{[names]
    listed:`$("target";"node_modules";"__pycache__";"site-packages";"venv";"build";"dist";"__MACOSX");
    (names like ".*") or (names like "*-ve") or (`$names) in listed};

/ Of paths, the q files under a workspace folder that no part of the path below the folder is .lsp.i.excluded and no
/ exclude pattern names, matched against the path relative to the folder.
.lsp.i.workspace:{[session;paths]
    paths:(`symbol$()),paths;
    s:.path.string paths;
    keep:{[roots;exclude;s]
        prefix:{$["/"=last x; x; x,"/"]} each roots;
        under:prefix where {y~count[y]#x}[s] each prefix;
        rel:count[first under]_s;
        (s like "*.q") and (0<count under) and not any (.lsp.i.excluded "/" vs rel),rel like/: exclude}[session`roots;session`exclude] each s;
    paths where keep};

/ A batch of watched-file events: each file .lsp.i.workspace keeps read again from disk, or dropped when it is gone
/ (.lint.ws.read).  A file an editor holds stays as the editor has it.
.lsp.i.watched:{[session;params]
    uris:.lsp.i.dig[;enlist `uri] each (),.lsp.i.dig[params;enlist `changes];
    paths:.lsp.i.key each uris where 10h=type each uris;
    .lsp.i.relint[session;`;.lint.ws.read .lsp.i.workspace[session;paths];()]};

/ Per batch of messages read together, whether a later one in it supersedes its lint: an open or change of a document
/ followed by another open, change or close of the same document with no request between them, which may read it.
/ q -lsp skips those, so a burst of edits is linted once, at its newest version.
/ @param jsons (list) the messages, each a JSON string
.lsp.i.stale:{[jsons]
    doc:{[json]
        m:@[.j.k;json;{::}];
        text:{$[10h=type x; `$x; `]};
        $[99h=type m; (text .lsp.i.dig[m;enlist `method];text .lsp.i.dig[m;`params`textDocument`uri];`id in key m); (`;`;0b)]} each jsons;
    method:doc[;0];
    uri:doc[;1];
    request:doc[;2] and not null method;
    edit:(method in `$"textDocument/",/:("didOpen";"didChange")) and not null uri;
    touch:edit or (method=`$"textDocument/didClose") and not null uri;
    superseded:{[request;touch;uri;i]
        later:(1+i)_til count uri;
        later@:where request[later] or touch[later] and uri[later]=uri i;
        $[count later; not request first later; 0b]}[request;touch;uri];
    edit and superseded each til count jsons};

/ An open or change of doc to text: its index rows replaced (.lint.ws.set), its diagnostics published, and those of the
/ other open documents a change to its definitions may move (.lsp.i.relint).
.lsp.i.edit:{[session;doc;text]
    k:.lsp.i.key doc`uri;
    session:@[session;`docs;,;enlist[k]!enlist (`uri`version inter key doc)#doc];
    changed:.lint.ws.set[k;text];
    published:.lsp.i.publish[session;k];
    .lsp.i.relint[first published;k;changed;last published]};

/ A close of doc: its rows dropped, read again from disk when it is a workspace file, and empty diagnostics published.
.lsp.i.close:{[session;doc]
    k:.lsp.i.key doc`uri;
    session:@[session;`docs;{y _ x};k];
    changed:.lint.ws.drop k;
    changed,:.lint.ws.read .lsp.i.workspace[session;k];
    params:((`uri`version inter key doc)#doc),enlist[`diagnostics]!enlist ();
    .lsp.i.relint[session;k;changed;enlist .lsp.i.notify["textDocument/publishDiagnostics";params]]};

/ (session; messages, then the diagnostics of every open document but k), when changed, the globals whose definitions
/ came or went, is not empty: every rule that resolves a name reads the whole project.
.lsp.i.relint:{[session;k;changed;messages]
    if[not count changed; :(session;messages)];
    others:(key session`docs) except k;
    {[r;o] p:.lsp.i.publish[first r;o]; (first p;last[r],last p)}/[(session;messages);others]};

/ (session; publishDiagnostics) for the open document k.  A file: uri takes the lint.config files above its folder, as
/ q -lint does; any other uri the defaults.  The workspace is the project (.lint.ws.lint).
.lsp.i.publish:{[session;k]
    doc:session[`docs] k;
    path:.lsp.i.path doc`uri;
    configured:$[count path; .lsp.i.settings[session;path]; (session;()!())];
    found:.lint.ws.lint[k;last configured];
    src:.lint.ws.text k;
    ls:0,1+where src="\n";
    range:.lsp.i.range[first configured;src;ls[found[`line]-1]+found[`col]-1;count[src]&ls[found[`endline]-1]+found`endcol];
    diagnostics:([] range; severity:(`error`warning`info!1 2 3) found`severity; code:string found`rule;
        source:count[found]#enlist "qlint"; message:found`msg);
    (first configured;enlist .lsp.i.notify["textDocument/publishDiagnostics";doc,enlist[`diagnostics]!enlist diagnostics])};

/ The index key of a document: its file's path for a file: uri, else the uri itself, as a path symbol.
.lsp.i.key:{[uri] hsym `$$[count p:.lsp.i.path uri; p; uri]};

/ The uri of index key k: the one its editor opened it by, else its file's.
.lsp.i.uri:{[session;k] $[k in key session`docs; session[`docs;k;`uri]; .lint.i.file_uri k]};

/ The path a file: uri names, "" for any other uri.
.lsp.i.path:{[uri]
    if[not uri like "file://*"; :""];
    path:.h.uh 7_uri;
    path:$[path like "localhost/*"; 9_path; path];
    $[path like "/?:*"; 1_path; "/"=first path; path; "//",path]};

/ Per byte of text and one past its end, the characters before it in the agreed encoding: bytes or UTF-16 code units.
.lsp.i.units:{[session;text] "j"$$[session`utf8; til 1+count text; .lint.i.utf16 text]};

/ The positions of bytes b of text: lines from 0, characters in the agreed encoding.
.lsp.i.position:{[session;text;b]
    ls:0,1+where text="\n";
    units:.lsp.i.units[session;text];
    flip `line`character!(ls bin b;units[b]-units ls ls bin b)};

/ The LSP ranges from bytes start to end, end exclusive, of text.
.lsp.i.range:{[session;text;start;end] flip `start`end!.lsp.i.position[session;text] each (start;end)};

/ The byte of text an LSP position names, kept on its line: a position inside a character is its first byte.
.lsp.i.byte:{[session;text;pos]
    ls:0,1+where text="\n";
    line:"j"$pos`line;
    if[line>=count ls; :count text];
    from:ls line;
    to:$[line<-1+count ls; -1+ls line+1; count text];
    units:.lsp.i.units[session;text];
    from|to&units bin units[from]+"j"$pos`character};

/ The index key and byte a request's textDocument and position name.
.lsp.i.at:{[session;params] k:.lsp.i.key params[`textDocument]`uri; (k;.lsp.i.byte[session;.lint.ws.text k;params`position])};

/ LSP Locations of rows ([] file; start; end), in order.
.lsp.i.locations:{[session;rows]
    g:group rows`file;
    ranges:raze {[session;rows;f;i] .lsp.i.range[session;.lint.ws.text f;rows[`start] i;rows[`end] i]}[session;rows]'[key g;value g];
    ([] uri:.lsp.i.uri[session] each rows`file; range:ranges iasc raze value g)};

/ The SignatureHelp of .lint.ws.sig's answer r, null for none.
.lsp.i.signature:{[r]
    if[r~(::); :(::)];
    parameters:([] label:r`params; documentation:r`docs);
    signature:`label`documentation`parameters!(r`label;.lsp.i.md r`doc;parameters);
    `signatures`activeSignature`activeParameter!(enlist signature;0;r`active)};

/ The CompletionList for a request: .lint.ws.complete's items, each replacing the name at the cursor, its tier leading
/ its sortText so an editor keeps the scope order.
.lsp.i.complete:{[session;params]
    k:.lsp.i.key params[`textDocument]`uri;
    text:.lint.ws.text k;
    at:.lsp.i.byte[session;text;params`position];
    c:.lint.ws.complete[k;at];
    items:c`items;
    range:first .lsp.i.range[session;text;enlist c`start;enlist c`end];
    kinds:`function`projection`table`view`value`local`column`keyword`namespace`builtin!3 3 22 10 6 6 5 14 9 3;
    edits:{[range;label] `range`newText!(range;label)}[range] each items`label;
    list:([] label:items`label; kind:kinds items`kind; detail:items`detail; documentation:.lsp.i.md each items`doc;
        textEdit:edits; sortText:string[items`tier],'items`label);
    `isIncomplete`items!(c`incomplete;list)};

/ The DocumentSymbols of the open document k: a symbol per definition (.lint.ws.symbols), those of a \d namespace in
/ a Namespace symbol of their own.  function and projection are Function, table Struct, view Property, value Variable.
.lsp.i.symbols:{[session;k]
    rows:.lint.ws.symbols k;
    text:.lint.ws.text k;
    short:{$[y=`.; string x; (1+count string y)_string x]}'[rows`name;rows`ns];
    kinds:`function`projection`table`view`value!12 12 23 7 13;
    symbols:([] name:short; detail:rows`signature; kind:kinds rows`kind; range:.lsp.i.range[session;text;rows`start;rows`end];
        selectionRange:.lsp.i.range[session;text;rows`at;rows`to]; children:count[rows]#enlist ());
    spaces:distinct (rows`ns) except `.;
    container:{[rows;symbols;ns]
        own:symbols where rows[`ns]=ns;
        range:`start`end!((first own`range)`start;(last own`range)`end);
        `name`detail`kind`range`selectionRange`children!(string ns;"";3;range;first own`selectionRange;own)}[rows;symbols];
    (symbols where rows[`ns]=`.),container each spaces};

/ The session with every workspace file queued again and the files no longer in a workspace folder dropped, the
/ globals they defined pending a relint: the qlint.reindex command, should the index and the disk disagree.
.lsp.i.reindex:{[session]
    files:raze .lsp.i.walk[session] each session`roots;
    dropped:.lint.ws.drop (exec file from .lint.ws.files[] where not open) except files;
    @[session;`queue`total`pending;:;(files;count files;session[`pending],dropped)]};

/ (session; rule!setting) for the file at path, from its lint.config chain as q -lint reads it, each file's settings
/ over those of the files above it.  A config that cannot be read, or is a directory, is skipped with a notice naming
/ it; the rest of the chain still applies.
.lsp.i.settings:{[session;path]
    chain:`$first .lint.i.chain enlist hsym `$path;
    configs:.lsp.i.config/[session`configs;chain];
    (@[session;`configs;:;configs];first .lint.i.merge[chain!last each configs chain;enlist chain])};

/ configs with config's entry, (what the file holds; its settings): its bytes, else `none, `dir or `unreadable.  An
/ entry whose file holds what it held is kept, so a notice about a file, its own or one of its lines, prints once until
/ the file changes.
.lsp.i.config:{[configs;config]
    path:string config;
    held:$[not .fs.exists path; `none; .fs.isdir path; `dir; @[read1;hsym config;{`unreadable}]];
    if[(config in key configs) and held~first configs config; :configs];
    if[held~`dir; -2 "q -lsp: ",path," is a directory; skipped"];
    unreadable:{[path;err]
        -2 "q -lsp: cannot read ",path,"; skipped";
        ()!()}[path];
    settings:$[4h=type held; @[.lint.i.config;path;unreadable]; held~`unreadable; unreadable[::]; ()!()];
    configs,(enlist config)!enlist (held;settings)};
