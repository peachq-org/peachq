# peachq — minimal public build.
#
# Builds the two binaries from source only:
#   ./q         — the q REPL / interpreter frontend
#   ./rayforce  — the underlying rayfall array engine
#
# Source set: src/**, the single public header include/rayforce.h, and the
# vendored third_party/ components (yyjson, picohttpparser, miniz). No test / wasm / windows / tooling
# targets — this is the release tree.
#
#   make            # optimized build: ./q and ./rayforce
#   make debug      # unoptimized + sanitizers (ASan/UBSan)
#   ./q             # REPL;  printf '2+3\n' | ./q  for piped mode
#   ./q -p 5000     # kdb-wire server

CC      ?= cc
STD      = c17
TARGET   = rayforce
Q_TARGET = q

# Version line — single source of truth is the VERSION file (override for
# one-off builds: make RAY_VERSION=X.Y.Z).
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
          -DRAY_VERSION_PATCH=$(VERSION_PATCH) -DRAYFORCE_VERSION=\"$(RAY_VERSION)\" \
          -DRAYFORCE_BUILD_DATE=\"$(BUILD_DATE)\"
INCLUDES = -Iinclude -Isrc -Ithird_party/yyjson -Ithird_party/picohttpparser -Ithird_party/miniz
DEPFLAGS = -MMD -MP

# Target microarchitecture. `native` = build for THIS machine (fastest; right for
# a local build). Override for a redistributable binary, e.g.
# `make RAY_MARCH=x86-64-v3` (AVX2 baseline) — a -march=native binary handed to an
# older CPU dies with SIGILL.
RAY_MARCH ?= native

RELEASE_CFLAGS = -fPIC $(WARNS) -std=$(STD) -O2 -march=$(RAY_MARCH) \
  -funroll-loops -fomit-frame-pointer -fno-math-errno \
  -fassociative-math -ffp-contract=fast -fno-signed-zeros -fno-trapping-math
DEBUG_CFLAGS   = -fPIC $(WARNS) -std=$(STD) -g -O0 -march=$(RAY_MARCH) -DDEBUG \
  -fsanitize=address,undefined -fno-omit-frame-pointer

UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Linux)
  LIBS = -lm -lpthread
else
  LIBS = -lm
endif
DEBUG_LDFLAGS = -fsanitize=address,undefined

CFLAGS  ?= $(RELEASE_CFLAGS)
LDFLAGS ?=

