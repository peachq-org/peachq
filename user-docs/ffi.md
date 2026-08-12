# Foreign functions (FFI)

peachq calls C through the `.ffi` namespace: the [KX ffikdb](https://github.com/KxSystems/ffi) surface, spelling for
spelling, over a vendored [libffi](https://sourceware.org/libffi/). The spellings are verbatim — `.ffi.bind`,
`.ffi.callFunction`, `.ffi.cvar`, `.ffi.setErrno`, `.ffi.extension`, `.ffi.ptrsize`, `.ffi.os` — so published ffikdb
examples run unchanged.

**Read the next section before anything else.** Nearly every "my FFI call doesn't work" is one of two things: the calling
convention, or a q list that collapsed. Both are below.

## Calling a bound function

**A bound function takes ONE list of arguments.** It is unary. You do not call it with the C function's arguments; you
call it with a *list* of them, and you end that list with `::`.

```q
q)\l pq
q)f:.ffi.bind[`libm.so.6`pow;"ff";"f"]
q)f(2f;10f;::)
1024f
```

The three spellings that look more natural all fail, and they all fail the same way:

| you write | what the binding receives | result |
|---|---|---|
| `f[2f;10f]` | two arguments | `'rank` |
| `f(2f;10f)` | one float **vector** — the list collapsed | `'rank` |
| `f[(2f;10f)]` | the same collapsed vector | `'rank` |
| `f(2f;10f;::)` | a general list of two floats | `1024f` |

```q
q)f[2f;10f]
'rank
q)f(2f;10f)
'rank
q)f[(2f;10f)]
'rank
q)f(2f;10f;::)
1024f
```

### Why `::`

The middle two rows are not an FFI rule at all — they are ordinary q. Same-type items written in a list collapse into a
*vector*:

```q
q)type (2f;10f)
9h
```

`9h` is a float vector: **one** value, of count 2. So `f(2f;10f)` hands the binding a single argument where it wanted
two, and the answer is `'rank`. Adding `::` puts a non-float in the list, which stops the collapse and leaves a general
list — and the binding drops the trailing `::` before marshalling. That is all the sentinel is for.

This also explains the call that *seems* to work by accident. A mixed list cannot collapse, so `f(1;2;"testing")` needs
no sentinel — right up until the day two of its arguments share a type.

This is KX's convention, not a peachq invention: every published ffikdb example is written `f (a;b;c;::)`. (The same
collapse rule bites character data in [`regex.md`](regex.md#the-collapse-trap), for the same reason.)

### The peachq superset

peachq also accepts the call **without** the sentinel when the bound arity already says how many arguments there are and
the values cannot collapse into one:

```q
q)strlen:.ffi.bind[`strlen;"C";"i"]
q)strlen "12345"
5i
```

**This is a superset, not KX behaviour.** Write the `::` anyway: it is the spelling that is correct on both, and it is
the only spelling whose meaning does not change the moment two arguments come to share a type.

## The environment functions, and their three different return types

These three report the host. They are the first thing most FFI code calls, and they do **not** all return the same kind
of thing:

| function | example | type |
|---|---|---|
| `.ffi.extension[]` | `` `so `` / `` `dll `` / `` `dylib `` | symbol, `-11h` |
| `.ffi.os[]` | `"l"` / `"w"` / `"m"` | **char**, `-10h` |
| `.ffi.ptrsize[]` | `8i` / `4i` | int, `-6h` |

```q
q).ffi.extension[]
`so
q).ffi.os[]
"l"
q).ffi.ptrsize[]
8i
q)type .ffi.extension[]
-11h
q)type .ffi.os[]
-10h
q)type .ffi.ptrsize[]
-6h
```

So building a platform-correct library name works — `.ffi.extension` returns a symbol, which is what `` ` sv `` joins:

