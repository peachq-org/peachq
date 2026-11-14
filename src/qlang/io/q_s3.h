/* q_s3 — the s3:// (s3a://, s3n://) transport of the byte core: an object's size, a ranged read of it and the
 * listing under a prefix, over our HTTP client and signed with SigV4 when credentials are configured (anonymous
 * otherwise).  `s3://bucket/key`; each config is a dict keyed by the AWS/KX environment variable names, scoped by path
 * prefix, held here once applied and never in a q value; the first access with the default ("s3://") unresolved applies
 * .s3.config[::;::] (qlib/s3.q).  A 403 is 'access and a missing object 'io, each naming the object; under -u every
 * operation is 'access. */
#ifndef QLANG_IO_Q_S3_H
#define QLANG_IO_Q_S3_H
#include <rayforce.h>
#include <stddef.h>
#include <stdint.h>

int q_s3_is(const char* p, size_t n);

/* The object's stored bytes as a long, from a HEAD. */
ray_t* q_s3_size(ray_t* pathstr);

/* `want` stored bytes from `off` (want < 0 = to the end), under q_io_clamp's law. */
ray_t* q_s3_read(ray_t* pathstr, int64_t off, int64_t want);

/* `key` of the prefix: the sorted names one level under it, () when nothing is there, NULL when it names an object. */
ray_t* q_s3_list(ray_t* pathstr);

void q_s3_register(void);                     /* `.pq.load_natives`s3`: .s3.i.config .s3.i.which .s3.i.configs */

void q_s3_mirror(void);                       /* every config, the default resolved, into DuckDB's main as it opens */

#endif /* QLANG_IO_Q_S3_H */
