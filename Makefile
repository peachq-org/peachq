CC      ?= clang
STD     = c17
AR      = ar
TARGET  = rayforce
# Version: the git tag is the single source of truth. Precedence:
#   explicit override (CI passes RAY_VERSION=X.Y.Z from the release PR title)
#   > latest git tag (vX.Y.Z) > 0.0.0 dev default.
# The value is injected into the build via -D below (see DEFS); nothing is
# hand-edited in source to cut a release. See RELEASE.md.
RAY_VERSION ?= $(shell git describe --tags --match 'v[0-9]*.[0-9]*.[0-9]*' --abbrev=0 2>/dev/null | sed 's/^v//')
ifeq ($(strip $(RAY_VERSION)),)
  RAY_VERSION := 0.0.0
endif
VERSION       = $(RAY_VERSION)
VERSION_MAJOR := $(word 1,$(subst ., ,$(RAY_VERSION)))
VERSION_MINOR := $(word 2,$(subst ., ,$(RAY_VERSION)))
VERSION_PATCH := $(word 3,$(subst ., ,$(RAY_VERSION)))
GIT_HASH := $(shell git rev-parse --short HEAD 2>/dev/null || echo "unknown")
BUILD_DATE := $(shell date -u +%Y-%m-%d)

WARNS   = -Wall -Wextra -Werror -Wstrict-prototypes -Wno-unused-parameter
DEFS    = -DRAYFORCE_GIT_COMMIT=\"$(GIT_HASH)\" -DRAYFORCE_BUILD_DATE=\"$(BUILD_DATE)\" \
          -DRAY_VERSION_MAJOR=$(VERSION_MAJOR) -DRAY_VERSION_MINOR=$(VERSION_MINOR) \
          -DRAY_VERSION_PATCH=$(VERSION_PATCH) -DRAYFORCE_VERSION=\"$(RAY_VERSION)\"
INCLUDES = -Iinclude -Isrc
# Header-dependency tracking: -MMD emits a .d makefile fragment next to
# each .o listing the headers it included (user headers only, not system);
# -MP adds a phony target per header so deleting a header doesn't break the
# build with a "no rule to make" error.  The fragments are -included below.
DEPFLAGS = -MMD -MP

UNAME_S := $(shell uname -s)

# Target microarchitecture.  Default `native` = build for THIS machine (fastest;
# right for local builds and the per-machine release tarballs).  Override for
# REDISTRIBUTABLE packages (.deb) that must run on any CPU, e.g.
# `make release RAY_MARCH=x86-64-v3` (AVX2 baseline, ~2013+) — a -march=native
# binary handed to a different/older CPU dies with SIGILL.
RAY_MARCH ?= native

DEBUG_CFLAGS   = -fPIC $(WARNS) -std=$(STD) -g -O0 -march=$(RAY_MARCH) -DDEBUG \
  -fsanitize=address,undefined -fno-omit-frame-pointer
RELEASE_CFLAGS = -fPIC $(WARNS) -std=$(STD) -O3 -march=$(RAY_MARCH) \
  -funroll-loops -fomit-frame-pointer -fno-math-errno \
  -fassociative-math -ffp-contract=fast -fno-signed-zeros -fno-trapping-math
# -fassociative-math: license to reorder FP additions/multiplications.
#   Required for autovectorization of F64 reductions (sum/avg/dot).
#   Without it, scalar_sum_f64_fn at group.c:1666 is a serial latency
#   chain (~3-4 cycles/op) instead of 4-8 lanes/cycle SIMD.
# -ffp-contract=fast: emit FMA (fused multiply-add) where beneficial.
# -fno-signed-zeros: treat -0.0 == +0.0 (matches how distinct/hashset
#   normalises -0.0 → 0.0 in group.c:208).
# -fno-trapping-math: assume FP ops never trap; enables more reorder.
# NOT enabling -ffinite-math-only or -ffast-math: those assume no
#   NaN/Inf, which would break our null sentinels (NaN-encoded nulls
#   in F64 columns).

# Coverage: clang source-based instrumentation.  Sanitizers conflict
# with the profile runtime, so we drop them; -O0 keeps line numbers
# and avoids dead-code regions getting marked uncovered for the
# wrong reason.  See `make coverage` below.
COVERAGE_CFLAGS = -fPIC $(WARNS) -std=$(STD) -g -O0 -march=$(RAY_MARCH) -DDEBUG \
  -fno-omit-frame-pointer -fprofile-instr-generate -fcoverage-mapping
COVERAGE_LDFLAGS = -fprofile-instr-generate -fcoverage-mapping

ifeq ($(UNAME_S),Linux)
  LIBS            = -lm -lpthread
  RELEASE_LDFLAGS = -Wl,--gc-sections -Wl,--as-needed
else
  LIBS            = -lm
  RELEASE_LDFLAGS = -Wl,-dead_strip
endif

DEBUG_LDFLAGS   = -fsanitize=address,undefined

CFLAGS  = $(DEBUG_CFLAGS)
LDFLAGS = $(DEBUG_LDFLAGS)

