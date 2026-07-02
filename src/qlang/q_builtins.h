#ifndef Q_BUILTINS_H
#define Q_BUILTINS_H
/* Register q's own builtins into the current rayforce env (call once, after
 * the runtime is created).  Starts with `parse`; grows into q's verb table. */
void q_builtins_register(void);
#endif /* Q_BUILTINS_H */
