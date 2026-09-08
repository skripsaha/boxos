#ifndef BOX_IPC_H
#define BOX_IPC_H

#ifdef __cplusplus
extern "C" {
#endif

#include "box/types.h"
#include "box/error.h"
#include "box/core/result.h"

int send(uint32_t target_pid, const void* data, uint16_t size);
int broadcast(const char* tag, const void* data, uint16_t size);
int listen(uint64_t required_tags, uint8_t flags);
bool receive(Result* out);
bool receive_wait(Result* out, uint32_t timeout_ms);
/* A program's arguments are not a message: they are its Luggage, in its cabin
 * from the first instruction — see box/luggage.h. */


#ifdef __cplusplus
}
#endif

#endif // IPC_H
