/ Draw on the terminal as a grid of cells: .termbox.init[] takes the screen, .termbox.show rows paints a list of
/ strings from the top-left corner, .termbox.present[] flushes the changes, .termbox.peek_event 100 waits up to
/ 100 ms for a key or mouse event, .termbox.shutdown[] hands the terminal back.  The names and meanings follow
/ termbox2 (snake_case), so its documentation and examples carry over.  examples/q/life.q is the worked example.
/ @implNote Natives .termbox.i.* (one session at a time; xterm-only, so one behaviour on Linux, mac and Win 11).
/ The terminal is restored on shutdown, on any error or Ctrl-C at the statement seam, and on exit - a q program
/ cannot trap 'stop, so restore is never the program's job.  Ctrl-C is the interrupt, never a key.
/ Output written by show/-1 inside a loop reaches the screen only after the statement ends (the console buffer
/ drains between statements); draw through .termbox instead.
/ ANY-ORDER LAW: definitions only.

/ take the terminal: raw input, the alternate screen, cursor hidden.  Nested inside the REPL's own terminal
/ state, which comes back at shutdown.
/ @throws os stdin/stdout is not a terminal
/ @throws domain a session is already open
.termbox.init:{[] .termbox.i.init[]};

/ hand the terminal back exactly as init found it.  Safe to call when no session is open.
.termbox.shutdown:{[] .termbox.i.shutdown[]};

/ the screen width in cells, as of the last present.
/ @return (long)
.termbox.width:{[] first .termbox.i.size[]};

/ the screen height in cells, as of the last present.
/ @return (long)
.termbox.height:{[] last .termbox.i.size[]};

/ blank every cell of the back buffer (space, default colours).  Nothing shows until present.
.termbox.clear:{[] .termbox.i.clear[]};

/ write the back buffer to the terminal: only the cells that changed since the last present are sent.
/ A resized window is noticed here; the next peek_event answers a `resize event.
.termbox.present:{[] .termbox.i.present[]};

/ forget what the terminal shows, so the next present repaints every cell.
.termbox.invalidate:{[] .termbox.i.invalidate[]};

/ place the cursor (0-based; shown at the next present).  A negative coordinate hides it.
/ @param x (long) column
/ @param y (long) row
.termbox.set_cursor:{[x;y] .termbox.i.set_cursor[x;y]};

/ hide the cursor (the state init starts in).
.termbox.hide_cursor:{[] .termbox.i.set_cursor[-1;-1]};

/ write one cell of the back buffer.  A position off the screen is ignored.
/ @param x (long) column, 0-based
/ @param y (long) row, 0-based
/ @param ch (char|long) a char, or a Unicode codepoint as a long
/ @param fg (long) a style: .termbox.color.<name>, .termbox.rgb[r;g;b], plus any .termbox.attribute.<name>
/ @param bg (long) the background style
.termbox.set_cell:{[x;y;ch;fg;bg] .termbox.i.set_cell[x;y;ch;fg;bg]};

/ write many cells at once - the sparse overlay: x and y are long vectors of one length (cell i at x[i];y[i]),
/ or one of them an atom that extends to the other's length (both atoms: one cell); ch a char or a codepoint
/ long for every cell, a string with one BYTE per cell, a long vector of codepoints, or a list of strings each
/ decoding from UTF-8 to ONE codepoint ('type otherwise); fg and bg a style for every cell or a long vector
/ with one per cell.  Cells off the screen are dropped.
/ @param x (long|long vector) columns, 0-based
/ @param y (long|long vector) rows, 0-based
/ @param ch (char|string|long|long vector|list)
/ @param fg (long|long vector)
/ @param bg (long|long vector)
/ @throws length x, y, ch and a vector style do not agree
.termbox.set_cells:{[x;y;ch;fg;bg] .termbox.i.set_cells[x;y;ch;fg;bg]};

/ write a string into the back buffer from (x;y), one cell per character (UTF-8 is decoded, so a
/ multibyte character is ONE cell).  Text past the right edge is dropped.  .termbox.print[x;y;fg;bg;text] or
/ .termbox.print[x;y;fg;bg;text;w;align]: the second form places the text inside a w-cell field starting at
/ x, aligned `left, `centre (`center) or `right by CELLS, and clipped to the field.
/ Bound straight to the vary native (both arities); guarded so the file still loads before the \l pq gate.
/ @param x (long) column, 0-based
/ @param y (long) row, 0-based
/ @param fg (long) the foreground style, or a long vector with one style per character
/ @param bg (long) the background style, or a long vector with one style per character
/ @param text (string)
.termbox.print:$[`print in key`.termbox.i; .termbox.i.print; ::];

/ every pending event as a table with the event dict's columns (kind name ch mod x y button w h), one row per
/ event in arrival order: waits at most ms for the first, then takes what is already queued without waiting.
/ An empty table with that schema when nothing arrived.
/ @param ms (long) the timeout for the first event; negative waits forever
/ @return (table)
.termbox.events:{[ms] .termbox.i.events ms};

/ milliseconds since init on a monotonic clock (never wall time), 0 before init so a game can call it anywhere.
/ @return (long)
.termbox.clock:{[] .termbox.i.clock[]};

/ word-wrap text to w CELLS (not bytes: a multibyte char is one cell): breaks at spaces, a newline in the text
/ starts a new line, a word wider than w is split where it must be.  Pure - no session needed.
/ @param w (long) the line width in cells
/ @param text (string)
/ @return (list) strings
.termbox.wrap:{[w;text] .termbox.i.wrap[w;text]};

/ paint whole rows from the top-left corner - the vectorised set_cell.  .termbox.show[rows] or
/ .termbox.show[rows;fg;bg]: rows is a list of strings (row y from the string at y), fg/bg a style atom for
/ everything, a long vector with one style per row, or a list of long vectors with one style per cell.
/ Rows below the screen and cells past the right edge are dropped.
/ Bound straight to the vary native (both arities); guarded so the file still loads before the \l pq gate.
.termbox.show:$[`show in key`.termbox.i; .termbox.i.show; ::];

/ wait up to ms milliseconds for an event: always a dict `kind`name`ch`mod`x`y`button`w`h (kind and name, not
/ type and key: qSQL cannot name a keyword column).  kind is `key, `mouse, `resize, or `none when nothing
/ arrived in time; name is the key's name (`a, `enter, `space, `left, `f1, `ctrl_c, `esc ...) and, for any other
/ event, its kind again (`mouse, `resize, `none) so one switch on name dispatches everything - except a pointer
/ movement, kind `mouse with name `move; ch is the char for a printable key (a long codepoint when it is not one
/ byte); mod sums 1 shift, 2 alt, 4 ctrl; x y and button (`left`right`middle`wheel_up`wheel_down`release,
/ `none for a plain movement) belong to mouse events, w h to a resize; a field that does not apply is null.
/ Mouse events arrive only after set_input_mode with .termbox.input.mouse.  No key-release events.
/ @param ms (long) the timeout; negative waits forever
/ @return (dict)
.termbox.peek_event:{[ms] .termbox.i.peek_event ms};

