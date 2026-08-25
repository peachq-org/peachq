// q_strfmt_shim — the second (and last) C++ translation unit peachq owns, and
// the one place fmt is visible.  It knows nothing about q: it turns an array of
// four-lane PODs into fmt arguments, calls the vendored formatter, and hands
// back a malloc'd buffer plus a status.
//
// fmt reports every grammar and argument fault by throwing, and peachq errors
// are bare classes carrying no text — so the catch-all here is the design, not
// a shortcut: the message is deliberately discarded and the class chosen by the
// caller, which knows which grammar it asked for.

#include "qlang/io/q_strfmt_abi.h"
#include "fmt/format.h"
#include "fmt/printf.h"

#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <vector>

namespace {

using pf_ctx = duckdb_fmt::basic_printf_context_t<char>;
using fm_ctx = duckdb_fmt::format_context;

// The returned arguments own their scalars but only POINT at byte-span text, so
// the caller's bytes — and nothing else here — must outlive the format call.
template <typename Ctx>
std::vector<duckdb_fmt::basic_format_arg<Ctx>> lanes(const pqfmt_arg* a, int n) {
    std::vector<duckdb_fmt::basic_format_arg<Ctx>> v;
    v.reserve(static_cast<size_t>(n));
    for (int k = 0; k < n; k++) {
        switch (a[k].lane) {
        case PQFMT_LANE_BOOL: {
            const bool b = a[k].v.i != 0;
            v.push_back(duckdb_fmt::internal::make_arg<Ctx>(b));
            break;
        }
        case PQFMT_LANE_I64: v.push_back(duckdb_fmt::internal::make_arg<Ctx>(a[k].v.i)); break;
        case PQFMT_LANE_F64: v.push_back(duckdb_fmt::internal::make_arg<Ctx>(a[k].v.f)); break;
        default: {
            const duckdb_fmt::string_view s(a[k].v.s.p, static_cast<size_t>(a[k].v.s.n));
            v.push_back(duckdb_fmt::internal::make_arg<Ctx>(s));
            break;
        }
        }
    }
    return v;
}

int own(const std::string& s, char** out, int64_t* outn) {
    char* buf = static_cast<char*>(malloc(s.size() + 1));
    if (!buf) return PQFMT_NOMEM;
    memcpy(buf, s.data(), s.size());
    buf[s.size()] = '\0';
    *out  = buf;
    *outn = static_cast<int64_t>(s.size());
    return PQFMT_OK;
}

}  // namespace

int pqfmt_printf(const char* fmt, int64_t fmtn, const pqfmt_arg* args, int argn, char** out, int64_t* outn) {
    try {
        auto v = lanes<pf_ctx>(args, argn);
        return own(duckdb_fmt::vsprintf(duckdb_fmt::string_view(fmt, static_cast<size_t>(fmtn)),
                                        duckdb_fmt::basic_format_args<pf_ctx>(v.data(), static_cast<int>(v.size()))),
                   out, outn);
    } catch (const std::bad_alloc&) {
        return PQFMT_NOMEM;
    } catch (...) {
        return PQFMT_BAD;
    }
}

int pqfmt_format(const char* fmt, int64_t fmtn, const pqfmt_arg* args, int argn, char** out, int64_t* outn) {
    try {
        auto v = lanes<fm_ctx>(args, argn);
        return own(duckdb_fmt::vformat(duckdb_fmt::string_view(fmt, static_cast<size_t>(fmtn)),
                                       duckdb_fmt::basic_format_args<fm_ctx>(v.data(), static_cast<int>(v.size()))),
                   out, outn);
    } catch (const std::bad_alloc&) {
        return PQFMT_NOMEM;
    } catch (...) {
        return PQFMT_BAD;
    }
}

void pqfmt_freestr(char* p) { free(p); }
