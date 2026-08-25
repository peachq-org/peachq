// PEACHQ STUB — not upstream. See third_party/fmt/README.peachq.md.
// DuckDB's hugeint_t is a struct pair; fmt only needs a 128-bit integer, and q
// never constructs one, so the compiler's own type is enough.
#pragma once

namespace duckdb {
using hugeint_t = __int128;
}
