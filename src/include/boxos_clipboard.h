#ifndef BOXOS_CLIPBOARD_H
#define BOXOS_CLIPBOARD_H

/*
 * boxos_clipboard.h — the Clipboard, shared kernel/userspace.
 *
 * The machine's one list of what was copied, kept by the kernel
 * (core/clipboard). A clip is bytes the list never reads, under a number no
 * other clip is ever given. The list keeps every clip until it is dropped,
 * the list is cleared, or the clip's own terms end it, and it is empty when
 * the machine starts. The ops and their crates are SYSTEM_OP_CLIPBOARD_* in
 * boxos_decks.h.
 *
 * Kernel:    #include "ktypes.h" first.
 * Userspace: #include "box/types.h" first.
 */

/* A term a clip may be put under, in its flags: it goes when the program that
 * put it ends. The other term, a count of whole takes, rides beside it. */
#define CLIPBOARD_WHILE_ALIVE  0x01u

/* One clip, as the list tells of it. */
typedef struct ClipboardRecord {
    uint64_t number;      /* never given to another clip                    */
    uint64_t bytes;       /* how long it is; never 0                        */
    uint64_t put_us;      /* when it was put, in microseconds since boot    */
    uint32_t takes_left;  /* whole takes before it goes; 0: no such term    */
    uint32_t flags;       /* CLIPBOARD_WHILE_ALIVE                          */
} ClipboardRecord;

/* What a list answer begins with; its records follow, newest first. */
typedef struct ClipboardPage {
    uint32_t delivered;   /* records after this head                        */
    uint32_t reserved;
    uint64_t next;        /* the number to ask from for older clips; 0: none */
    uint64_t clips;       /* how many clips the list holds now              */
    uint64_t bytes;       /* how many bytes they hold between them          */
} ClipboardPage;

/*
 * The Touch tag the kernel says the list's changes on. Payload: one
 * ClipboardChange.
 *
 *   PUT     — a clip was put; `number` is its number.
 *   GONE    — a clip left the list: dropped, its last take taken, or the
 *             program it was put to live with ended.
 *   CLEARED — every clip left at once; `number` is 0.
 */
#define CLIPBOARD_TOUCH_TAG  "clipboard:changed"
#define CLIPBOARD_PUT        1u
#define CLIPBOARD_GONE       2u
#define CLIPBOARD_CLEARED    3u

typedef struct ClipboardChange {
    uint64_t number;
    uint32_t what;        /* CLIPBOARD_PUT / _GONE / _CLEARED               */
    uint32_t reserved;
} ClipboardChange;

#ifdef __cplusplus
static_assert(sizeof(ClipboardRecord) == 32, "ClipboardRecord is a kernel/userspace ABI and must stay 32 bytes");
static_assert(sizeof(ClipboardPage) == 32, "ClipboardPage is a kernel/userspace ABI and must stay 32 bytes");
static_assert(sizeof(ClipboardChange) == 16, "ClipboardChange is a kernel/userspace ABI and must stay 16 bytes");
#else
_Static_assert(sizeof(ClipboardRecord) == 32, "ClipboardRecord is a kernel/userspace ABI and must stay 32 bytes");
_Static_assert(sizeof(ClipboardPage) == 32, "ClipboardPage is a kernel/userspace ABI and must stay 32 bytes");
_Static_assert(sizeof(ClipboardChange) == 16, "ClipboardChange is a kernel/userspace ABI and must stay 16 bytes");
#endif

#endif /* BOXOS_CLIPBOARD_H */
