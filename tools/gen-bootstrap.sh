#!/bin/sh
# gen-bootstrap.sh — turn an authored .q file into a generated C header that
# exposes its bytes as `static const char OPENQ_BOOTSTRAP[]`. Part of the
# openq embedded-bootstrap pipeline (ARCHITECTURE.md BIGDECISION): src/qlang/
# dotq.q is the source of truth, this bakes it into the binary. DO NOT edit the
# generated output — edit dotq.q and rebuild.
#
# Usage: gen-bootstrap.sh <input.q> <output.h>
# Portable: POSIX sh + awk only; runs on the BUILD host (native and mingw
# cross-builds alike), so there is no target-side dependency.
set -eu

in=$1
out=$2

# Escape each source line for a C string literal: strip a trailing CR (CRLF
# safety), backslash first, then double-quote; emit one "..\n" chunk per line
# so the embedded text keeps its newlines (the loader splits on '\n' and skips
# blank / `/`-comment lines). One chunk per line keeps the seed's inner quotes
# (e.g. .Q.b6's "..+/") correctly escaped.
{
    printf '/* AUTO-GENERATED from %s by tools/gen-bootstrap.sh — DO NOT EDIT. */\n' "$in"
    printf '#ifndef OPENQ_DOTQ_GEN_H\n#define OPENQ_DOTQ_GEN_H\n'
    printf 'static const char OPENQ_BOOTSTRAP[] =\n'
    awk '
        { sub(/\r$/, ""); gsub(/\\/, "\\\\"); gsub(/"/, "\\\""); printf "\"%s\\n\"\n", $0 }
        END { print ";" }
    ' "$in"
    printf '#endif /* OPENQ_DOTQ_GEN_H */\n'
} > "$out"
