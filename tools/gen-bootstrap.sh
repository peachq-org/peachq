#!/bin/sh
# gen-bootstrap.sh — turn the authored .q files into a generated C header that
# exposes their bytes as `static const char OPENQ_BOOTSTRAP[]`. Part of the
# openq embedded-bootstrap pipeline (ARCHITECTURE.md BIGDECISION): src/qlang/
# q.q + dotq.q are the source of truth, this bakes them into the binary. DO NOT
# edit the generated output — edit the .q sources and rebuild.
#
# Usage: gen-bootstrap.sh <output.h> <input.q>...
# Inputs are concatenated IN ORDER — the load order contract (q.q keyword
# definitions load before dotq.q, which may use them; never the reverse).
# Portable: POSIX sh + awk only; runs on the BUILD host (native and mingw
# cross-builds alike), so there is no target-side dependency.
set -eu

out=$1
shift

# Escape each source line for a C string literal: strip a trailing CR (CRLF
# safety), backslash first, then double-quote; emit one "..\n" chunk per line
# so the embedded text keeps its newlines (the loader splits on '\n' and skips
# blank / `/`-comment lines). One chunk per line keeps the seed's inner quotes
# (e.g. .Q.b6's "..+/") correctly escaped.
{
    printf '/* AUTO-GENERATED from %s by tools/gen-bootstrap.sh — DO NOT EDIT. */\n' "$*"
    printf '#ifndef OPENQ_DOTQ_GEN_H\n#define OPENQ_DOTQ_GEN_H\n'
    printf 'static const char OPENQ_BOOTSTRAP[] =\n'
    awk '
        { sub(/\r$/, ""); gsub(/\\/, "\\\\"); gsub(/"/, "\\\""); printf "\"%s\\n\"\n", $0 }
        END { print ";" }
    ' "$@"
    printf '#endif /* OPENQ_DOTQ_GEN_H */\n'
} > "$out"
