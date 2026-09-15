/ Conway's Game of Life on the terminal, the worked example for the .termbox cell-terminal API.
/ Run it:  q examples/q/life.q     ('r' reseeds, space pauses, 'q' or Ctrl-C quits).
/ The loop runs as its own statement, so .termbox restores the terminal however that statement ends -
/ a clean 'q', an error, or a Ctrl-C 'stop (which q cannot trap) - all restore at the statement seam.

\l pq

/ one generation: a cell lives iff it has three live neighbours, or two and is itself alive.
life:{[b] 3=n-b*4=n:2 sum/ 1 0 -1 rotate\:/: 1 0 -1 rotate/:\: b};

seed:{[h;w] w cut (h*w)?01b};

/ a colour per cell, kept while the cell lives; the 16-colour set because Terminal.app has no truecolor by default.
tint:{[h;w] w cut (h*w)?2+til 15};

/ paint the board plus a one-line status bar, then flush.
render:{[b;t;paused]
    .termbox.clear[];
    .termbox.show[{" #"x} each b; t; 0];
    .termbox.print[0; count b; .termbox.color.cyan; 0; "[q]uit [r]andom [space]",$[paused;"run  ";"pause"]];
    .termbox.present[]};

run:{[]
    h:.termbox.height[]-1; w:.termbox.width[];
    b:seed[h;w]; t:tint[h;w];
    paused:0b;
    live:1b;
    while[live;
        render[b; t; paused];
        e:.termbox.peek_event 100;
        k:e`name;
        if[k in `q`ctrl_c; live:0b];
        if[k=`resize; h:.termbox.height[]-1; w:.termbox.width[]];
        if[k in `r`resize; b:seed[h;w]; t:tint[h;w]];
        if[k=`space; paused:not paused];
        if[not paused; b:life b]]};

.termbox.init[];
run[];
.termbox.shutdown[];
exit 0
