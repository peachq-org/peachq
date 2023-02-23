#include "storm.h"
#include <stdlib.h>

#include "format.h"
#include <stdio.h>

// Global static destructor for generics g0
// static void free_g0(g0 *node)
// {
//     printf("Freeing g0 node: %lld\n", (*node)->i64_value);
// }

extern g0 new_scalar_i64(i64 value)
{
    // __attribute__((cleanup(free_g0)))
    g0 scalar = malloc(sizeof(g0));
    scalar->type = -TYPE_I64;
    scalar->i64_value = value;

    return scalar;
}

extern g0 new_vector_i64(i64 *ptr, i64 len)
{
    g0 vector = malloc(sizeof(g0));
    vector->type = TYPE_I64;
    vector->list_value.ptr = ptr;
    vector->list_value.len = len;

    return vector;
}