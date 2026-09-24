# C extensions (`2:`)

`2:` loads a function from a shared library written against kdb's C API (`k.h`) and hands it back as a q function.
It is kdb's [Dynamic Load](https://code.kx.com/q/ref/dynamic-load/), and it is always on — no `\l pq` needed.

An extension you already built for kx q loads **as it is, without recompiling**. A new one is written exactly as
kx's [Using C functions](https://code.kx.com/q/interfaces/using-c-functions/) describes.

| Platform | Status |
|---|---|
| Linux x86-64 | Supported |
| Windows x64 | Supported, except `sd1` |
| macOS | Not yet |

If you want to call a plain C library (libm, libc, your own `.so` that knows nothing about `k.h`), you want
[Foreign functions](ffi.md) instead.

## Loading a function

```q
q)add:`:./add 2:(`add;2)
q)add[2;3]
5
```

The left side names the library, the right side is `` (`function;rank) ``. The result is an ordinary q function
(`type` is `112h`): it projects, iterates and displays like any other.

```q
q)inc:add 1
q)inc each 1 2 3
2 3 4
q)add
`:./add 2:(`add;2)
q)value add
`:./add
`add
2
```

Leave the suffix off and `.so` is added (`.dll` on Windows). The library is looked for, in order:

1. at the path as given (relative to the current directory);
2. under `$QHOME/l64/` (`$QHOME/w64/` on Windows) — kdb's own fallback;
3. beside the script that is doing the loading — so a project that ships `foo.q` and `foo.so` side by side works
   wherever it is run from.

If none is found, the error names every path tried:

```q
q)`:missing 2:(`add;2)
'missing.so /home/me/q/l64/missing.so
```

A library that loads but has no such function signals the function's name: `'nosuch`.

## Moving an extension from kx q

1. **Put the library where it was.** In `$QHOME/l64/` (`w64/` for a `.dll`), or beside the `.q` file that loads
   it. Your `2:` lines do not change.
