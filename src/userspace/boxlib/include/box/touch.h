#ifndef BOX_TOUCH_H
#define BOX_TOUCH_H

#include "box/types.h"
#include "box/error.h"

/* Canonical Touch tag names. Convention is `key:value` (TagFS bucket
 * format) — subscribing to bare "key" catches all values; subscribing
 * to "key:value" catches one. Keep these in lock-step with the
 * kernel's TouchPublish() call sites or events vanish silently. */
#define TOUCH_TAG_KEYBOARD          "keyboard"
#define TOUCH_TAG_PROCESS_DIED      "process:died"
#define TOUCH_TAG_PROCESS_SPAWNED   "process:spawned"
#define TOUCH_TAG_SYSTEM_SHUTDOWN   "system:shutdown"
#define TOUCH_TAG_SYSTEM_REBOOT     "system:reboot"
#define TOUCH_TAG_USB_CONNECT       "usb:connect"
#define TOUCH_TAG_USB_DISCONNECT    "usb:disconnect"

typedef enum {
    TOUCH_REST      = 0,
    TOUCH_REACT     = 1,
    TOUCH_INTERRUPT = 2,
} TouchMode;

#define TOUCH_FLAG_KERNEL  0x0001u
#define TOUCH_FLAG_USER    0x0002u
#define TOUCH_FLAG_TAGFS   0x0004u

typedef struct __attribute__((packed)) {
    uint16_t tag_id;
    uint16_t flags;
    uint32_t source_pid;
    uint64_t payload_addr;
    uint32_t payload_len;
    uint64_t timestamp_tsc;
    uint32_t reserved;
} Touch;

/* Canonical payload shapes for kernel-published Touch events. Keep
 * these byte-for-byte identical with the kernel publishers — TouchPublish
 * passes raw bytes via ipc_copy_to_heap so any size/layout drift breaks
 * subscribers silently. */
typedef struct __attribute__((packed)) {
    uint32_t pid;
    int32_t  exit_code;
} TouchProcessDied;

typedef struct __attribute__((packed)) {
    uint32_t pid;
    uint32_t parent_pid;
} TouchProcessSpawned;

typedef struct __attribute__((packed)) {
    uint32_t reason;     /* opaque shutdown reason; 0 = clean halt */
    uint32_t grace_ms;   /* hint: ms left to flush before halt fires */
} TouchSystemHalt;

typedef struct __attribute__((packed)) {
    uint8_t  port;
    uint8_t  speed;      /* xHCI port speed code */
    uint16_t vendor_id;
    uint16_t product_id;
    uint16_t _reserved;
} TouchUsbConnect;

typedef enum {
    TOUCH_POLICY_EDGE    = 0,
    TOUCH_POLICY_LEVEL   = 1,
    TOUCH_POLICY_LATCHED = 2,
} TouchPolicy;

typedef enum {
    TOUCH_CAP_OPEN        = 0,
    TOUCH_CAP_OWNERS      = 1,
    TOUCH_CAP_KERNEL_ONLY = 2,
} TouchCapability;

int touch_claim(const char *tag, TouchMode mode, uint64_t manifest_or_handler,
                uint64_t stack_top);
int touch_release(const char *tag);
int touch_send(const char *tag, const void *payload, uint32_t plen, uint32_t after_ms);
int touch_await(const char *tag, Touch *out, uint32_t timeout_ms);
int touch_irq_return(void);
int touch_register(const char *tag, TouchPolicy policy, TouchCapability capability);
int touch_ack(const char *tag);

void touch_await_stats(uint32_t out[7]);

#endif /* BOX_TOUCH_H */
