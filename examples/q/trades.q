/ A day of synthetic trades, then the questions q is built for: select, group, aggregate.
/ Run it:  \l trades.q   (in the browser REPL, or q examples/q/trades.q)

\S 42
n:10000
syms:`AAPL`MSFT`IBM`GOOG`AMZN
trades:([] time:09:30:00.000+asc n?23400000; sym:n?syms; price:100+0.01*n?5000; size:100*1+n?50)

/ the first few rows
5#trades

/ volume, trade count and a volume-weighted average price per symbol
select trades:count i, volume:sum size, vwap:size wavg price by sym from trades

/ open, high, low and close per symbol per hour
select open:first price, high:max price, low:min price, close:last price by sym, hour:time.hh from trades where sym in `AAPL`IBM

/ the five largest trades
5#`size xdesc trades