2. **Check it only uses functions peachq provides.** Most of `k.h` is here (see [the list](#what-is-provided));
   a library that needs a missing one fails to load. Check before you start:

   ```bash
   nm -u myext.so | grep -v GLIBC
   ```

   Every name that prints should be a `k.h` function from the list below (or from a library you link yourself).
3. **Run your tests.** The differences that can show up are few, and all are listed under
   [Behaviour differences](#behaviour-differences).

It must have been built with `KXVER=3` (every kdb+ 3.x/4.x extension is). `KXVER=2` layouts are not supported.

## Starting a new extension

Write it against `k.h` exactly as kx documents. Use the header from
[KxSystems/kdb](https://github.com/KxSystems/kdb/blob/master/c/c/k.h) — peachq builds against that published header.

```c
#include "k.h"

K add(K x, K y) {
    if (x->t != -KJ || y->t != -KJ) return krr("type");
    return kj(x->j + y->j);
}

K total(K x) {
    if (x->t != KF) return krr("type");
    F s = 0;
    for (J i = 0; i < x->n; i++) s += kF(x)[i];
    return kf(s);
}

/* call back into q: k() takes ownership of its arguments, so r1() the ones you were lent */
K twice(K f, K x) {
    return k(0, "{x x y}", r1(f), r1(x), (K)0);
}
```

Build it with no q library to link — the `k.h` functions are resolved from the running `q` when the library loads:

```bash
gcc -shared -fPIC -DKXVER=3 add.c -o add.so
```

On Windows, build the DLL as kx documents: link against an import library that names `q.exe` — kx's `q.lib`, or
with MinGW a `libq.a` made by `dlltool` from a `.def` (`LIBRARY q.exe`, then `EXPORTS` and the names you use).
peachq's `q.exe` exports the same names under the same module name. The MinGW route is what peachq's own tests run.

```q
q)add:`:./add 2:(`add;2)
q)add[2;3.5]
'type
q)total:`:./add 2:(`total;1)
q)total 1 2 3.5
6.5
q)twice:`:./add 2:(`twice;2)
q)twice[{x*10};7]
700
```

The rules are kdb's, from its [C API reference](https://code.kx.com/q/interfaces/capiref/):

- **Arguments are lent to you.** Don't `r0` them. To keep one past the call, `r1` it.
- **What you return is owned by q.** Build it with `kj`, `ktn` and friends; to hand an argument straight back,
  `return r1(x);`.
- **Signal an error with `krr("text")`.** q sees `'text`.
- **`k(0, "expr", args…, (K)0)`** evaluates in the running q and takes ownership of `args`.
- **`sd1(fd, callback)`** puts your file descriptor on q's event loop; q calls `callback` on the main thread
  when it is readable. This is how a GUI or a background thread wakes q up. Linux only for now.

## Projects known to work

- **[embedPy](https://github.com/KxSystems/embedPy)** — Python inside q, on Linux. Its own test suite passes in
  full (268 tests) with a small patch to `p.q`: two `k)` one-liners rewritten in q, since peachq does not run k.
  Its published examples give the documented values too.
- **[qVis](https://github.com/mkeenan-kdb/qVis)** — an SDL3 pixel canvas, on Linux. It loads and draws — window,
  pixels, shapes, bulk pixel upload, keyboard and mouse polling, driven through `sd1` — and its `qOS` desktop's
  smoke test passes. Its inspector's drill-down still stops on a known table-append bug.

## Troubleshooting

| You see | Likely cause |
|---|---|
| `'foo.so …` listing paths | Not found at any of them — or found but it needs a function peachq does not provide. Run `nm -u foo.so`. |
| `'myfunc` | The library loaded but does not export `myfunc`. `nm -D foo.so` shows what it does export. |
| `'nyi` from inside a call | A value that cannot cross yet (an enumeration), `k()` with a non-zero handle, or `sd1` on Windows. |
| `'kapi bad-return: lib:fn` | (debug builds) Your function returned a malformed object — bad type, negative count, or one already freed. |
| `'rank` at `2:` | Rank must be 1 to 8. |

## Behaviour differences

- **`x->r` is one higher than kx's** for an object nobody else holds. An extension that mutates an argument in place
  only "when I'm the sole owner" will take its copy branch instead. Correct, slightly slower.
- **Never write an argument's header.** Setting `x->t`, `x->a` or `x->u` on something q lent you changes a live q
  value. (kdb forbids this too; peachq just doesn't forgive it.) `x->a`'s bits are not kdb's attribute values.
- **Enumerations (`20h`–`76h`) do not cross** — `'nyi`. Pass `value` of the column.
- **`k()` works with handle `0` only** (the running q). A remote handle is `'nyi`.
- **Libraries are never unloaded** once loaded; reloading a rebuilt `.so` needs a new session.

### What is provided

Constructors `ka kb kg kh ki kj ke kf kc ks kd kz kt ktj ktn kp kpn knk ku ktd knt xD xT`; list building
`ja js jk jv`; `ss sn krr orr ee dl r1 r0 ymd dj setm m9 gc ver sd1 sd0 sd0x k`.

Not provided: `b9 d9` (serialise), `dot`, `m4`, `okx`, `vi vk vak vaknk`, and the IPC client calls
(`khp khpu khpun khpunc kclose sslInfo`), which belong to kx's `c.o` rather than to an extension.

---

## How it works

*For the curious and for anyone debugging an extension; nothing above depends on it.*

**Zero copy for flat data.** kdb's `struct k0` and peachq's own value header differ only by a fixed 16-byte offset,
and the type numbers are the same, so a `K` handed to your code is a pointer *into* the q value: `kG(x)` is the q
vector's own memory, and what you build with `ktn` is a q vector from birth. Atoms and flat vectors of every
numeric, temporal, char, byte and boolean type cross in both directions without copying.

**Copies where the two models genuinely differ.** Symbols (kdb wants a stable `char*`; peachq stores ids), real
atoms, error objects, and the *spines* of general lists, dictionaries and tables (kdb's are `K*` arrays) are built
fresh at the crossing — but the vectors inside them are still shared, not copied. A slice of a larger vector is
materialised, because `kG(x)` must point at contiguous data.

**Functions and foreign objects.** A q function passed to C arrives as its kdb type (`100h` lambda, `104h`
projection, …) and can be called back with `k(0, …)`. A `2:` result and a foreign object (the `112h` wrapper
embedPy uses for a `PyObject*`) are both `112h` in q; a foreign's destructor runs when q drops the last reference.

**Linking.** `q` exports exactly the `k.h` names above and nothing else, so an extension resolves against them and
cannot collide with peachq's internals. `dlopen` uses `RTLD_NOW`, so a missing name fails at load, not at the first
call. On Windows `q.exe` exports the same list, and a DLL binds to it by module name, just as it binds to kx's `q.exe`.

**Debug builds check what you return.** Tag in range, count non-negative, refcount sane, not already freed —
otherwise `'kapi bad-return` names the library and function at the call that did it, instead of a crash several
statements later.
