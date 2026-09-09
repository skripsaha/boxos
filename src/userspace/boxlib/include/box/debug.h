#ifndef BOX_DEBUG_H
#define BOX_DEBUG_H

#ifdef __cplusplus
extern "C" {
#endif

void kdbg(const char *msg);
/* The same line, said without waiting for the answer — for a strand that
 * cannot reach its answers (see box/core/stash.h). One per process. */
void kdbg_nowait(const char *msg);
int  kdbg_print(const char *fmt, ...);

#ifdef __cplusplus
}
#endif

#endif /* BOX_DEBUG_H */
