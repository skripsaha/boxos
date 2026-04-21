#ifndef FB_GOP_H
#define FB_GOP_H

#include "ktypes.h"

int FbGopInit(uint64_t phys_addr, uint32_t width, uint32_t height,
              uint32_t stride, uint32_t format);

#endif /* FB_GOP_H */