# Sources
LIB_SRC  = $(wildcard src/*/*.c)
LIB_SRC := $(filter-out src/app/main.c, $(LIB_SRC))
LIB_OBJ  = $(LIB_SRC:.c=.o)
MAIN_SRC = src/app/main.c
MAIN_OBJ = $(MAIN_SRC:.c=.o)
TEST_SRC = $(wildcard test/*.c)
TEST_OBJ = $(TEST_SRC:.c=.o)

# Auto-generated header dependencies (one .d per .o, see DEPFLAGS).
# The fragments are -included at the very END of this file — including
# them here would let a .d's first rule (e.g. `foo.o: ...`) become the
# default goal, so bare `make` would build one object instead of `debug`.
DEPS = $(LIB_OBJ:.o=.d) $(MAIN_OBJ:.o=.d) $(TEST_OBJ:.o=.d)

# Default target (pinned so an -included .d fragment can't steal it).
.DEFAULT_GOAL := default
default: debug

%.o: %.c
	$(CC) -c $(CFLAGS) $(DEPFLAGS) $(DEFS) $(INCLUDES) -o $@ $<

# Main binary — shared by debug/release/test (test/rfl/system/ipc_diff.rfl
# spawns ./$(TARGET) as a server, so test depends on it too).
$(TARGET): $(LIB_OBJ) $(MAIN_OBJ)
	$(CC) $(CFLAGS) -o $(TARGET) $(LIB_OBJ) $(MAIN_OBJ) $(LIBS) $(LDFLAGS)

# Debug build
debug: CFLAGS = $(DEBUG_CFLAGS)
debug: LDFLAGS = $(DEBUG_LDFLAGS)
debug: $(TARGET)

# Release build
release: CFLAGS = $(RELEASE_CFLAGS)
release: LDFLAGS = $(RELEASE_LDFLAGS)
release: $(TARGET)

# Static library
lib: CFLAGS = $(RELEASE_CFLAGS)
lib: $(LIB_OBJ)
	$(AR) rc lib$(TARGET).a $(LIB_OBJ)

# Release tarball: build the optimized binary and package it as
# dist/rayforce-<version>-<os>-<arch>.tar.gz plus a SHA-256 checksum.
# Used by .github/workflows/release.yml (which passes RAY_VERSION=X.Y.Z) and
# runnable locally. VERSION comes from the tag/override resolved at the top.
dist: release
	@mkdir -p dist
	@os=$$(uname -s | tr 'A-Z' 'a-z'); arch=$$(uname -m); \
	 name=$(TARGET)-$(VERSION)-$$os-$$arch; stage=dist/$$name; \
	 mkdir -p $$stage; \
	 cp $(TARGET) LICENSE README.md include/rayforce.h $$stage/; \
	 tar -czf dist/$$name.tar.gz -C dist $$name; \
	 rm -rf $$stage; \
	 ( cd dist && { command -v sha256sum >/dev/null 2>&1 && sha256sum $$name.tar.gz || shasum -a 256 $$name.tar.gz; } > $$name.tar.gz.sha256 ); \
	 echo "built dist/$$name.tar.gz"

# Allocator micro-benchmark (release-optimized, linked against lib objects).
# Compile all sources fresh with RELEASE_CFLAGS so the benchmark measures
# the release allocator, not a sanitizer-instrumented debug build.
bench-alloc:
	$(CC) $(RELEASE_CFLAGS) $(DEFS) $(INCLUDES) -o bench-alloc \
		bench/alloc/main.c $(LIB_SRC) $(LIBS) $(RELEASE_LDFLAGS) -lpthread
	./bench-alloc

# Group predicate pushdown perf gate (release-optimized, no sanitizers).
# Measures FILTER(GROUP) with predicate pushed below GROUP vs unpushed.
bench-group-pushdown:
	$(CC) $(RELEASE_CFLAGS) $(DEFS) $(INCLUDES) -o bench-group-pushdown \
		bench/group_pushdown/main.c $(LIB_SRC) $(LIBS) $(RELEASE_LDFLAGS)
	./bench-group-pushdown

# Aggregation-engine A/B perf microbench (release-optimized, no sanitizers).
# H2O-style group-by shapes; v2 engine (this branch) vs rowforms (master).
bench-agg-v2:
	$(CC) $(RELEASE_CFLAGS) $(DEFS) $(INCLUDES) -o bench-agg-v2 \
		bench/agg_v2/main.c $(LIB_SRC) $(LIBS) $(RELEASE_LDFLAGS) -lm
	./bench-agg-v2

# Index routing per-point perf gate (release-optimized, no sanitizers).
# Measures indexed vs plain side for each of the 9 routing consumption points.
bench-idx-route:
	$(CC) $(RELEASE_CFLAGS) $(DEFS) $(INCLUDES) -o bench-idx-route \
		bench/idx_route/main.c $(LIB_SRC) $(LIBS) $(RELEASE_LDFLAGS)
	./bench-idx-route

# Join build-side selection perf gate.
# Measures swap (build hash on smaller left) vs legacy (build on right) for
# three cases: WIN (10K left vs 10M right), CONTROL (10M==10M, no swap),
# MANY-TO-MANY (100K left vs 10M right, ~10M output).  Sanitizer-free.
bench-join-buildside:
	$(CC) $(RELEASE_CFLAGS) $(DEFS) $(INCLUDES) -o bench-join-buildside \
		bench/join_buildside/main.c $(LIB_SRC) $(LIBS) $(RELEASE_LDFLAGS)
	./bench-join-buildside

# Join dup-fallback perf gate.
# Measures post-fix (auto dup-fallback to chained build) vs pre-fix (O(dup²)
# build via the ray_join_no_dup_fallback bypass knob) on catastrophic,
# zero-regression, and moderate-dup cases.  Sanitizer-free.
bench-join-dup:
	$(CC) $(RELEASE_CFLAGS) $(DEFS) $(INCLUDES) -o bench-join-dup \
		bench/join_dup/main.c $(LIB_SRC) $(LIBS) $(RELEASE_LDFLAGS)
	./bench-join-dup

# Worker threads per process during tests. Without this the runtime
# auto-sizes to ncpu-1, so on a many-core box the in-process harness AND
# every server it spawns via .sys.exec each create ~ncpu-1 threads — a lot of
# wasted CPU for tiny test inputs. RAYFORCE_CORES (honored by ray_pool_create)
# caps it; children inherit the env. Override for a fuller parallel stress,
# e.g. `make test TEST_CORES=0` (serial) or `make test TEST_CORES=8`.
TEST_CORES ?= 2

# Tests.  Depends on $(TARGET) because test/rfl/system/ipc_diff.rfl
# spawns ./$(TARGET) as an IPC server via .sys.exec — both binaries
# must exist on disk and share the build flavour (sanitizers, coverage).
test: CFLAGS = $(DEBUG_CFLAGS)
test: LDFLAGS = $(DEBUG_LDFLAGS)
test: $(TARGET) $(LIB_OBJ) $(TEST_OBJ)
	$(CC) $(CFLAGS) -o $(TARGET).test $(LIB_OBJ) $(TEST_OBJ) $(LIBS) $(LDFLAGS) -Itest
	RAYFORCE_CORES=$(TEST_CORES) ./$(TARGET).test

# Coverage report.  Builds both binaries with clang source-based
# instrumentation, runs the test suite (writing one .profraw per
# process — the test binary AND every IPC server it spawns —
# thanks to LLVM_PROFILE_FILE='%p' giving each pid a unique file),
# merges, and emits an HTML report under coverage_html/.
#
# Requires clang + llvm-profdata + llvm-cov.  Sanitizers are dropped
# for this build (incompatible with the profile runtime).
coverage:
	@command -v clang         >/dev/null || { echo "coverage: clang not found";         exit 1; }
	@command -v llvm-profdata >/dev/null || { echo "coverage: llvm-profdata not found"; exit 1; }
	@command -v llvm-cov      >/dev/null || { echo "coverage: llvm-cov not found";      exit 1; }
	$(MAKE) clean
	rm -f cov-*.profraw default.profraw coverage.profdata
	rm -rf coverage_html
	LLVM_PROFILE_FILE='cov-%p.profraw' $(MAKE) test \
		CC=clang \
		DEBUG_CFLAGS='$(COVERAGE_CFLAGS)' \
		DEBUG_LDFLAGS='$(COVERAGE_LDFLAGS)'
	llvm-profdata merge -sparse cov-*.profraw -o coverage.profdata
	llvm-cov show ./$(TARGET).test \
		-instr-profile=coverage.profdata \
		-format=html -output-dir=coverage_html \
		-show-line-counts-or-regions \
		-ignore-filename-regex='test/.*|/usr/.*|.*_alloc_stub\.c|include/rayforce\.h'
	@echo
	@echo "=== coverage summary ==="
	@llvm-cov report ./$(TARGET).test \
		-instr-profile=coverage.profdata \
		-ignore-filename-regex='test/.*|/usr/.*|.*_alloc_stub\.c|include/rayforce\.h' 2>/dev/null | tail -3
	@echo
	@echo "→ coverage_html/index.html"

clean:
	-rm -f $(LIB_OBJ) $(MAIN_OBJ) $(TEST_OBJ)
	-rm -f $(DEPS)
	-rm -f $(TARGET) $(TARGET).test lib$(TARGET).a
	-rm -rf build build_release dist
	# Test-generated fixtures (see test/rfl/system/*.rfl) — should not linger after a run.
	-rm -f rf_test_*.csv
	# Coverage artefacts (see `make coverage`).
	-rm -f cov-*.profraw default.profraw coverage.profdata
	-rm -rf coverage_html

.PHONY: default debug release lib dist bench-alloc bench-join-buildside bench-join-dup test coverage clean

# Header dependencies last: .d fragments only add prerequisites to the
# object targets above, and being last they can't hijack the default goal.
# -include silently skips any that don't exist yet (first build).
-include $(DEPS)
