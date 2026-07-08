#ifndef Q_BUILTINS_H
#define Q_BUILTINS_H
#include <rayforce.h>
/* Register q's own builtins into the current rayforce env (call once, after
 * the runtime is created).  Starts with `parse`; grows into q's verb table. */
void q_builtins_register(void);

/* q `string x` — cast-to-string (sym bare, atoms via the base formatter,
 * vectors/lists map element-wise).  Exposed as THE q cell formatter so
 * 0: Prepare Text renders cells through the same single home. */
ray_t* q_string_fn(ray_t* x);
#endif /* Q_BUILTINS_H */