```q
q)` sv `qr,.ffi.extension[]
`qr.so
```

and the same idiom with `.ffi.os` does not, because a char is not a symbol:

```q
q)` sv `qr,.ffi.os[]
'type
```

Both return types are KX's, kept deliberately: `.ffi.os` is `{[] first string .z.o}` verbatim, and published examples
branch on it with `"l"=.ffi.os[]`, which would itself be `'type` against a symbol. If you want the OS letter *in* a
symbol, convert it: `` `$ "qr", .ffi.os[] ``.

## Loading

`.ffi` is part of the standard library, behind the one gate:

```q
q)\l pq
```

Before the gate the namespace does not exist, so a pre-gate environment stays kdb-clean.

## `bind` and `callFunction`

Both call C. The difference is *when* the work happens.

`.ffi.bind[funcname;argtypes;returntype]` resolves the symbol and builds the call interface **once**, and hands back the
unary function described above. Use it on anything hot.

`.ffi.callFunction[returnfunc;arglist]` does that work on **every** call, and takes the argument list directly. Use it
for a one-shot.

```q
q).ffi.callFunction[("f";`sqrt)] (16f;::)
4f
q).ffi.callFunction[("i";`abs)] (-7i;::)
7i
```

Two spellings name the function, and both `bind` and `callFunction` take them:

| spelling | meaning |
|---|---|
| `` `strlen `` | a symbol already resolvable in the running process |
| `` `libm.so.6`pow `` | an explicit library and symbol — the `dlopen` path |

`callFunction` and `cvar` accept a **third** spelling that `bind` does not: `("f";`sqrt)`, a return-type letter paired
with either of the rows above. They need it because they are told nothing else about types — `callFunction` infers the
argument types from the values you pass, and with a bare symbol the return letter defaults to `"i"`. `bind` is given
`returntype` as its own argument, so handing it the paired form instead is `'type`:

