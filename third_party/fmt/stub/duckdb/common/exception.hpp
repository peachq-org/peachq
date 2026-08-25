// PEACHQ STUB — not upstream. See third_party/fmt/README.peachq.md.
// The two exception classes fmt throws. Text is never read: q_strfmt_shim.cc
// catches everything and answers with a status code, because peachq errors are
// bare classes.
#pragma once

#include <stdexcept>
#include <string>

namespace duckdb {

struct InvalidInputException : std::runtime_error {
    explicit InvalidInputException(const std::string& what) : std::runtime_error(what) {}
};

struct InternalException : std::runtime_error {
    explicit InternalException(const std::string& what) : std::runtime_error(what) {}
};

}  // namespace duckdb
