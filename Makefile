# peachq — build ./q from source.
#   make            # optimized
#   ./q -p 5000     # kdb-wire server

CC      ?= cc

RAY_ENTRY_SRC = src/app/main.c src/qlang/qmain.c src/qlang/qdoctest_main.c
RAY_LIB_SRC   = $(filter-out $(RAY_ENTRY_SRC), $(wildcard src/*/*.c src/qlang/eval/*.c src/qlang/ops/*.c src/qlang/net/*.c src/qlang/io/*.c \
                                            src/qlang/parse/*.c src/qlang/base/*.c))

RAY_VENDOR_SRC = third_party/yyjson/yyjson.c \
                 third_party/picohttpparser/picohttpparser.c \
                 third_party/miniz/miniz.c

# q_gz.c hand-rolls RFC 1952 framing: miniz has no gzip windowBits+16 mode.
RAY_MINIZ_DEFS = -DMINIZ_NO_ARCHIVE_APIS -DMINIZ_NO_ARCHIVE_WRITING_APIS -DMINIZ_NO_STDIO

RAY_INCLUDES = -Iinclude -Isrc \
               -Ithird_party/yyjson -Ithird_party/picohttpparser -Ithird_party/miniz

RAY_GEN_HDRS = src/qlang/dotq_gen.h src/qlang/h_gen.h src/qlang/j_gen.h \
               src/qlang/pq_gen.h src/qlang/html_assets_gen.h

# q.q before dotq.q — dotq.q may use q.q keywords, never the reverse.
src/qlang/dotq_gen.h: src/qlang/q.q src/qlang/dotq.q tools/gen-bootstrap.sh
	tools/gen-bootstrap.sh $@ src/qlang/q.q src/qlang/dotq.q

src/qlang/h_gen.h: src/qlang/h.q tools/gen-bootstrap.sh
	SYMBOL=OPENQ_H_BOOTSTRAP tools/gen-bootstrap.sh $@ src/qlang/h.q

src/qlang/j_gen.h: src/qlang/j.q tools/gen-bootstrap.sh
	SYMBOL=OPENQ_J_BOOTSTRAP tools/gen-bootstrap.sh $@ src/qlang/j.q

src/qlang/pq_gen.h: src/qlang/pq.q tools/gen-bootstrap.sh
	SYMBOL=OPENQ_PQ_BOOTSTRAP tools/gen-bootstrap.sh $@ src/qlang/pq.q

# Dirs in the prerequisite list: deleting an asset bumps its directory's mtime,
# which a file-only list cannot see. A no-change make must not touch this rule.
HTML_ASSET_DEPS := $(shell find src/qlang/html -type f -o -type d 2>/dev/null)
src/qlang/html_assets_gen.h: tools/gen-assets.sh $(HTML_ASSET_DEPS)
	@tools/gen-assets.sh $@ src/qlang/html

BUILD_DIR = build

# Both suffixes: a .win.o inherits none of the .o target's prerequisites, which is
# how the mingw build broke while the native one was already fixed.
$(BUILD_DIR)/src/qlang/q_runtime.o $(BUILD_DIR)/src/qlang/q_runtime.win.o: \
    src/qlang/dotq_gen.h src/qlang/h_gen.h src/qlang/j_gen.h
$(BUILD_DIR)/src/qlang/q_pq.o   $(BUILD_DIR)/src/qlang/q_pq.win.o:   src/qlang/pq_gen.h
$(BUILD_DIR)/src/qlang/net/q_http.o $(BUILD_DIR)/src/qlang/net/q_http.win.o: src/qlang/html_assets_gen.h

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
DATE_STEMS = $(addprefix $(BUILD_DIR)/,src/app/repl src/ops/system src/qlang/q_dotz src/qlang/qmain)
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
ifeq ($(UNAME_S),Linux)
  LIBS = -lm -lpthread
else
  LIBS = -lm
endif

CFLAGS  ?= $(RELEASE_CFLAGS)
LDFLAGS ?=

LIB_SRC = $(RAY_LIB_SRC) $(RAY_VENDOR_SRC)
LIB_OBJ    = $(addprefix $(BUILD_DIR)/,$(LIB_SRC:.c=.o))
Q_MAIN_OBJ = $(BUILD_DIR)/src/qlang/qmain.o
DEPS = $(LIB_OBJ:.o=.d) $(Q_MAIN_OBJ:.o=.d)

.DEFAULT_GOAL := all
all: git-merge-drivers $(Q_TARGET)

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

# Header discipline, qlang-scoped: frozen base src/ must not need edits to pass.
$(BUILD_DIR)/src/qlang/%.o: CFLAGS += -Wmissing-prototypes
$(BUILD_DIR)/src/qlang/%.win.o: WIN_CFLAGS += -Wmissing-prototypes

$(BUILD_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) -c $(CFLAGS) $(DEPFLAGS) $(DEFS) $(INCLUDES) -o $@ $<

$(Q_TARGET): $(LIB_OBJ) $(Q_MAIN_OBJ)
	$(CC) $(CFLAGS) -o $@ $(LIB_OBJ) $(Q_MAIN_OBJ) $(LIBS) $(LDFLAGS)

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
WIN_LIB_OBJ    = $(filter-out $(BUILD_DIR)/src/core/iocp.win.o, $(addprefix $(BUILD_DIR)/,$(LIB_SRC:.c=.win.o)))
WIN_Q_MAIN_OBJ = $(Q_MAIN_OBJ:.o=.win.o)
WIN_DEPS = $(WIN_LIB_OBJ:.o=.d) $(WIN_Q_MAIN_OBJ:.o=.d)

$(BUILD_DIR)/third_party/%.win.o: third_party/%.c
	@mkdir -p $(dir $@)
	$(WIN_CC) -c $(filter-out -Wextra,$(WIN_CFLAGS)) -Wno-error $(DEPFLAGS) $(DEFS) $(INCLUDES) -o $@ $<

$(BUILD_DIR)/third_party/miniz/miniz.win.o: third_party/miniz/miniz.c
	@mkdir -p $(dir $@)
	$(WIN_CC) -c $(filter-out -Wextra,$(WIN_CFLAGS)) -Wno-error $(RAY_MINIZ_DEFS) $(DEPFLAGS) $(DEFS) $(INCLUDES) -o $@ $<

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
	-rm -f $(Q_TARGET) q.exe $(RAY_GEN_HDRS)

version:
	@echo $(RAY_VERSION)

.PHONY: all clean version

# The development tree adds tests, gates, benches and the release tooling. It is
# NOT shipped, so -include silently skips it in the public tree — which is how the
# public build stays the build everyone actually runs.
-include Makefile.dev

-include $(DEPS)
