/ Functions and the iterators (adverbs) that apply them: each, over, scan, prior, each-left and each-right.
/ Run it:  \l adverbs.q

/ a lambda; x, y and z are the implicit arguments
hyp:{sqrt (x*x)+y*y}
hyp[3;4]

/ each applies a function item by item; ' pairs up two lists
count each ("one";"three";"seven")
3 5 8 hyp' 4 12 15

/ over (/) folds a list to one value, scan (\) keeps every step
(+/) 1 2 3 4 5
(+\) 1 2 3 4 5
fib:{x,sum -2#x}/[10;0 1]
fib

/ prior (':) sees each item with the one before it
(-':) 3 7 12 20 31

/ each-left (\:) and each-right (/:) make tables of results: a times table
1 2 3 */: 1 2 3 4

/ a projection fixes some arguments; scan with no count converges: it repeats until nothing changes
add3:+[3]
add3 10 20
{x div 2}\[100]
