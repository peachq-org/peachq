# md4c (vendored)

- **Upstream:** https://github.com/mity/md4c
- **Version:** release `v0.6.0` (commit `7fc1815`, 2026-09-17)
- **License:** MIT (see `LICENSE.md`) — Copyright © 2016-2026 Martin Mitáš
- **Files vendored:** `src/md4c.c`, `src/md4c.h` (the parser), `src/md4c-html.c`, `src/md4c-html.h`,
  `src/entity.c`, `src/entity.h` (the HTML renderer and its entity table), `LICENSE.md`

## Why it is here

peachq's `.md` namespace (`lib/md.q`) reads Markdown. md4c does all of the parsing (CommonMark 0.31.2 plus the
tables, strikethrough and task-list extensions) and `.md.html`'s rendering; peachq's glue
(`src/qlang/io/q_md.c`) only adapts md4c's callbacks into q values. `entity.c` also serves the glue: a named
entity in plain text decodes through its table. `md2html` is not vendored.

## Updating

Re-fetch the six files and `LICENSE.md` from the upstream repo at the desired commit and update the version above.
No local modifications are made to the vendored sources; the Makefile's generic `third_party/%.o` rule builds them
without `-Wextra`/`-Werror`.
