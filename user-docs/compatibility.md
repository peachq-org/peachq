# Compatibility with kx q

peachq aims to be a drop-in q replacement, measured by q-observable behaviour: `parse` display, `type`, error
text. Where it deliberately does **not** follow kx, the divergence is recorded here with its reason. This page is
for choices we have made on purpose. Bugs and unfinished work are not divergences — they live in `PLAN.md`.

## Pattern matching (kdb+ 4.1) — not replicated

kdb+ 4.1 added pattern matching to assignment and to lambda parameters:

```q
(1):1                          / assert the right side matches the literal
(1 2):1 3                      / 'match
f:{[x;]x}                      / trailing empty parameter
c2f:{[x:tempCheck]32+1.8*x}    / run tempCheck against the argument on entry
```

**peachq does not implement this and does not intend to.** The forms above signal `'parse`.

The reason is not effort, it is who can read it. Pattern matching puts arbitrary *runtime* behaviour into a
signature: `{[x:tempCheck] … }` means "call `tempCheck` on entry", so the only way to know what that function
accepts is to execute q. Every tool outside the interpreter — an editor, an IDE's completion and hover, a linter,
a doc generator, a code reviewer reading a diff — is then either blind or forced to embed a q runtime.

peachq takes the other route: a **static** signature that says what it means in its own text, and that anything
can read cheaply without running code. That is [typed parameters](typed-parameters.md) — declared types,
optional arguments, defaults, varargs — where a type is a name (``x:`j``), not a function call, so a signature
can be checked by reading it.

We would rather have one description of a function's interface that every tool can use than a more expressive
one that only the interpreter can evaluate.

### What this means in practice

- The assertion forms (`(1):1`, `(1 2):1 2`) have no peachq equivalent. Use `~` and a signal, or `.qunit`
  asserts in tests.
- A trailing empty parameter (`{[x;]x}`) is not accepted; write the arity you mean.
- Argument validation on entry is expressed as a **type** in the signature where a type suffices, and as an
  ordinary check in the body where it does not.
- `test/q/extracted/basics/pattern.qcmd` is extracted from the kx documentation, so it covers the whole feature.
  The rows for the unsupported forms are commented out rather than left red — a permanent red row asserts we
  intend to fix something, and here we do not. The remaining rows still run.
