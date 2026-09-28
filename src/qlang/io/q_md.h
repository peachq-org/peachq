/* q_md — the `.md.i.*` natives `.md` calls: Markdown through the vendored md4c (third_party/md4c). */
#ifndef QLANG_IO_Q_MD_H
#define QLANG_IO_Q_MD_H

/* Bind `.md.i.parse` (text -> the block table) and `.md.i.html` (text -> XHTML string).  Both take the document
 * as TEXT; `.md` turns a resource or a list of lines into text first. */
void q_md_register(void);

#endif