```q
q).ffi.bind[("f";`sqrt);"f";"f"]
'type
```

Being told all the types up front is what lets `bind` build the interface once.

A bare symbol resolves against the already-linked process, which is why `strlen`, `sprintf`, `qsort`, `getpid` and `pow`
need no library name here. Name the library explicitly for anything that is not already loaded.

## Type characters

One letter per argument, one for the return. The letters describe the *C prototype*; the values marshal from the actual
q arguments.

| letter | C type | q value |
|---|---|---|
| `b` | `unsigned char` (0/1) | boolean |
| `c` | `char` | char |
| `x`, `g` | `unsigned char` | byte |
| `h` | `short` | short |
| `i` | `int` | int |
| `j` | `long long` | long |
| `l` | `size_t` | long |
| `e` | `float` | real |
| `f` | `double` | float |
| `s` | `char *` | symbol |
| `m` `d` `u` `v` `t` | `int` | month, date, minute, second, time |
| `p` `n` | `long long` | timestamp, timespan |
| `z` | `double` | datetime |
| `r` | a raw pointer, passed as an int or long holding the address | — |
| `k` | a callback — see below | — |
| `" "` (space) | `void` | return only; the call answers `::` |
| **uppercase** | **pointer to that type** | dereferenced on return |

```q
q)lround:.ffi.bind[`lround;"f";"j"]
q)lround (2.6;::)
3
q)labs:.ffi.bind[`labs;"j";"j"]
q)labs (-7;::)
7
```

An empty `argtypes` is a zero-argument function. It still takes an argument list — the list is just the sentinel alone:

```q
q)b0:.ffi.bind[`getpid;"";"i"]
q).z.i=b0 (::)
1b
```

## Pointers

An uppercase letter is a pointer to the lowercase type. On return the pointer is dereferenced, and `C` — a
NUL-terminated `char *` — comes back as a string:

```q
q)sr:.ffi.bind[`strerror;"i";"C"]
q)sr (2i;::)
"No such file or directory"
```

A NULL pointer answers the typed null for its letter rather than crashing.

**A numeric vector crosses as an in-place data pointer**, so anything C writes through it is visible in q immediately.
That is how out-parameters work:

```q
q)p:-1 -1i
q).ffi.callFunction[("i";`pipe)] (p;::)
0i
q)all p>=0
1b
q)(.ffi.callFunction[("i";`close)] (p 0;::)),.ffi.callFunction[("i";`close)] (p 1;::)
0 0i
```

Character vectors are the one place peachq diverges, as a compatible superset: a char vector is **auto-NUL-terminated
through a copy** unless it already ends in an explicit `"\000"`. Only the explicit tail keeps the in-place contract —
which is exactly why an output buffer is built with one:

```q
q)x:80#"\000"
q)n:.ffi.callFunction[("i";`sprintf)] (x;"%s %f %hd\000";"test\000";2f;0h;::)
q)n
15i
q)"test 2.000000 0"~x til "j"$n
1b
```

A symbol atom marshals as `char *`. A symbol *vector* is different again: it crosses as a `char *[]` of copied strings,
and what comes back is the **permutation** C left them in — which is what makes sorting one from C work:

```q
q)s:`c`a`b
q).ffi.callFunction[(" ";`qsort)] (s;`int$count s;.ffi.ptrsize[];({(x>y)-x<y};"SS";"i");::)
q)s
`a`b`c
```

The `r` letter takes an address you already hold as an int or long, for the case where a C function hands you an opaque
handle to give back later.

## Callbacks

A q function crosses to C as a function pointer under the `k` letter. You pass it as the tuple
`(function;"argtypes";"returntype")` — the argument letters describe what **C** will call it with:

```q
q)cmp:{(x>y)-x<y}
q)qs:.ffi.bind[`qsort;"liik";" "]
q)u:5 4 1 3 2i
q)qs (u;`int$count u;4i;(cmp;"II";"i");::)
q)u
1 2 3 4 5i
```

`"II"` because `qsort` hands its comparator two `const void *` that point at ints, and `"i"` because it wants an int
back. A callback may itself make foreign calls; re-entering is fine.

### When a callback fails

This part is **peachq's, beyond KX**, and it is deliberate. A q error inside a callback does not longjmp through C. It
*parks*: this invocation and every later one inside the same foreign call return neutral zero results, the foreign call
completes normally, and the parked error then propagates as the q result of the call.

```q
q)qs (u;`int$count u;4i;({[x;y] '"kaboom"};"II";"i");::)
'kaboom
q)2+2
4
```

The evaluator is healthy afterwards, the vector keeps its shape, and the same binding works again. A callback returning
something that will not convert is `'type` rather than a silently faked zero:

```q
q)qs (u;`int$count u;4i;({[x;y]`abc};"II";"i");::)
'type
```

Closures, like bindings, live until the process exits — KX parity.

## `cvar` — reading a C global

`.ffi.cvar` takes the same shape `callFunction` takes for its function, and reads a variable instead of calling
anything:

```q
q)0<.ffi.cvar ("r";`stdout)
1b
```

## `setErrno`

Sets `errno` and returns its **previous** value. Called with no argument it only reads:

```q
q){.ffi.setErrno 7i; .ffi.setErrno[]} []
7i
```

## Errors

peachq answers bare q error classes here. KX embeds a message string in the error text; we do not, which is a recorded
divergence — the class is the contract.

| error | means |
|---|---|
| `'os` | the symbol did not resolve — wrong name, or the library is not loaded |
| `'type` | a type letter outside the table, a malformed function spelling, or a value that will not convert |
| `'rank` | the argument list is the wrong length — **this is where the collapse trap lands** |
| `'limit` | `callFunction` was given more than 32 arguments (`bind` answers `'type` for more than 32 letters) |
| `'nyi` | this build has no libffi |

```q
q).ffi.bind[`no_such_fn_xyz;"";" "]
'os
q).ffi.bind[`strlen;"q";"i"]
'type
q)strlen ("a\000";"b\000";::)
'rank
```

The vendored libffi links on Linux x86-64 and its Windows cross-build only. Everywhere else — macOS, the WebAssembly
browser build — `.ffi` loads but every call answers `'nyi`. That is the one meaning of `'nyi` here: the build, never the
arguments.

## Where peachq matches KX, and where it is a superset

| | |
|---|---|
| **Matches KX** | every public spelling; the one-argument-list calling convention and the `::` sentinel; the type letters; uppercase-is-a-pointer; numeric vectors crossing as in-place data pointers, and symbol vectors as a copied `char *[]` with the permutation written back; bindings and closures living until exit; `.ffi.extension` byte for byte |
| **Superset** | the `::` may be omitted when the bound arity disambiguates and the arguments cannot collapse; char vectors auto-NUL-terminate through a copy unless an explicit `"\000"` tail is present; a q error inside a callback parks and propagates rather than longjmping; errors are bare classes with no embedded message |

Nothing in the superset column changes the meaning of code written to the KX rules. If you write ffikdb-portable q —
sentinel always, explicit NUL tails on buffers — it runs identically here.
