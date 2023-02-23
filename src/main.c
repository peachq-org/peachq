#include <stdio.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include "storm.h"
#include "format.h"

int main()
{
    // FILE *stream;
    // char *line = NULL;
    // size_t len = 0;
    // ssize_t nread;

    // while ((nread = getline(&line, &len, stream)) != -1)
    // {
    //     printf("Retrieved line of length %zu:\n", nread);
    //     fwrite(line, nread, 1, stdout);
    // }

    // free(line);
    // fclose(stream);

    str buffer;
    i64 vec[12] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
    g0 value = new_vector_i64(vec, 12);
    // g0 value = new_scalar_i64(9223372036854775807);
    Result res = g0_fmt(&buffer, value);
    if (res == Ok)
    {
        printf("%s\n", buffer);
    }

    result_fmt(&buffer, res);
    printf("Result: %s\n", buffer);

    return 0;
}
