#ifndef BOX_IPC_H
#define BOX_IPC_H

#include "types.h"
#include "result.h"
#include "error.h"

#define BOX_SOURCE_KERNEL   0
#define BOX_SOURCE_ROUTE    1
#define BOX_SOURCE_HARDWARE 2

#define BOX_LISTEN_KEYBOARD 0
#define BOX_LISTEN_MOUSE    1
#define BOX_LISTEN_NETWORK  2

#define BOX_LISTEN_FLAG_EXCLUSIVE 0x01

#define BOX_IPC_FLAG_TEXT    0x00
#define BOX_IPC_FLAG_ERROR   0x01
#define BOX_IPC_FLAG_BINARY  0x02
#define BOX_IPC_FLAG_COMMAND 0x03

#define BOX_IPC_OP_ROUTE      0x40
#define BOX_IPC_OP_ROUTE_TAG  0x41
#define BOX_IPC_OP_LISTEN     0x42

int send(uint32_t target_pid, const void* data, uint16_t size);
int broadcast(const char* tag, const void* data, uint16_t size);
int listen(uint8_t source_type, uint8_t flags);
bool receive(box_result_entry_t* out_entry);
bool receive_wait(box_result_entry_t* out_entry, uint32_t timeout_ms);

#endif // BOX_IPC_H
