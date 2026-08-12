# Typed parameters

> **Status.** This feature lands in stages. Each row below says where it is; every example in
> this document is pinned as a test row, and where this document and the code disagree, this
> document and those ledgers are the specification.
>
> | feature | syntax | status |
> |---|---|---|
> | type checks | ``{[x:`j;y:`f] … }`` | in review |
> | defaults | `{[x;y=10] … }` | designed, not yet shipped |
> | varargs | `{[x;...rest] … }` | designed, not yet shipped |
> | named apply | ``.[f;`x`y!1 2]`` | designed, not yet shipped |

A q function signature has always told you how many arguments a function takes and what they
are called. In peachq it can also tell you their **types**, which of them are **optional**,
what their **defaults** are, and whether the function accepts **extra arguments** — and it
tells you all of that from the text of the signature alone, without running anything.

```q
q)quote:{[sym:`s;size:`j=100;price:`f=0n] (sym;size;price)}
q)quote[`IBM]
`IBM
100
0n
```

That signature is a complete description of how to call `quote`. You do not have to read the
body, and neither does your editor.

## Why peachq supports a subset — and why that is the point

kdb+ 4.1 introduced [pattern matching](https://code.kx.com/q/basics/pattern/), a broad feature
covering list and dictionary destructuring, constant patterns, a pattern conditional, and
**filter functions** — where a parameter annotation is an arbitrary q function that is *called*
on the argument every time the function runs.

peachq implements one part of it deliberately: the **type-check pattern**, plus two extensions
of its own (defaults and varargs) built to the same standard. The standard is:

> **Everything in a signature is a literal or a type name. Nothing in a signature is code.**

That single rule is what makes the whole feature useful to tools:

- **A linter can check your call sites.** Argument counts, argument types, unknown parameter
  names and missing required arguments are all decidable by reading two pieces of text — the
  signature and the call — with no evaluation and no execution risk.
- **An editor can complete and document as you type**, because the parameter list is data, not
  a program whose meaning depends on the state of the process.
- **Errors arrive early.** A malformed or self-contradictory signature is reported when the
  file loads, not on the unlucky code path six months later.
- **A signature is honest to anyone reading it** — a colleague, a reviewer, a code-generation
  tool, or a model. There is no hidden behaviour to discover by running it.

A filter function can express things a type name cannot — "a long between 0 and 100", "a
non-empty table". peachq asks you to write those on the first line of the body instead, where
they read as what they are: validation logic. The signature stays a description.

## Type checks

Annotate a parameter with `:` and a type symbol.

```q
q)f:{[x:`j;y:`j] x+y}
q)f[1;2]
3
q)f[1f;2]
'type
```

Type checking is **exact**. A float is not accepted where a long is declared, and a long is not
accepted where a float is declared — there is no automatic widening. If you want a float
parameter, pass a float:

```q
q)price:{[p:`f] p*2}
q)price 42
'type
q)price 42f
84f
```

### Atoms and lists

Following kdb+, **lowercase means an atom and uppercase means a list**:

```q
q)atom:{[x:`j] x}
q)atom 5
5
q)atom 1 2 3
'type

q)vec:{[x:`J] sum x}
q)vec 1 2 3
6
q)vec 5
'type
```

This is worth understanding before you annotate: **an annotation narrows a function.** An
unannotated q function often works on both atoms and vectors, and declaring `` `j `` gives that
up.

```q
q)plus:{[x;y] x+y}
q)plus[1 2 3;4]
5 6 7

q)tplus:{[x:`j;y:`j] x+y}
q)tplus[1 2 3;4]
'type
```

Iterators are unaffected, because they pass atoms:

```q
q)tplus[;1] each 1 2 3
2 3 4
```

Annotate where you want the narrowing — boundaries, public entry points, anything called from
somewhere you cannot see. Leave general-purpose internal functions unannotated.

### Type symbols

The type character is the standard kdb+ one, lowercase for an atom and uppercase for a list:

| symbol | type | | symbol | type | | symbol | type |
|---|---|---|---|---|---|---|---|
| `` `b `` | boolean | | `` `e `` | real | | `` `d `` | date |
| `` `g `` | guid | | `` `f `` | float | | `` `z `` | datetime |
| `` `x `` | byte | | `` `c `` | char | | `` `n `` | timespan |
| `` `h `` | short | | `` `s `` | symbol | | `` `u `` | minute |
| `` `i `` | int | | `` `p `` | timestamp | | `` `v `` | second |
| `` `j `` | long | | `` `m `` | month | | `` `t `` | time |

There is no character for a table, a dictionary, a general list or a function, so those
parameters go unannotated for now. Longer type names are a planned extension; until they
arrive, annotate what you can name and leave the rest.

## Defaults

A default makes a parameter optional. Write it with `=`, the same way TypeScript does:

```q
q)g:{[x;y=10] x+y}
q)g 1
11
q)g[1;2]
3
```

Defaults combine with types, and the default must satisfy the type it is declared against —
a signature that contradicts itself is rejected when the function is defined, not when it is
called:

```q
q)f2d:{[x:`j;y:`j=10] x+y}
q)f2d 5
15
q)f2d[5;20]
25
```

**Defaults are literals**: atoms, homogeneous vector literals, and `()`. They are not
expressions, and they cannot refer to other parameters.

```q
q)f:{[x;y=1 2 3] y}      / fine
q)f:{[x;y=x*2] y}        / 'parse
q)f:{[x;y=count x] y}    / 'parse
q)f:{[x;y=1+2] y}        / 'parse — a literal is a literal, not a computation
```

This is the rule that lets a tool report a function's defaults without running your code.

**Passing a null is not the same as omitting an argument.** Omission takes the default; an
explicit null is a value you chose:

```q
q)n:{[x=10] x}
q)n[]
10
q)n[0N]
0N
```

Required parameters must come before defaulted ones, which must come before a rest parameter.
`{[x=1;y] … }` is rejected, because `f[5]` would be ambiguous.

## Varargs

A rest parameter, written `...name`, collects any extra arguments into a list. It must be last,
there can only be one, and it takes no default. It may be typed, in which case every collected
argument is checked.

```q
q)fv:{[...x] x}
q)fv[]
()
q)fv[1;2;3]
1 2 3

