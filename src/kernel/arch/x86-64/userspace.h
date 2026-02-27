#ifndef USERSPACE_H
#define USERSPACE_H

#include "ktypes.h"

void jump_to_userspace(uint64_t rip, uint64_t rsp, uint64_t rflags);

#endif
