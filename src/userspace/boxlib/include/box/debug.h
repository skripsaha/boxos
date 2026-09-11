#ifndef BOX_DEBUG_H
#define BOX_DEBUG_H

#ifdef __cplusplus
extern "C" {
#endif

void kdbg(const char *msg);
void kdbg_nowait(const char *msg);
int  kdbg_print(const char *fmt, ...);

#ifdef __cplusplus
}
#endif

#endif