#ifndef IPC_H
#define IPC_H

#include "types.h"
#include "result.h"
#include "error.h"

#define SOURCE_KERNEL   0
#define SOURCE_ROUTE    1
#define SOURCE_HARDWARE 2

#define LISTEN_KEYBOARD  (1ULL << 0)
#define LISTEN_MOUSE     (1ULL << 1)
#define LISTEN_NETWORK   (1ULL << 2)
#define LISTEN_STORAGE   (1ULL << 3)
#define LISTEN_TIMER     (1ULL << 4)

#define LISTEN_FLAG_EXCLUSIVE 0x01

#define IPC_FLAG_TEXT    0x00
#define IPC_FLAG_ERROR   0x01
#define IPC_FLAG_BINARY  0x02
#define IPC_FLAG_COMMAND 0x03

#define IPC_OP_ROUTE      0x40
#define IPC_OP_ROUTE_TAG  0x41
#define IPC_OP_LISTEN     0x42

int send(uint32_t target_pid, const void* data, uint16_t size);
int broadcast(const char* tag, const void* data, uint16_t size);
int listen(uint64_t required_tags, uint8_t flags);
bool receive(Result* out);
bool receive_wait(Result* out, uint32_t timeout_ms);

int send_args(uint32_t target_pid, int argc, char** argv);
int receive_args(int* argc, char argv[][64], int max_args);

#endif // IPC_H