/ wait for the next event, with no timeout.  peek_event's answer.
/ @return (dict)
.termbox.poll_event:{[] .termbox.i.peek_event -1};

/ choose how input is read: a sum of .termbox.input.<name> values.  .termbox.input.mouse turns mouse
/ reporting on - clicks, wheel, releases and every pointer movement (name `move, button `none, or the held
/ button while dragging); esc and alt both accept Esc-prefixed keys as alt-modified (that is what xterm sends).
/ @param mode (long)
/ @return (long) the previous mode
.termbox.set_input_mode:{[mode] .termbox.i.set_input_mode mode};

/ choose how colours are sent: a .termbox.output.<name> value.  init picks the richest the terminal reports
/ (COLORTERM, TERM); NO_COLOR or PEACHQ_COLORS=0 picks mono.  A style richer than the mode degrades to the
/ nearest colour the mode has.
/ @param mode (long)
/ @return (long) the previous mode
.termbox.set_output_mode:{[mode] .termbox.i.set_output_mode mode};

/ whether the terminal reported 24-bit colour at init.
/ @return (boolean)
.termbox.has_truecolor:{[] .termbox.i.has_truecolor[]};

/ the back buffer as a table with one row per cell: x y ch fg bg, ch a long codepoint ("c"$ a row's ch for
/ ASCII text).  What present would send, before it is sent.
/ @return (table)
.termbox.cells:{[] .termbox.i.cells[]};

/ start a tone of hz for ms milliseconds and return at once (it plays on its own thread, one at a time): 1b when
/ a tone was started, 0b when one is still playing (the call is dropped) or no audio device was available and the
/ terminal bell rang instead.
/ @param hz (long) 0-32767; 0 is silence
/ @param ms (long) 0-60000
/ @return (boolean)
.termbox.beep:{[hz;ms] .termbox.i.beep[hz;ms]};

/ a 24-bit colour as a style long.  Styles are disjoint bit fields, so add colours and attributes:
/ .termbox.rgb[255;128;0]+.termbox.attribute.bold
/ @param r (long) 0-255
/ @param g (long) 0-255
/ @param b (long) 0-255
/ @return (long)
.termbox.rgb:{[r;g;b] 512+1024*b+256*g+256*r};   / right to left: b+256*(g+256*r), shifted 10, rgb flag at bit 9

/ the named colours as style longs - .termbox.color.red - plus default (the terminal's own) and peach.
/ Add an attribute to a colour: .termbox.color.red+.termbox.attribute.bold.  .termbox.color is the whole set as a dict.
.termbox.color.default:0;
.termbox.color.black:1;
.termbox.color.red:2;
.termbox.color.green:3;
.termbox.color.yellow:4;
.termbox.color.blue:5;
.termbox.color.magenta:6;
.termbox.color.cyan:7;
.termbox.color.white:8;
.termbox.color.bright_black:9;
.termbox.color.bright_red:10;
.termbox.color.bright_green:11;
.termbox.color.bright_yellow:12;
.termbox.color.bright_blue:13;
.termbox.color.bright_magenta:14;
.termbox.color.bright_cyan:15;
.termbox.color.bright_white:16;
.termbox.color.peach:.termbox.rgb[255;160;122];

/ text attributes as style longs; add them to a colour.
.termbox.attribute.bold:1099511627776;
.termbox.attribute.underline:2199023255552;
.termbox.attribute.reverse:4398046511104;
.termbox.attribute.italic:8796093022208;
.termbox.attribute.blink:17592186044416;
.termbox.attribute.dim:35184372088832;

/ input modes for set_input_mode; add mouse to esc or alt to receive mouse events.
.termbox.input.esc:1;
.termbox.input.alt:2;
.termbox.input.mouse:4;

/ output modes for set_output_mode.
.termbox.output.mono:0;
.termbox.output.normal:1;
.termbox.output.color256:2;
.termbox.output.truecolor:3;
