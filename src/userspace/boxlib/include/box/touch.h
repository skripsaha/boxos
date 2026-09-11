#ifndef BOX_TOUCH_H
#define BOX_TOUCH_H

#ifdef __cplusplus
extern "C" {
#endif

#include "box/types.h"
#include "box/error.h"


typedef uint16_t TouchTag;
#define TOUCH_TAG_INVALID  ((TouchTag)0xFFFF)

#define TOUCH_TAG_KEYBOARD          "keyboard"
#define TOUCH_TAG_PROCESS_DIED      "process:died"
#define TOUCH_TAG_PROCESS_SPAWNED   "process:spawned"
#define TOUCH_TAG_SYSTEM_SHUTDOWN   "system:shutdown"
#define TOUCH_TAG_SYSTEM_REBOOT     "system:reboot"
#define TOUCH_TAG_USB_CONNECT       "usb:connect"
#define TOUCH_TAG_USB_DISCONNECT    "usb:disconnect"
#define TOUCH_TAG_USB_ARRIVED       "usb:arrived"
#define TOUCH_TAG_USB_LEFT          "usb:left"

typedef struct {
    uint8_t  port;
    uint8_t  slot_id;
    uint8_t  speed;
    uint8_t  dev_class;
    uint8_t  dev_subclass;
    uint8_t  dev_protocol;
    uint16_t vendor_id;
    uint16_t product_id;
    uint16_t usb_version;
    uint32_t reserved;
} __attribute__((packed)) TouchUsbDevice;

typedef struct {
    TouchTag full;
    TouchTag bare;
} TouchTagPair;

typedef enum {
    TOUCH_REST      = 0,
    TOUCH_REACT     = 1,
    TOUCH_INTERRUPT = 2,
} TouchMode;

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

#define TOUCH_FLAG_KERNEL  0x0001u
#define TOUCH_FLAG_USER    0x0002u
#define TOUCH_FLAG_TAGFS   0x0004u

#define BOXOS_TOUCH_PAYLOAD_MAX  96u

typedef struct __attribute__((packed)) {
    uint16_t tag_id;
    uint16_t flags;
    uint32_t source_pid;
    uint32_t payload_len;
    uint32_t _reserved;
    uint64_t timestamp_tsc;
    uint8_t  payload[BOXOS_TOUCH_PAYLOAD_MAX];
} Touch;

STATIC_ASSERT(sizeof(Touch) == 120, "Touch must be 120 bytes (24 hdr + 96 payload)");

typedef struct __attribute__((packed)) {
    uint32_t pid;
    int32_t  exit_code;
    uint32_t generation;
} TouchProcessDied;

STATIC_ASSERT(sizeof(TouchProcessDied) == 12,
              "TouchProcessDied must be 12 bytes (pid@0, exit_code@4, generation@8) "
              "— pid/exit_code offsets are frozen for all existing readers");

typedef struct __attribute__((packed)) {
    uint32_t pid;
    uint32_t parent_pid;
} TouchProcessSpawned;

typedef struct __attribute__((packed)) {
    uint32_t reason;
    uint32_t grace_ms;
} TouchSystemHalt;

typedef struct __attribute__((packed)) {
    uint8_t  port;
    uint8_t  speed;
    uint16_t vendor_id;
    uint16_t product_id;
    uint16_t _reserved;
} TouchUsbConnect;

TouchTagPair touch_intern(const char *tag);

static inline TouchTag touch_pair_choose(TouchTagPair p) {
    return (p.full != TOUCH_TAG_INVALID) ? p.full : p.bare;
}

int touch_claim(TouchTag tag, TouchMode mode, uint64_t manifest_or_handler,
                uint64_t stack_top);
int touch_release(TouchTag tag);

int touch_send(TouchTagPair pair, const void *payload, uint32_t plen,
               uint32_t after_ms);

int touch_await(TouchTag tag, Touch *out, uint32_t timeout_ms);
int touch_irq_return(void);
int touch_register(TouchTag tag, TouchPolicy policy, TouchCapability capability);
int touch_ack(TouchTag tag);

bool touch_pop(Touch *out);

bool touch_wait(Touch *out, uint32_t timeout_ms);

bool touch_available(void);

bool touch_try_pop_tag(TouchTag tag, Touch *out);
bool touch_wait_tag(TouchTag tag, Touch *out, uint32_t timeout_ms);

void touch_stash_free_self(void);

void touch_pop_stats(uint64_t out[8]);

uint64_t touch_owed(void);

void touch_await_stats(uint32_t out[7]);

#define TOUCH_TAG_PAIR(str) \
    ({ static TouchTagPair _tp = { TOUCH_TAG_INVALID, TOUCH_TAG_INVALID }; \
       if (_tp.full == TOUCH_TAG_INVALID && _tp.bare == TOUCH_TAG_INVALID) { \
           _tp = touch_intern(str); \
       } _tp; })

#define TOUCH_TAG_ID(str)  (touch_pair_choose(TOUCH_TAG_PAIR(str)))

#ifdef __cplusplus
}
#endif

#endif