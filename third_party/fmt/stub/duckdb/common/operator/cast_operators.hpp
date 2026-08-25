// PEACHQ STUB — not upstream. See third_party/fmt/README.peachq.md.
// DuckDB's Cast::Operation is range-checked. fmt reaches it on ONE path — the
// widening of an integer argument to 128 bits — which cannot overflow, so a
// plain conversion is the whole of it.
#pragma once

namespace duckdb {

struct Cast {
    template <class From, class To> static To Operation(From x) { return static_cast<To>(x); }
};

}  // namespace duckdb
