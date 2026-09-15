/ Snake on the terminal, the second worked example for the .termbox cell-terminal API.
/ Run it:  q examples/q/snake.q     (arrows or wasd/hjkl steer, 'r' restarts after a crash, 'q' or Ctrl-C quits).
/ The loop runs as its own statement, so .termbox restores the terminal however that statement ends -
/ a clean 'q', an error, or a Ctrl-C 'stop (which q cannot trap) - all restore at the statement seam.

\l pq

dirs:(`up`w`k,`down`s`j,`left`a`h,`right`d`l)!(0 -1;0 1;-1 0;1 0) where 4#3;

/ n random empty cells inside the border
food:{[w;h;taken;n] neg[n]?(raze (1+til w-2),/:\:1+til h-2) except taken};

/ a fresh round on the current console size: one status row below the board, one apple per 150 cells.
/ 16-colour only (Terminal.app).
newround:{[]
    w:.termbox.width[]; h:.termbox.height[]-1;
    snake:enlist (w div 2;h div 2);
    n:max 1,((w-2)*h-2) div 150;
    `w`h`snake`dir`apples`score`over`quit!(w;h;snake;1 0;food[w;h;snake;n];0;0b;0b)};

status:{[st]
    sc:"score ",string st`score;
    rest:$[st`over; "  GAME OVER  [r]estart [q]uit"; "  \302\267 apples ",string[count st`apples],"  \302\267 arrows/wasd  \302\267 q quits"];
    (sc,rest; (count[sc]#.termbox.color.cyan+.termbox.attribute.bold),count[rest]#.termbox.color.cyan)};

render:{[st]
    edge:"+",(((st`w)-2)#"-"),"+";
    mid:"|",(((st`w)-2)#" "),"|";
    .termbox.clear[];
    .termbox.show[enlist[edge],(((st`h)-2)#enlist mid),enlist edge; .termbox.color.cyan; 0];
    s:st`snake; a:st`apples;
    .termbox.set_cells[s[;0];s[;1];"@",(count[s]-1)#"o";.termbox.color.bright_green,(count[s]-1)#.termbox.color.green;0];
    .termbox.set_cells[a[;0];a[;1];"*";.termbox.color.red;0];
    line:status st;
    .termbox.print[0;st`h;line 1;0;line 0];
    .termbox.present[]};

/ one step for one key (a held key repeats, so the snake speeds up): advance unless the round is over
step:{[st;k]
    if[k=`resize; :newround[]];
    if[k in `q`ctrl_c; st[`quit]:1b; :st];
    if[st`over; :$[k=`r; newround[]; st]];
    if[k in key dirs; d:dirs k; if[not d~neg st`dir; st[`dir]:d]];
    nh:(first st`snake)+st`dir;
    if[(nh[0] in 0,(st`w)-1) or (nh[1] in 0,(st`h)-1) or nh in st`snake; st[`over]:1b; .termbox.beep[220;250]; :st];
    ate:nh in st`apples;
    st[`snake]:enlist[nh],$[ate; st`snake; -1_st`snake];
    if[ate; .termbox.beep[880;40]; st[`score]:1+st`score; st[`apples]:(st[`apples] except enlist nh),food[st`w;st`h;st[`snake],st`apples;1]];
    st};

run:{[] st:newround[]; while[not st`quit; render st; st:step/[st;{$[count x; x`name; enlist `]} .termbox.events 120]]};

.termbox.init[];
run[];
.termbox.shutdown[];
exit 0