q)f2v:{[x;y;...z] (x;y;z)}
q)f2v[1;2;3;4]
1
2
3 4
```

A single collected argument is a one-item list, not an atom:

```q
q)f2v[1;2;3]
1
2
,3
```

## Optional arguments and projection

q projects when you supply too few arguments, and that does not change:

```q
q)add:{[x;y] x+y}
q)add[1]
{[x;y]x+y}[1;]
```

Once a function has defaults, an omitted argument has an obvious meaning, so it is used:

```q
q)g 1
11
```

The rule that reconciles the two is short:

> **An explicit hole projects. An absent argument defaults.**

```q
q)p:g[1;]     / a hole: still a projection
q)p 5
6
q)p[]         / applied with nothing: now the default fills
11
```

A function with no defaults and no rest parameter behaves exactly as it always has.

## Named apply

`.[f;dict]` binds dictionary keys to parameter names. Order does not matter, and you can supply
any subset of the arguments as long as the required ones are present.

```q
q)quote:{[sym:`s;size:`j=100;price:`f=0n] (sym;size;price)}

q).[quote;`sym!enlist `IBM]
`IBM
100
0n

q).[quote;`price`sym!(42.5;`IBM)]
`IBM
100
42.5
```

Positional apply is unchanged, and a dictionary you want to pass *as an argument* is enlisted
exactly as before:

```q
q).[quote;(`IBM;500;42.5)]     / positional, as always
`IBM
500
42.5

q)d:`a`b!10 20
q)fd:{[d] d`a+d`b}
q).[fd;enlist d]               / d as one argument
30
```

Unknown and missing names are reported rather than guessed:

```q
q)f:{[x;y=10] x+y}
q).[f;`x`z!1 2]
'param
q).[f;`y!enlist 2]
'param
```

The rest parameter can be bound by name, and its value must be a list — it becomes the rest
list as given:

```q
q)f1v:{[x;y=10;...rest] (x;y;rest)}
q).[f1v;`x`rest!(1;(30;40))]
1
10
30 40
```

## Errors

| you get | when |
|---|---|
| `'parse` | the signature itself is not valid — reported when the function is **defined** |
| `'type` | an argument does not match its declared type |
| `'param` | an unknown or missing parameter name in a named apply |
| `'rank` | too many arguments, and no rest parameter |

The first row is the one that matters for tooling: **every signature mistake is a load-time
error.** A bad type character, a computed default, a required parameter after an optional one,
two rest parameters — none of these can reach production, because the file will not load.

## Not supported, on purpose

| not supported | write this instead |
|---|---|
| filter functions — ``{[x:tempCheck] … }`` | validate on the first line of the body |
| computed defaults — `{[x;y=x*2] … }` | compute in the body when the argument is absent |
| destructuring — `{[(a;b);c] … }` | take the list and index it |
| identifier types — `{[x:long] … }`, `{[c:int|long] … }` | use the type symbol, `` `j `` |

Each of these is rejected because it would put code, or a name that could mean anything, into a
signature — and the signature is the part we promise you can read without running.

The identifier form is worth calling out: `int|long` is a perfectly good q *expression*, so
accepting it in a type position would mean you could no longer tell a type from a variable by
looking. peachq keeps the type position to symbols so that never becomes a question.

## Planned

These are reserved — they are errors today so they can be added later without changing the
meaning of any code you write now:

```q
{[x:`j`J] … }      / unions
{[x:`long] … }     / long type names, with `LONG for the list form
{[x:`number] … }   / built-in type aliases
```

Type aliases will be defined by peachq, not by users — an alias you would have to look up in
someone's source is exactly the thing this feature exists to avoid.

## Using this with kdb+

Type checks are **kdb+ 4.1 syntax**. A function annotated with types and nothing else runs on
kdb+ 4.1 or later unchanged, and can be sent to a kdb+ process over IPC:

```q
q)portable:{[x:`j;y:`f] x*y}
```

Defaults, varargs and named apply are peachq extensions. kdb+ will not parse them, so a
function using them cannot be forwarded to a kdb+ process.

If you maintain code that must run on both, the line is per feature, not per file: annotate
types freely, and keep defaults, varargs and named apply out of the shared parts.
