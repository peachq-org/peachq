// PEACHQ STUB — not upstream. See third_party/fmt/README.peachq.md.
// DuckDB swaps std::unique_ptr for its own checked one here. We want the
// standard one, which format.h itself falls back to when DUCKDB_BASE_STD is
// undefined — so this header only has to supply the declaration it aliases.
#pragma once

#include <memory>
