# Contributing to peachq

Thank you for your interest in peachq — it genuinely means a lot. Please read
this first, because our contribution policy is a little unusual and it exists to
**protect this project's legal freedom for everyone who uses it**.

## We can't accept outside code contributions (for now)

We would love help, but at the moment **we cannot accept pull requests or patches
of source code from outside contributors.** This isn't about the quality of your
work — it's about keeping peachq unambiguously free and safe to use.

A few reasons, and we hope they make sense:

- **Code provenance.** peachq is built solely from the *published* q language
  reference. Its value — legally and to users — depends on that provenance being
  airtight. Accepting code from people we can't vet risks introducing material
  derived from proprietary sources, which would put every user at risk.
- **We don't have a contributor agreement in place.** Accepting outside code
  responsibly would require a signed contributor licensing/IP agreement, and we
  aren't set up to administer one yet.
- **Your employer might have a claim you don't expect.** If you work at a bank, a
  trading firm, or a company in the kdb+/q space (including its vendor), code you
  contribute could be encumbered by your employment agreement without either of
  us intending it. We'd rather protect you *and* the project from that situation
  entirely.

So please **don't send source-code PRs** — if one is opened here it will be
closed with a friendly pointer back to this page. No offense is ever meant by it.

## How you *can* help — and we'd love it

- **Report bugs and rough edges.** Open an [Issue](../../issues) with a clear
  reproduction: the input, the q output you expected, and what you actually got.
  Conformance is measured against q-observable behaviour, so precise cases are
  gold.
- **Request features / share use cases.** Tell us what you need q to do.
- **Ask questions and discuss** via Issues (or Discussions, if enabled).

None of the above requires any agreement, and all of it is hugely valuable.

## Please don't paste proprietary material

When filing issues, please do **not** include code, algorithms, or expected
outputs taken from any proprietary kdb+ binary or source — decompiled,
disassembled, or copied. Describe behaviour from the published q reference or
from your own observations instead. This keeps peachq safe to use for everyone.

## A note on this repository

This GitHub repo is a **read-only release mirror** of a private development repo;
its history is regenerated on each sync, so even well-intentioned merges here
would be overwritten. That's another reason code changes need to flow through the
upstream process rather than PRs on this mirror.

Thanks again — reports and ideas move this project forward, and we're grateful
for them.
