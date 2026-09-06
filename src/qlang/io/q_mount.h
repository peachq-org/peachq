/* q_mount — the `\l` DIRECTORY forms (basics/syscmds.md#l-load-file-or-directory): a splayed dir maps
 * to a variable named after it (header-only, kb/splayed-tables.md), a db root binds serialized objects
 * and splays and runs q scripts, and the opened dir becomes the current one.  Partition-typed entries
 * (date/month/year/long names) are stage 4's. */
#ifndef PEACHQ_Q_MOUNT_H
#define PEACHQ_Q_MOUNT_H

#include <rayforce.h>

/* `path` is relative or absolute and must exist; scripts=0 is the `\l .` reload (data only).  Answers
 * the bound name sym for the splayed form (kx echoes it), NULL for a root mount, or an owned error. */
ray_t* q_mount_dir(const char* path, int scripts);

#endif /* PEACHQ_Q_MOUNT_H */
