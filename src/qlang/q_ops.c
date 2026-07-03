/* q op manifest table + static lexical classifier — see q_ops.h.
 *
 * ROSTER GROUND TRUTH — audited against ray_register_builtins (src/lang/eval.c)
 * and the divergence matrix (specs/2026-07-03-q-op-registry-complete-design.md,
 * corrected in adr/0003):
 *   - rayfall `/` is FLOAT-divide (7/2 -> 3.5) and `%` is MODULO, so q `%`
 *     (float divide) renames rayfall `/`.
 *   - q monadic `-` is negate (rayfall `neg`); `*` monadic is `first`; `#`
 *     monadic is `count`; `=` monadic is `group`; `<`/`>` monadic are grade
 *     up/down (`iasc`/`idesc`); `|` monadic is `reverse`; `&` monadic is
 *     `where`; `,` monadic is `enlist`; `~` monadic is `not`.
 *   - q `=`/`<>` dyadic wrap rayfall `==`/`!=` (char-string element-wise case);
 *     q `#`/`_` dyadic are arg-swap `take` / count-`drop` wrappers.
 *   - q `,` dyadic is join -> rayfall `concat`.
 * DEFERRED (QK_NONE — no clean rayfall target, do NOT guess): `&`/`|` dyadic
 *   (min/max are AGGREGATES, not dyadic element-wise lesser/greater), monadic
 *   `+` (flip: no builtin), monadic `%` (reciprocal: no builtin), dyadic `~`
 *   (match: no builtin).  Type-dispatch verbs `! ? $ @ . _` are entirely out of
 *   2a scope (2c).
 *
 * KW_INFIX is held to EXACTLY {div} so the lexer's infix-keyword classification
 * is byte-identical to the retired q_is_kw_verb memcmp — guarding the parse_*
 * ledgers (AST shape unchanged this stage). */
#define _POSIX_C_SOURCE 200809L
#include "qlang/q_ops.h"
#include <string.h>

static const q_op_t Q_OPS[] = {
    /* name    lex             mon_kind mon_target   dyad_kind dyad_target */
    /* ---- arithmetic / compare glyphs ---- */
    { "+",     QLEX_GLYPH,     QK_NONE, NULL,        QK_ENV,   "+"       },
    { "-",     QLEX_GLYPH,     QK_ENV,  "neg",       QK_ENV,   "-"       },
    { "*",     QLEX_GLYPH,     QK_ENV,  "first",     QK_ENV,   "*"       },
    { "%",     QLEX_GLYPH,     QK_NONE, NULL,        QK_ENV,   "/"       },
    { "<",     QLEX_GLYPH,     QK_ENV,  "iasc",      QK_ENV,   "<"       },
    { ">",     QLEX_GLYPH,     QK_ENV,  "idesc",     QK_ENV,   ">"       },
    { "<=",    QLEX_GLYPH,     QK_NONE, NULL,        QK_ENV,   "<="      },
    { ">=",    QLEX_GLYPH,     QK_NONE, NULL,        QK_ENV,   ">="      },
    { "=",     QLEX_GLYPH,     QK_ENV,  "group",     QK_EQ,    "=="      },
    { "<>",    QLEX_GLYPH,     QK_NONE, NULL,        QK_NE,    "!="      },
    /* ---- structural glyphs ---- */
    { "#",     QLEX_GLYPH,     QK_ENV,  "count",     QK_TAKE,  "take"    },
    { "_",     QLEX_GLYPH,     QK_NONE, NULL,        QK_DROP,  "drop"    },
    { "|",     QLEX_GLYPH,     QK_ENV,  "reverse",   QK_NONE,  NULL      },
    { "&",     QLEX_GLYPH,     QK_ENV,  "where",     QK_NONE,  NULL      },
    { ",",     QLEX_GLYPH,     QK_ENV,  "enlist",    QK_ENV,   "concat"  },
    { "~",     QLEX_GLYPH,     QK_ENV,  "not",       QK_NONE,  NULL      },
    /* ---- keyword-infix ---- */
    { "div",   QLEX_KW_INFIX,  QK_NONE, NULL,        QK_ENV,   "div"     },
    /* ---- keyword-prefix monads (pass-through/rename) ---- */
    { "neg",     QLEX_KW_PREFIX, QK_ENV, "neg",      QK_NONE,  NULL      },
    { "til",     QLEX_KW_PREFIX, QK_ENV, "til",      QK_NONE,  NULL      },
    { "count",   QLEX_KW_PREFIX, QK_ENV, "count",    QK_NONE,  NULL      },
    { "first",   QLEX_KW_PREFIX, QK_ENV, "first",    QK_NONE,  NULL      },
    { "last",    QLEX_KW_PREFIX, QK_ENV, "last",     QK_NONE,  NULL      },
    { "where",   QLEX_KW_PREFIX, QK_ENV, "where",    QK_NONE,  NULL      },
    { "reverse", QLEX_KW_PREFIX, QK_ENV, "reverse",  QK_NONE,  NULL      },
    { "sum",     QLEX_KW_PREFIX, QK_ENV, "sum",      QK_NONE,  NULL      },
    { "group",   QLEX_KW_PREFIX, QK_ENV, "group",    QK_NONE,  NULL      },
    /* ---- adverbs (lexer classification only; carrier values via q_deriv) ---- */
    { "'",     QLEX_ADVERB,    QK_NONE, NULL,        QK_NONE,  NULL      },
    { "/",     QLEX_ADVERB,    QK_NONE, NULL,        QK_NONE,  NULL      },
    { "\\",    QLEX_ADVERB,    QK_NONE, NULL,        QK_NONE,  NULL      },
    { "':",    QLEX_ADVERB,    QK_NONE, NULL,        QK_NONE,  NULL      },
    { "/:",    QLEX_ADVERB,    QK_NONE, NULL,        QK_NONE,  NULL      },
    { "\\:",   QLEX_ADVERB,    QK_NONE, NULL,        QK_NONE,  NULL      },
};
#define N_Q_OPS ((int)(sizeof Q_OPS / sizeof Q_OPS[0]))

const q_op_t* q_ops_table(int* n) {
    if (n) *n = N_Q_OPS;
    return Q_OPS;
}

int q_lex_is_kw_infix(const char* s, int len) {
    for (int i = 0; i < N_Q_OPS; i++) {
        if (Q_OPS[i].lex != QLEX_KW_INFIX) continue;
        const char* nm = Q_OPS[i].name;
        if ((int)strlen(nm) == len && memcmp(nm, s, (size_t)len) == 0)
            return 1;
    }
    return 0;
}
