#ifndef BOX_IPC_H
#define BOX_IPC_H

#include "box/types.h"
#include "box/error.h"
#include "box/core/result.h"

int send(uint32_t target_pid, const void* data, uint16_t size);
int broadcast(const char* tag, const void* data, uint16_t size);
int listen(uint64_t required_tags, uint8_t flags);
bool receive(Result* out);
bool receive_wait(Result* out, uint32_t timeout_ms);

int send_args(uint32_t target_pid, int argc, char** argv);
int receive_args(int* argc, char argv[][64], int max_args);

#endif // IPC_H
