#ifndef RTC_H
#define RTC_H

#include "klib.h"

/* ‼ SECOND DEFINITION. box/time.h declares this same structure for userspace,
 * and the two must stay byte-identical: HW_RTC_GET_TIME writes it here and
 * boxlib's time_get() reads it there, field by field, out of a 20-byte crate.
 * A field added on one side alone is silent corruption, not a build error —
 * the same shape as the two message tables <cstring>'s strerror and
 * <system_error>'s generic_category had to be taught to share in Ф41-b.
 * The static_assert below is what makes a size change loud on this side.
 *
 * It was called time_t on both sides until Ф41-e, which needed that name for
 * the arithmetic type [ctime.syn] requires. Boxtime is what the tree had
 * always called the thing — rtc_get_boxtime, clock_boxtime — so the type
 * finally got the name its functions already had. */
typedef struct __attribute__((packed)) {
    uint64_t seconds;
    uint32_t nanosec;
    uint16_t year;
    uint8_t  month;
    uint8_t  day;
    uint8_t  hour;
    uint8_t  minute;
    uint8_t  second;
    uint8_t  weekday;
} BoxTime;

_Static_assert(sizeof(BoxTime) == 20, "BoxTime must be 20 bytes");

void     rtc_init(void);

/* The calendar fields, or the declared absence of them. On a machine with no
 * wall clock — the FADT says there is no CMOS RTC, Register D says the part
 * lost power, the CMOS never answered, or what it answered did not decode
 * inside the registers' own ranges — every field is written zero. Month 0 is
 * not a date any reading can produce, and seconds == 0 is the same "unknown"
 * the whole tree already reads off boot_unix_secs. */
void     rtc_get_boxtime(BoxTime* out);

/* Seconds since 1970, or 0 when this machine has no wall clock. */
uint64_t rtc_get_unix64(void);
uint64_t rtc_get_uptime_ns(void);

/* Toggle the NMI mask state baked into every subsequent CMOS register
 * selection. Default after rtc_init is "NMI enabled" (mask byte = 0).
 * Pairs must be balanced — disable() before a critical sequence,
 * enable() immediately after — to avoid permanently silencing the
 * system NMI source. */
void cmos_nmi_disable(void);
void cmos_nmi_enable(void);

#endif // RTC_H
