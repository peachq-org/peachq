// q_re2_shim — the ONLY C++ translation unit peachq owns.  RE2 is linked into
// the executable, so this is where C++ STOPS: everything RE2-shaped is confined
// here and reached through the flat ABI in q_re2_abi.h.  No q types, no ray_t,
// no allocation policy beyond the buffers `replace` and `escape` hand back.
//
// RE2::Options is left at its defaults on purpose.  q has no options argument:
// case-insensitivity and the newline flags ride inside the pattern as (?i)/(?s)/
// (?m), which RE2 reads itself, and the two Options with no inline form become
// ordinary functions instead — QuoteMeta for `literal`, replace's global flag
// for DuckDB's `g`.

#include "qlang/io/q_re2_abi.h"
#include "re2/re2.h"

#include <cstdlib>
#include <cstring>
#include <string>

using duckdb_re2::RE2;
using duckdb_re2::StringPiece;

// The one owned-buffer convention: shim-allocated, shim-freed, NUL-padded so a
// C caller may also read it as a string.
static int pq_own(const std::string& s, char** out, int64_t* outn) {
    char* buf = (char*)malloc(s.size() + 1);
    if (!buf) return -1;
    memcpy(buf, s.data(), s.size());
    buf[s.size()] = '\0';
    *out  = buf;
    *outn = (int64_t)s.size();
    return 0;
}

void* pqre2_compile(const char* pat, int64_t patn, int* err) {
    RE2::Options o;
    o.set_log_errors(false);   // a bad pattern is a q error, never stderr noise
    *err    = PQRE2_OK;
    RE2* re = new RE2(StringPiece(pat, (size_t)patn), o);
    if (!re->ok()) { delete re; *err = PQRE2_BAD_PATTERN; return nullptr; }
    return re;
}

void pqre2_release(void* prog) { delete (RE2*)prog; }

int pqre2_ngroups(void* prog) { return ((RE2*)prog)->NumberOfCapturingGroups(); }

int pqre2_match(void* prog, const char* s, int64_t sn, int64_t start, int anchor,
                int64_t* out, int maxg) {
    RE2* re = (RE2*)prog;
    if (start < 0 || start > sn) return 0;
    int ng    = maxg < 1 ? 1 : maxg;
    int avail = 1 + re->NumberOfCapturingGroups();
    if (ng > avail) ng = avail;
    StringPiece* m = new StringPiece[(size_t)ng];
    bool ok = re->Match(StringPiece(s, (size_t)sn), (size_t)start, (size_t)sn,
                        anchor ? RE2::ANCHOR_BOTH : RE2::UNANCHORED, m, ng);
    if (ok && maxg > 0)
        for (int i = 0; i < ng; i++) {
            bool got  = m[i].data() != nullptr;
            out[2 * i]     = got ? (int64_t)(m[i].data() - s) : -1;
            out[2 * i + 1] = got ? (int64_t)m[i].size() : 0;
        }
    delete[] m;
    return ok ? ng : 0;
}

int pqre2_replace(void* prog, const char* s, int64_t sn, const char* r, int64_t rn,
                  int global, char** out, int64_t* outn) {
    RE2*        re = (RE2*)prog;
    std::string rewrite(r, (size_t)rn), why;
    if (!re->CheckRewriteString(rewrite, &why)) return -1;
    std::string subj(s, (size_t)sn);
    if (global) RE2::GlobalReplace(&subj, *re, rewrite);
    else        RE2::Replace(&subj, *re, rewrite);
    return pq_own(subj, out, outn);
}

int pqre2_escape(const char* s, int64_t sn, char** out, int64_t* outn) {
    return pq_own(RE2::QuoteMeta(StringPiece(s, (size_t)sn)), out, outn);
}

void pqre2_freestr(char* p) { free(p); }