# Sources: every src/*/*.c, minus every binary's entry point (rayforce's main,
# the q REPL, and the qdoctest harness — the latter ships in the source tree but
# is not built here), plus the vendored third-party sources.
LIB_SRC  = $(filter-out src/app/main.c src/qlang/qmain.c src/qlang/qdoctest_main.c, $(wildcard src/*/*.c))
LIB_SRC += third_party/yyjson/yyjson.c
LIB_SRC += third_party/picohttpparser/picohttpparser.c
LIB_SRC += third_party/miniz/miniz.c
LIB_OBJ  = $(LIB_SRC:.c=.o)
MAIN_OBJ   = src/app/main.o
Q_MAIN_OBJ = src/qlang/qmain.o
DEPS = $(LIB_OBJ:.o=.d) $(MAIN_OBJ:.o=.d) $(Q_MAIN_OBJ:.o=.d)

.DEFAULT_GOAL := all
# peachq is the q product, so the default build is ./q only. The underlying
# rayfall engine REPL is still buildable on demand: `make rayforce`.
all: $(Q_TARGET)

# Build-time codegen. The .q sources and the web assets are baked into generated
# C headers that src/ #includes. These call the SAME generators the development
# tree uses (tools/gen-*.sh, shipped with the release tree) rather than
# reimplementing them here — an inline copy of one of them rotted silently as the
# others were added, and the public build broke on the header nobody had noticed.
src/qlang/dotq_gen.h: src/qlang/q.q src/qlang/dotq.q tools/gen-bootstrap.sh
	tools/gen-bootstrap.sh $@ src/qlang/q.q src/qlang/dotq.q

src/qlang/h_gen.h: src/qlang/h.q tools/gen-bootstrap.sh
	SYMBOL=OPENQ_H_BOOTSTRAP tools/gen-bootstrap.sh $@ src/qlang/h.q

src/qlang/j_gen.h: src/qlang/j.q tools/gen-bootstrap.sh
	SYMBOL=OPENQ_J_BOOTSTRAP tools/gen-bootstrap.sh $@ src/qlang/j.q

src/qlang/pq_gen.h: src/qlang/pq.q tools/gen-bootstrap.sh
	SYMBOL=OPENQ_PQ_BOOTSTRAP tools/gen-bootstrap.sh $@ src/qlang/pq.q

# FORCE prereq: the recipe runs every build so ADDING or DELETING an asset is
# reflected; gen-assets.sh writes atomically (temp + cmp + mv), so the header
# mtime only moves when the embedded bytes actually change.
.PHONY: force-html-assets
force-html-assets: ;
src/qlang/html_assets_gen.h: force-html-assets tools/gen-assets.sh
	@tools/gen-assets.sh $@ src/qlang/html

src/qlang/q_runtime.o: src/qlang/dotq_gen.h src/qlang/h_gen.h src/qlang/j_gen.h
src/qlang/q_pq.o:      src/qlang/pq_gen.h
src/qlang/q_http.o:    src/qlang/html_assets_gen.h

# Vendored third-party TUs: drop the fork's stricter warnings (they are not ours
# to fix, and -Wextra on them fails the build).
third_party/%.o: third_party/%.c
	$(CC) -c $(filter-out -Wextra,$(CFLAGS)) -Wno-error $(DEPFLAGS) $(DEFS) $(INCLUDES) -o $@ $<

# miniz LEAN, matching the development build: only tdefl/tinfl/mz_crc32 are used,
# so the ZIP archive and stdio APIs are compiled out.
MINIZ_DEFS = -DMINIZ_NO_ARCHIVE_APIS -DMINIZ_NO_ARCHIVE_WRITING_APIS -DMINIZ_NO_STDIO
third_party/miniz/miniz.o: third_party/miniz/miniz.c
	$(CC) -c $(filter-out -Wextra,$(CFLAGS)) -Wno-error $(MINIZ_DEFS) $(DEPFLAGS) $(DEFS) $(INCLUDES) -o $@ $<

%.o: %.c
	$(CC) -c $(CFLAGS) $(DEPFLAGS) $(DEFS) $(INCLUDES) -o $@ $<

$(TARGET): $(LIB_OBJ) $(MAIN_OBJ)
	$(CC) $(CFLAGS) -o $@ $(LIB_OBJ) $(MAIN_OBJ) $(LIBS) $(LDFLAGS)

$(Q_TARGET): $(LIB_OBJ) $(Q_MAIN_OBJ)
	$(CC) $(CFLAGS) -o $@ $(LIB_OBJ) $(Q_MAIN_OBJ) $(LIBS) $(LDFLAGS)

debug: CFLAGS  := $(DEBUG_CFLAGS)
debug: LDFLAGS := $(DEBUG_LDFLAGS)
debug: all

# --- Windows cross-build (mingw-w64) ------------------------------------------
# Cross-compile q.exe + rayforce.exe with mingw. Separate .win.o objects so the
# native build is untouched. RAY_OS_WINDOWS=1 on the command line (some files
# test it before the platform header is visible); _WIN32_WINNT=0x0A00 for the
# Win8.1+ memory APIs; __USE_MINGW_ANSI_STDIO=1 for C99 printf. No sanitizers,
# no -march (target CPU unknown).
#   make win        # -> q.exe + rayforce.exe   (needs mingw-w64)
WIN_CROSS  ?= x86_64-w64-mingw32-
WIN_CC      = $(WIN_CROSS)gcc
WIN_CFLAGS  = $(WARNS) -std=$(STD) -O2 \
  -DRAY_OS_WINDOWS=1 -D_WIN32_WINNT=0x0A00 -D__USE_MINGW_ANSI_STDIO=1
WIN_LIBS    = -lws2_32 -lm
# Exclude the frozen IOCP stub (src/core/iocp.c) from the WINDOWS link — the
# openq-owned real backend src/core/iocp_win.c provides ray_poll_* there, so
# linking both is a multiple-definition error. Mirrors the private Makefile.
WIN_LIB_OBJ    = $(filter-out src/core/iocp.win.o, $(LIB_SRC:.c=.win.o))
WIN_Q_MAIN_OBJ = $(Q_MAIN_OBJ:.o=.win.o)
WIN_DEPS = $(WIN_LIB_OBJ:.o=.d) $(WIN_Q_MAIN_OBJ:.o=.d)

# The Windows objects need the SAME generated headers as the native ones — listed
# per target because a .win.o is a different target from the .o and inherits none
# of its prerequisites (the Windows build died on html_assets_gen.h, 2026-07-21).
src/qlang/q_runtime.win.o: src/qlang/dotq_gen.h src/qlang/h_gen.h src/qlang/j_gen.h
src/qlang/q_pq.win.o:      src/qlang/pq_gen.h
src/qlang/q_http.win.o:    src/qlang/html_assets_gen.h

# Vendored third-party TUs: same relaxation as the native rule above.
third_party/%.win.o: third_party/%.c
	$(WIN_CC) -c $(filter-out -Wextra,$(WIN_CFLAGS)) -Wno-error $(DEPFLAGS) $(DEFS) $(INCLUDES) -o $@ $<

third_party/miniz/miniz.win.o: third_party/miniz/miniz.c
	$(WIN_CC) -c $(filter-out -Wextra,$(WIN_CFLAGS)) -Wno-error $(MINIZ_DEFS) $(DEPFLAGS) $(DEFS) $(INCLUDES) -o $@ $<

%.win.o: %.c
	$(WIN_CC) -c $(WIN_CFLAGS) $(DEPFLAGS) $(DEFS) $(INCLUDES) -o $@ $<

q.exe: $(WIN_LIB_OBJ) $(WIN_Q_MAIN_OBJ)
	$(WIN_CC) $(WIN_CFLAGS) -o $@ $(WIN_LIB_OBJ) $(WIN_Q_MAIN_OBJ) $(WIN_LIBS)

win: q.exe

clean:
	-rm -f $(LIB_OBJ) $(MAIN_OBJ) $(Q_MAIN_OBJ) $(DEPS)
	-rm -f $(WIN_LIB_OBJ) $(WIN_Q_MAIN_OBJ) $(WIN_DEPS)
	-rm -f $(TARGET) $(Q_TARGET) q.exe src/qlang/dotq_gen.h

# Print the build version (single source of truth for release tags).
version:
	@echo $(RAY_VERSION)

.PHONY: all debug clean version

-include $(DEPS)
