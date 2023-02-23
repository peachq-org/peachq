#ifndef STORM_H
#define STORM_H

#ifdef __cplusplus
extern "C"
{
#endif

// A compile time assertion check
#define CASSERT(predicate, file) _impl_CASSERT_LINE(predicate, __LINE__, file)
#define _impl_PASTE(a, b) a##b
#define _impl_CASSERT_LINE(predicate, line, file) \
    typedef char _impl_PASTE(assertion_failed_##file##_, line)[2 * !!(predicate)-1];

// Type constants
#define TYPE_S0 0
#define TYPE_I8 1
#define TYPE_I64 2
#define TYPE_F64 3
#define TYPE_ERR 127

    typedef enum
    {
        Ok = 0,
        FormatError,
    } Result;

    typedef char i8;
    typedef char *str;
    typedef long long i64;
    typedef double f64;

    // Generic type
    typedef struct s0
    {
        i8 type;

        union
        {
            i8 i8_value;
            i64 i64_value;
            f64 f64_value;
            struct s0 *g0_value;
            struct
            {
                i64 len;
                void *ptr;
            } list_value;
        };
    } __attribute__((aligned(32))) * g0;

    CASSERT(sizeof(struct s0) == 32, storm_h)

    // Constructors
    extern g0 new_scalar_i64(i64 value);
    extern g0 new_vector_i64(i64 *ptr, i64 len);

    // Accessors

#ifdef __cplusplus
}
#endif

#endif
