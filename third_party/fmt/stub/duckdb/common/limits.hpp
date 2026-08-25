// PEACHQ STUB — not upstream. See third_party/fmt/README.peachq.md.
// fmt asks only for the 128-bit maxima, which std::numeric_limits does not
// carry portably; the primary template is left undefined so any other
// instantiation is a compile error rather than a silent wrong bound.
#pragma once

#include "duckdb/common/hugeint.hpp"
#include "duckdb/common/uhugeint.hpp"

namespace duckdb {

template <class T> struct NumericLimits;

template <> struct NumericLimits<hugeint_t> {
    static constexpr hugeint_t Maximum() { return static_cast<hugeint_t>(~static_cast<uhugeint_t>(0) >> 1); }
};

template <> struct NumericLimits<uhugeint_t> {
    static constexpr uhugeint_t Maximum() { return ~static_cast<uhugeint_t>(0); }
};

}  // namespace duckdb
