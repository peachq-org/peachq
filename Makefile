# peachq — build ./q from source.
#   make            # optimized
#   ./q -p 5000     # kdb-wire server

CC      ?= cc

RAY_ENTRY_SRC = src/app/main.c src/qlang/repl/qmain.c src/qlang/repl/qdoctest_main.c
RAY_LIB_SRC   = $(filter-out $(RAY_ENTRY_SRC), $(wildcard src/*/*.c src/qlang/eval/*.c src/qlang/ops/*.c src/qlang/net/*.c src/qlang/io/*.c \
                                            src/qlang/parse/*.c src/qlang/base/*.c src/qlang/repl/*.c))

RAY_VENDOR_SRC = third_party/yyjson/yyjson.c \
                 third_party/picohttpparser/picohttpparser.c \
                 third_party/miniz/miniz.c

# libffi: generated configs are COMMITTED per target (third_party/libffi/README.openq.md),
# so no configure step. Ports vendored: linux x86-64 native + win64 cross only —
# other native hosts (macOS arm64) build WITHOUT libffi until their port is vendored.
# ffi64.c/unix64.S are SysV-only; ffiw64.c/win64.S build on both vendored targets.
UNAME_M := $(shell uname -m)
LIBFFI_INC        = -Ithird_party/libffi/include -Ithird_party/libffi/src/x86
FFI_INC           = -Ithird_party/libffi/config/linux-x86_64 $(LIBFFI_INC)
FFI_WIN_INC       = -Ithird_party/libffi/config/win64 $(LIBFFI_INC)
ifeq ($(shell uname -s)-$(UNAME_M),Linux-x86_64)
LIBFFI_WIN_OBJ    = $(BUILD_DIR)/third_party/libffi/src/x86/win64.win.o
RAY_VENDOR_SRC   += third_party/libffi/src/prep_cif.c \
                    third_party/libffi/src/types.c \
                    third_party/libffi/src/closures.c \
                    third_party/libffi/src/tramp.c \
                    third_party/libffi/src/x86/ffiw64.c
LIBFFI_NATIVE_OBJ = $(addprefix $(BUILD_DIR)/third_party/libffi/src/x86/,ffi64.o unix64.o win64.o)
QFFI_DEF          = -DRAY_FFI=1
endif

# q_gz.c hand-rolls RFC 1952 framing: miniz has no gzip windowBits+16 mode.
RAY_MINIZ_DEFS = -DMINIZ_NO_ARCHIVE_APIS -DMINIZ_NO_ARCHIVE_WRITING_APIS -DMINIZ_NO_STDIO

BUILD_DIR = build
# Generated headers land in $(BUILD_DIR)/gen (never the source tree); its -I
# sits AHEAD of -Isrc so every `#include "qlang/X_gen.h"` line stays byte-identical.
GEN_DIR = $(BUILD_DIR)/gen

RAY_INCLUDES = -Iinclude -I$(GEN_DIR) -Isrc \
               -Ithird_party/yyjson -Ithird_party/picohttpparser -Ithird_party/miniz

RAY_GEN_HDRS = $(GEN_DIR)/qlang/dotq_gen.h $(GEN_DIR)/qlang/h_gen.h \
               $(GEN_DIR)/qlang/j_gen.h $(GEN_DIR)/qlang/lib_gen.h \
               $(GEN_DIR)/qlang/html_assets_gen.h

# q.q before dotq.q — dotq.q may use q.q keywords, never the reverse.
$(GEN_DIR)/qlang/dotq_gen.h: src/qlang/q.q src/qlang/dotq.q tools/gen-bootstrap.sh
	@mkdir -p $(dir $@)
	tools/gen-bootstrap.sh $@ src/qlang/q.q src/qlang/dotq.q

$(GEN_DIR)/qlang/h_gen.h: src/qlang/h.q tools/gen-bootstrap.sh
	@mkdir -p $(dir $@)
	SYMBOL=OPENQ_H_BOOTSTRAP tools/gen-bootstrap.sh $@ src/qlang/h.q

$(GEN_DIR)/qlang/j_gen.h: src/qlang/j.q tools/gen-bootstrap.sh
	@mkdir -p $(dir $@)
	SYMBOL=OPENQ_J_BOOTSTRAP tools/gen-bootstrap.sh $@ src/qlang/j.q

# The standard library: lib/*.q TOP LEVEL ONLY (lib/qunit/ deliberately out),
# sorted for determinism — the ANY-ORDER LAW makes the order semantically moot.
# The directory itself is a prerequisite (spelled lib/. — bare `lib` is the
# librayforce.a target): deleting/renaming a file bumps the dir mtime, which
# the file-only list cannot see (the html-assets rule's law).
LIB_Q_SRCS := $(sort $(wildcard lib/*.q))
$(GEN_DIR)/qlang/lib_gen.h: lib/. $(LIB_Q_SRCS) tools/gen-bootstrap.sh
	@mkdir -p $(dir $@)
	SYMBOL=OPENQ_LIB_BOOTSTRAP tools/gen-bootstrap.sh $@ $(LIB_Q_SRCS)

# Dirs in the prerequisite list: deleting an asset bumps its directory's mtime,
# which a file-only list cannot see. A no-change make must not touch this rule.
HTML_ASSET_DEPS := $(shell find src/qlang/html -type f -o -type d 2>/dev/null)
$(GEN_DIR)/qlang/html_assets_gen.h: tools/gen-assets.sh $(HTML_ASSET_DEPS)
	@mkdir -p $(dir $@)
	@tools/gen-assets.sh $@ src/qlang/html

# Both suffixes: a .win.o inherits none of the .o target's prerequisites, which is
# how the mingw build broke while the native one was already fixed.
$(BUILD_DIR)/src/qlang/q_runtime.o $(BUILD_DIR)/src/qlang/q_runtime.win.o: \
    $(GEN_DIR)/qlang/dotq_gen.h $(GEN_DIR)/qlang/h_gen.h $(GEN_DIR)/qlang/j_gen.h
$(BUILD_DIR)/src/qlang/q_pq.o   $(BUILD_DIR)/src/qlang/q_pq.win.o:   $(GEN_DIR)/qlang/lib_gen.h
$(BUILD_DIR)/src/qlang/net/q_http.o $(BUILD_DIR)/src/qlang/net/q_http.win.o: $(GEN_DIR)/qlang/html_assets_gen.h

STD      = c17
Q_TARGET = q

# Single source of truth: the VERSION file. Override: make RAY_VERSION=X.Y.Z
RAY_VERSION  ?= $(shell cat VERSION 2>/dev/null || echo 0.41)
VERSION_MAJOR := $(word 1,$(subst ., ,$(RAY_VERSION)))
VERSION_MINOR := $(word 2,$(subst ., ,$(RAY_VERSION)))
VERSION_PATCH := $(word 3,$(subst ., ,$(RAY_VERSION)))
ifeq ($(strip $(VERSION_MINOR)),)
  VERSION_MINOR := 0
endif
ifeq ($(strip $(VERSION_PATCH)),)
  VERSION_PATCH := 0
endif
BUILD_DATE := $(shell date -u +%Y-%m-%d)

WARNS   = -Wall -Wextra -Wno-unused-parameter
DEFS    = -DRAY_VERSION_MAJOR=$(VERSION_MAJOR) -DRAY_VERSION_MINOR=$(VERSION_MINOR) \
          -DRAY_VERSION_PATCH=$(VERSION_PATCH) -DRAYFORCE_VERSION=\"$(RAY_VERSION)\"
# Changes daily, so scoped to the four objects that read it — otherwise every
# cached object misses at midnight.
DATE_DEF   = -DRAYFORCE_BUILD_DATE=\"$(BUILD_DATE)\"
DATE_STEMS = $(addprefix $(BUILD_DIR)/,src/app/repl src/ops/system src/qlang/q_dotz src/qlang/repl/qmain)
$(addsuffix .o,$(DATE_STEMS)) $(addsuffix .win.o,$(DATE_STEMS)): DEFS += $(DATE_DEF)
INCLUDES = $(RAY_INCLUDES)
DEPFLAGS = -MMD -MP

# A -march=native binary handed to an older CPU dies with SIGILL, so redistributable
# builds override: make RAY_MARCH=x86-64-v3
RAY_MARCH ?= native

RELEASE_CFLAGS = -fPIC $(WARNS) -std=$(STD) -O2 -march=$(RAY_MARCH) \
  -funroll-loops -fomit-frame-pointer -fno-math-errno \
  -fassociative-math -ffp-contract=fast -fno-signed-zeros -fno-trapping-math

UNAME_S := $(shell uname -s)
# -ldl: OpenSSL is dlopen'd at runtime, never linked (see qlang/net/q_tls.c).
ifeq ($(UNAME_S),Linux)
  LIBS = -lm -lpthread -ldl
else
  LIBS = -lm
endif

CFLAGS  ?= $(RELEASE_CFLAGS)
LDFLAGS ?=

LIB_SRC = $(RAY_LIB_SRC) $(RAY_VENDOR_SRC)
LIB_OBJ    = $(addprefix $(BUILD_DIR)/,$(LIB_SRC:.c=.o)) $(LIBFFI_NATIVE_OBJ)
Q_MAIN_OBJ = $(BUILD_DIR)/src/qlang/repl/qmain.o
DEPS = $(LIB_OBJ:.o=.d) $(Q_MAIN_OBJ:.o=.d)

.DEFAULT_GOAL := all
all: git-merge-drivers $(Q_TARGET) $(RE2_MODULE)

# `merge=ours` on test/observed/** (.gitattributes) is NOT a git built-in the
# way `union` is — without this per-clone config the attribute is silently
# inert and the mirror conflicts on every overlapping branch.  Idempotent;
# skipped outside a git checkout.
.PHONY: git-merge-drivers
git-merge-drivers:
	@git rev-parse --git-dir >/dev/null 2>&1 && \
	  git config merge.ours.driver true || true

# Vendored TUs: not ours to fix, and -Wextra on them fails the build.
$(BUILD_DIR)/third_party/%.o: third_party/%.c
	@mkdir -p $(dir $@)
	$(CC) -c $(filter-out -Wextra,$(CFLAGS)) -Wno-error $(DEPFLAGS) $(DEFS) $(INCLUDES) -o $@ $<

$(BUILD_DIR)/third_party/miniz/miniz.o: third_party/miniz/miniz.c
	@mkdir -p $(dir $@)
	$(CC) -c $(filter-out -Wextra,$(CFLAGS)) -Wno-error $(RAY_MINIZ_DEFS) $(DEPFLAGS) $(DEFS) $(INCLUDES) -o $@ $<

$(BUILD_DIR)/third_party/libffi/%.o: third_party/libffi/%.c
	@mkdir -p $(dir $@)
	$(CC) -c $(filter-out -Wextra,$(CFLAGS)) -Wno-error $(DEPFLAGS) $(FFI_INC) -o $@ $<

$(BUILD_DIR)/third_party/libffi/%.o: third_party/libffi/%.S
	@mkdir -p $(dir $@)
	$(CC) -c $(filter-out -Wextra,$(CFLAGS)) -Wno-error $(DEPFLAGS) $(FFI_INC) -o $@ $<

# RAY_FFI comes ONLY from the host gate above: a host that doesn't link the
# libffi objects compiles q_ffi.c (native AND win cross) as the 'nyi stub.
$(BUILD_DIR)/src/qlang/io/q_ffi.o:     INCLUDES += $(FFI_INC)
$(BUILD_DIR)/src/qlang/io/q_ffi.o:     DEFS += $(QFFI_DEF)
$(BUILD_DIR)/src/qlang/io/q_ffi.win.o: INCLUDES += $(FFI_WIN_INC)
$(BUILD_DIR)/src/qlang/io/q_ffi.win.o: DEFS += $(QFFI_DEF)

# Header discipline, qlang-scoped: frozen base src/ must not need edits to pass.
$(BUILD_DIR)/src/qlang/%.o: CFLAGS += -Wmissing-prototypes
$(BUILD_DIR)/src/qlang/%.win.o: WIN_CFLAGS += -Wmissing-prototypes

$(BUILD_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) -c $(CFLAGS) $(DEPFLAGS) $(DEFS) $(INCLUDES) -o $@ $<

$(Q_TARGET): $(LIB_OBJ) $(Q_MAIN_OBJ)
	$(CC) $(CFLAGS) -o $@ $(LIB_OBJ) $(Q_MAIN_OBJ) $(LIBS) $(LDFLAGS)

# --- libpqre2: the dlopen'd RE2 module (user-docs/regex.md) --------------------
# The only C++ in the tree, fenced into its own shared object BY DESIGN: `./q`
# stays pure C17 and links no libstdc++, and a missing module is answered with
# 'regex rather than a broken binary.  So this is never a prerequisite of
# $(Q_TARGET) — it is built alongside it, and only where a C++ compiler exists
# (RE2_MODULE is empty otherwise, which is exactly what "C++ is optional" means).
# The engine finds it on the q_re2.c ladder: $PEACHQ_RE2_LIB, $QHOME, the exe
# dir (here), then the system path.
CXX         ?= g++
# The filename is the LOADER's, not a choice: q_re2.c's RE2_LIB_BASENAME is what
# gets dlopen'd, so building libpqre2.so on a host that looks for a .dylib would
# ship a module nothing can find.
RE2_TARGET   = $(if $(filter Darwin,$(UNAME_S)),libpqre2.dylib,libpqre2.so)
RE2_MODULE  := $(if $(shell command -v $(CXX) 2>/dev/null),$(RE2_TARGET),)
RE2_SRC      = $(wildcard third_party/re2/re2/*.cc third_party/re2/util/*.cc)
RE2_OBJ      = $(addprefix $(BUILD_DIR)/,$(RE2_SRC:.cc=.o)) \
               $(BUILD_DIR)/src/qlang/io/q_re2_shim.o
# -fvisibility=hidden keeps RE2 private: only the seven PQRE2_API entry points
# are exported.  c++17 over upstream's c++11 — the shim uses nothing newer, but
# the vendored sources build clean either way and c++17 is the tree's baseline.
RE2_CXXFLAGS = -std=c++17 -O2 -fPIC -fvisibility=hidden -Ithird_party/re2 -Isrc

$(BUILD_DIR)/third_party/re2/%.o: third_party/re2/%.cc
	@mkdir -p $(dir $@)
	$(CXX) -c $(RE2_CXXFLAGS) -w $(DEPFLAGS) -o $@ $<

$(BUILD_DIR)/src/qlang/io/%.o: src/qlang/io/%.cc
	@mkdir -p $(dir $@)
	$(CXX) -c $(RE2_CXXFLAGS) $(WARNS) $(DEPFLAGS) -o $@ $<

# The pin check runs at every relink: a re-vendored tree that still claims the
# old digest fails HERE, not silently at match time (tools/re2-pin.sh).
$(RE2_TARGET): $(RE2_OBJ)
	@tools/re2-pin.sh
	$(CXX) -shared -o $@ $(RE2_OBJ) -lpthread

RE2_DEPS = $(RE2_OBJ:.o=.d)

# --- Windows cross-build (mingw-w64): make win --------------------------------
# RAY_OS_WINDOWS on the command line because some files test it before the
# platform header is visible.
#
# Optimisation parity with RELEASE_CFLAGS (2026-07-29): the win build was plain
# -O2 while the native release build also got -march, unrolling and the relaxed
# float flags, so q.exe shipped slower for no stated reason.  Two notes on why
# this set is the safe one:
#   - -march defaults to the x86-64-v2 baseline (SSE4.2/POPCNT, Nehalem 2008)
#     rather than v3, because the target CPU is unknown and a v3 binary SIGILLs
#     on a pre-AVX2 machine.  Override for known-modern targets:
#     make win RAY_WIN_MARCH=x86-64-v3
#   - the float flags are exactly the native set, which deliberately EXCLUDES
#     -ffinite-math-only / -ffast-math: the live-infinity model (2026-07-28)
#     needs +-Inf to stay ordinary values and NaN to stay the null.  Parity also
#     stops q.exe and ./q disagreeing on float results.
# WIN_OPT is the ONE home for the win optimisation set: Makefile.dev redefines
# WIN_CFLAGS (stricter warnings, dev tree only) and references this, so the two
# WIN_CFLAGS definitions cannot drift on optimisation.
RAY_WIN_MARCH ?= x86-64-v2
RAY_WIN_JOBS  ?= 4
WIN_OPT = -O2 -march=$(RAY_WIN_MARCH) -funroll-loops -fomit-frame-pointer \
  -fno-math-errno -fassociative-math -ffp-contract=fast -fno-signed-zeros \
  -fno-trapping-math
WIN_CROSS  ?= x86_64-w64-mingw32-
WIN_CC      = $(WIN_CROSS)gcc
WIN_CFLAGS  = $(WARNS) -std=$(STD) $(WIN_OPT) \
  -DRAY_OS_WINDOWS=1 -D_WIN32_WINNT=0x0A00 -D__USE_MINGW_ANSI_STDIO=1
WIN_LIBS    = -lws2_32 -lm
# iocp_win.c provides ray_poll_* on Windows; linking the iocp.c stub too is a
# multiple-definition error.
WIN_LIB_OBJ    = $(filter-out $(BUILD_DIR)/src/core/iocp.win.o, $(addprefix $(BUILD_DIR)/,$(LIB_SRC:.c=.win.o))) \
                 $(LIBFFI_WIN_OBJ)
WIN_Q_MAIN_OBJ = $(Q_MAIN_OBJ:.o=.win.o)
WIN_DEPS = $(WIN_LIB_OBJ:.o=.d) $(WIN_Q_MAIN_OBJ:.o=.d)

$(BUILD_DIR)/third_party/%.win.o: third_party/%.c
	@mkdir -p $(dir $@)
	$(WIN_CC) -c $(filter-out -Wextra,$(WIN_CFLAGS)) -Wno-error $(DEPFLAGS) $(DEFS) $(INCLUDES) -o $@ $<

$(BUILD_DIR)/third_party/miniz/miniz.win.o: third_party/miniz/miniz.c
	@mkdir -p $(dir $@)
	$(WIN_CC) -c $(filter-out -Wextra,$(WIN_CFLAGS)) -Wno-error $(RAY_MINIZ_DEFS) $(DEPFLAGS) $(DEFS) $(INCLUDES) -o $@ $<

$(BUILD_DIR)/third_party/libffi/%.win.o: third_party/libffi/%.c
	@mkdir -p $(dir $@)
	$(WIN_CC) -c $(filter-out -Wextra,$(WIN_CFLAGS)) -Wno-error $(DEPFLAGS) $(FFI_WIN_INC) -o $@ $<

$(BUILD_DIR)/third_party/libffi/%.win.o: third_party/libffi/%.S
	@mkdir -p $(dir $@)
	$(WIN_CC) -c $(filter-out -Wextra,$(WIN_CFLAGS)) -Wno-error $(DEPFLAGS) $(FFI_WIN_INC) -o $@ $<

$(BUILD_DIR)/%.win.o: %.c
	@mkdir -p $(dir $@)
	$(WIN_CC) -c $(WIN_CFLAGS) $(DEPFLAGS) $(DEFS) $(INCLUDES) -o $@ $<

q.exe: $(WIN_LIB_OBJ) $(WIN_Q_MAIN_OBJ)
	$(WIN_CC) $(WIN_CFLAGS) -o $@ $(WIN_LIB_OBJ) $(WIN_Q_MAIN_OBJ) $(WIN_LIBS)

# Recursive so the -j lands on the object rules even when the outer make is
# serial (win-smoke and the bare `make win` both go through here).
win:
	+@$(MAKE) --no-print-directory -j$(RAY_WIN_JOBS) q.exe

clean::
	-rm -rf $(BUILD_DIR)
	-rm -f $(Q_TARGET) q.exe $(RE2_TARGET)

version:
	@echo $(RAY_VERSION)

.PHONY: all clean version

# The development tree adds tests, gates, benches and the release tooling. It is
# NOT shipped, so -include silently skips it in the public tree — which is how the
# public build stays the build everyone actually runs.
-include Makefile.dev

-include $(DEPS)
-include $(RE2_DEPS)
