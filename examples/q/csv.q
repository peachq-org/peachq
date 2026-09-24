/ Load a CSV with 0:, ask it questions, and write an answer back out as CSV.
/ Run it:  \l csv.q   (it reads prices.csv from the directory it is run in)

/ one type letter per column (D date, S symbol, F float, J long); enlist "," = the first row is the header
prices:("DSFJ";enlist ",") 0: `:prices.csv
meta prices

/ each symbol's close on the last day, and its move over the period
select last close, move:(last close)%first close by sym from prices

/ the day each symbol traded most
select from prices where volume=(max;volume) fby sym

/ a daily return per symbol, saved as CSV
returns:update ret:-1+close%prev close by sym from prices
`:returns.csv 0: csv 0: select date, sym, ret from returns where not null ret
count read0 `:returns.csv
