#include "rtc.h"
#include "io.h"
#include "pit.h"
#include "hourglass.h"
#include "atomics.h"
#include "acpi_internal.h"   /* g_acpi.fadt — the platform's word on whether
                              * there is a CMOS RTC at 0x70/0x71 at all */

#define CMOS_ADDR       0x70
#define CMOS_DATA       0x71

#define CMOS_REG_SEC    0x00
#define CMOS_REG_MIN    0x02
#define CMOS_REG_HOUR   0x04
#define CMOS_REG_DAY    0x07
#define CMOS_REG_MON    0x08
#define CMOS_REG_YEAR   0x09
#define CMOS_REG_STA    0x0A
#define CMOS_REG_STB    0x0B
#define CMOS_REG_STD    0x0D

#define STA_UIP         0x80
#define STB_BIN         0x04
#define STB_24H         0x02

/* MC146818A, REGISTER D ($0D), bit 7: "VRT — The valid RAM and time (VRT)
 * bit indicates the status of the contents of the RAM... A '0' appears in
 * the VRT bit when the power-sense pin is low... VRT is a read only bit
 * which is not modified by the RESET pin. The VRT bit can only be set by
 * reading Register D." It is the part's own statement that its RAM and its
 * time survived since they were last set — and it is the ONLY way to tell
 * a dead coin cell apart from a working clock, because a part on a dead
 * cell still answers while the machine is powered and hands back an
 * in-range, entirely meaningless reset date that no range check can catch.
 *
 * Because reading Register D is what sets VRT back to 1, the register is
 * read exactly once, at init, before anything else touches the CMOS. */
#define STD_VRT         0x80

/* ACPI 6.5 §5.2.9.3, IA-PC Boot Architecture Flags, bit 5, "CMOS RTC Not
 * Present": "If set, indicates that the CMOS RTC is either not implemented,
 * or does not exist at the legacy addresses. OSPM uses the Control Method
 * Time and Alarm Namespace device instead." Linux acts on the same bit —
 * arch/x86/kernel/acpi/boot.c clears x86_platform.legacy.rtc on it and the
 * platform device is never registered. BoxOS has no Time and Alarm device,
 * so on such a board there is no wall clock at all: a fact to state, not a
 * pair of ports to read anyway. */
#define FADT_NO_CMOS_RTC   (1u << 5)

typedef struct {
    uint64_t base_seconds;
    /* The monotonic microsecond reading taken at the same instant as
     * base_seconds, on the same clock the rest of the kernel reports uptime
     * with. It used to be a count of IRQ0 ticks, and that made the wall clock
     * lose the whole boot: the tick counter cannot move before the BSP enables
     * interrupts, which on the 2026-09-20 board happened 12.3 s after this
     * reading was taken, and nothing ever went back for those seconds. */
    uint64_t base_uptime_us;
    uint16_t year;
    uint8_t  month;
    uint8_t  day;
    uint8_t  hour;
    uint8_t  minute;
    uint8_t  second;
    uint8_t  weekday;
    /* False until a reading has been proven to come from a real, powered,
     * answering RTC and to decode inside the fields' own ranges. While it
     * is false this kernel HAS NO WALL CLOCK, and every accessor says so
     * instead of handing out a plausible number. */
    bool     wall_known;
} rtc_state_t;

static rtc_state_t rtc_state;

/* NMI mask byte OR'd into every CMOS register selection.
 *
 * Port 0x70 bit 7 is the system-wide NMI mask: writing 1 disables
 * NMI, writing 0 enables it. The byte is write-only — the chip does
 * not expose the current state for read-back — so every outb to 0x70
 * must explicitly choose the NMI state along with the register index.
 *
 * The legacy implementation here OR'd 0x80 unconditionally into every
 * read, masking NMI for the duration of every PIO cycle. On real HW
 * with watchdog/IOCHK/SERR# wired into NMI, that trains a
 * microsecond-wide NMI blackout *every* `cmos_read()` call (the
 * scheduler and rtc_get_unix64 path do this dozens of times per
 * second), which can drop legitimate hardware errors silently.
 *
 * Default: bit 7 = 0 (NMI enabled). Callers that genuinely require
 * an NMI-free CMOS sequence (e.g. arming the alarm interrupt) call
 * cmos_nmi_disable() → ... → cmos_nmi_enable(). */
static uint8_t g_cmos_nmi_select = 0x00;

void cmos_nmi_disable(void) { g_cmos_nmi_select = 0x80; }
void cmos_nmi_enable(void)  { g_cmos_nmi_select = 0x00; }

static uint8_t cmos_read(uint8_t reg) {
    outb(CMOS_ADDR, g_cmos_nmi_select | reg);
    return inb(CMOS_DATA);
}

/* The longest the part may legitimately hold UIP, taken from the part itself.
 *
 * MC146818A datasheet, "UPDATE CYCLE": driven from the 32.768 kHz time base
 * the update transfer takes 1984 us, and the UIP status bit is raised 244 us
 * before that transfer begins. 244 + 1984 = 2228 us is therefore the whole
 * window in which any reader can find UIP set, and the chip opens that window
 * once per second, so two of them can never abut.
 *
 * The patience is that window taken twice. The second window is a DECLARED
 * MARGIN, not a spec figure: it pays for the poll's own I/O latency and for an
 * SMI that steals the core mid-wait, and since the part cannot start a second
 * update inside it, a wait that outlives it is not a slow RTC — it is an RTC
 * that is not answering. */
#define RTC_UIP_WINDOW_US     2228u
#define RTC_UIP_PATIENCE_US   (2u * RTC_UIP_WINDOW_US)

/* Returns true once the RTC clears its Update-In-Progress bit, false once the
 * patience above has run out. A false is REFUSAL, and rtc_init treats it as
 * one: on a part whose UIP never clears there is no reading to be had, which
 * is what linux's mc146818_get_time() says by zeroing the time and returning
 * -ETIMEDOUT. The two-read consistency check in rtc_init is not a safety net
 * for this case and never was — on a board where port 0x71 answers 0xFF both
 * reads return 0xFF, compare equal, and the check passes.
 *
 * The million iterations that stood here were not time at all. On a board
 * where CMOS port 0x71 does not answer — RTC disabled in firmware, a dead coin
 * cell, an unclaimed LPC/eSPI cycle reading back 0xFF — register 0x0A reads
 * 0xFF, UIP is stuck set by definition, and boot spent a million CMOS accesses
 * discovering it. A million is also not the same amount of time on any two
 * machines, which is the part that makes it unarguable.
 *
 * rtc_init runs after hpet_init, so a ruler exists by now; the branch that
 * says otherwise is here because a missing ruler must never quietly become an
 * invented one. */
static bool wait_not_uip(void) {
    HourGlass glass;
    if (!HourGlassTurn(&glass, RTC_UIP_PATIENCE_US)) {
        kprintf("[RTC] no time source on this machine — waiting for UIP to "
                "clear on the fact itself, with no bound\n");
        while (cmos_read(CMOS_REG_STA) & STA_UIP) cpu_pause();
        return true;
    }
    for (;;) {
        if ((cmos_read(CMOS_REG_STA) & STA_UIP) == 0) return true;
        if (HourGlassRunOut(&glass)) break;
        cpu_pause();
    }
    kprintf("[RTC] UIP still set after %lu us on the %s (%s) — the CMOS is not "
            "answering; the reading is refused\n",
            (unsigned long)HourGlassElapsedUs(&glass),
            HourGlassRulerName(&glass),
            HourGlassSourceDied(&glass) ? "ruler died mid-wait"
                                        : "span ran out");
    return false;
}

static uint8_t bcd_to_bin(uint8_t v) {
    return (v >> 4) * 10 + (v & 0x0F);
}

static uint64_t days_since_epoch(uint16_t y, uint8_t m, uint8_t d) {
    uint32_t yy = y, mm = m;
    if (mm <= 2) { yy--; mm += 9; } else { mm -= 3; }
    uint64_t era = yy / 400;
    uint32_t yoe = yy - (uint32_t)(era * 400);
    uint32_t doy = (153 * mm + 2) / 5 + d - 1;
    uint32_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468ULL;
}

/* Does the platform itself say there is no CMOS RTC at the legacy ports?
 * boot_arch_flags sits at FADT offset 109 and exists from FADT revision 3;
 * the length test is what keeps a short table from being read past its own
 * end, and a firmware that predates the field simply does not answer. */
static bool acpi_says_no_cmos_rtc(void) {
    if (!g_acpi.initialized || !g_acpi.fadt) return false;
    size_t need = __builtin_offsetof(acpi_fadt_t, boot_arch_flags)
                + sizeof(((acpi_fadt_t *)0)->boot_arch_flags);
    if (g_acpi.fadt->header.length < need) return false;
    return (g_acpi.fadt->boot_arch_flags & FADT_NO_CMOS_RTC) != 0;
}

/* The CMOS register index that holds the century, as named by the platform.
 *
 * ACPI 6.5 §5.2.9, FADT `century` (offset 108, one byte, immediately before
 * the boot_arch_flags read above): "The RTC CMOS RAM index to the century of
 * data value (hundred and thousand year decimals). If this field contains a
 * zero, then the RTC centenary feature is not supported." The index belongs
 * to the platform; 0x32 is only what PC firmware has usually chosen, and this
 * driver used to read it unconditionally. That was survivable while a nonsense
 * century merely shifted the year, and stopped being survivable the moment the
 * reading is range-checked: an arbitrary byte of CMOS RAM that decodes above 99
 * would now cost the machine its entire wall clock. Linux reads
 * acpi_gbl_FADT.century and reads no century register at all when the field is
 * zero (arch/x86/kernel/rtc.c, mach_get_cmos_time).
 *
 * Returns 0 when the platform names no index: no ACPI, a FADT too short to hold
 * the field, a table too old for the field to be trusted, the field itself
 * zero, or an index this driver must not address. */
#define FADT2_REVISION_ID   3

static uint8_t acpi_century_index(void) {
    if (!g_acpi.initialized || !g_acpi.fadt) return 0;
    size_t need = __builtin_offsetof(acpi_fadt_t, century)
                + sizeof(((acpi_fadt_t *)0)->century);
    if (g_acpi.fadt->header.length < need) return 0;

    /* The field is inside even a 116-byte ACPI 1.0 FADT, so the length test
     * above cannot tell a named index from a byte an old firmware never
     * filled in. Linux draws the line at the table's revision — "if
     * (acpi_gbl_FADT.header.revision >= FADT2_REVISION_ID &&
     * acpi_gbl_FADT.century)", with FADT2_REVISION_ID 3 in
     * include/acpi/actbl.h — and so does this driver. Below that revision
     * the century is taken as 20 further down, which is right until 2100
     * and never a refusal. */
    if (g_acpi.fadt->header.revision < FADT2_REVISION_ID) return 0;

    uint8_t index = g_acpi.fadt->century;

    /* An index with bit 7 set cannot be read here without a side effect on
     * something else entirely: cmos_read() ORs the index into port 0x70,
     * whose bit 7 is the system-wide NMI mask this file is careful with.
     * Reading the date would silently switch NMI off and leave it off. Xen
     * guards the same read the same way: "acpi_gbl_FADT.century &&
     * acpi_gbl_FADT.century < 0x80" in __get_cmos_time(). */
    if (index >= 0x80) {
        kprintf("[RTC] FADT names century index 0x%x, which carries the NMI "
                "mask bit of port 0x70 — not reading it; the century is "
                "taken as 20\n", (unsigned)index);
        return 0;
    }
    return index;
}

/* This kernel has no wall clock, and says so once, loudly, on the board.
 *
 * Everything the RTC would have published is left at zero — which is not a
 * placeholder date but the tree's existing signal for "unknown": boxlib's
 * clock.c already refuses a ClockBoard whose boot_unix_secs is zero, and
 * main.c hands it exactly what rtc_get_unix64() returns. The alternative
 * this replaces was a decoded 0xFF reading that yielded the year 25755 and
 * unix second 751,276,322,355, passed every downstream guard because it was
 * non-zero, and was written into TagFS metadata, DiskBook last_seen, COW
 * created_time and self-heal records, where no later boot corrects it. */
static void rtc_declare_no_wall_clock(const char *why) {
    memset(&rtc_state, 0, sizeof(rtc_state));
    rtc_state.base_uptime_us = pit_get_uptime_us();
    kprintf("[RTC] no wall clock on this machine: %s. Boot time is unknown "
            "and will be reported as unknown — nothing is invented.\n", why);
}

/* Every decoded field against the range of the register it came from, plus
 * the one bound the representation imposes.
 *
 * MC146818A, TABLE 2 (TIME, CALENDAR AND ALARM DATA MODES): Seconds 0-59,
 * Minutes 0-59, Hours 0-23 in the 24-hour mode, Date of Month 1-31, Month
 * 1-12, Year 0-99. A decode outside those is not a strange date, it is a
 * register that did not answer: bcd_to_bin(0xFF) is 165, and that is how an
 * unclaimed LPC/eSPI cycle used to reach the calendar arithmetic and come
 * back with the year 25755.
 *
 * The last bound is ours rather than the part's. The value is published as
 * a count of seconds since 1970 and days_since_epoch() computes it in
 * unsigned arithmetic, so a year before the epoch does not yield a wrong
 * date -- it yields a wrap. */
static bool rtc_reading_is_sane(uint8_t sec, uint8_t min, uint8_t hour,
                                uint8_t day, uint8_t mon, uint8_t year2,
                                uint8_t century, uint16_t full_year) {
    if (sec > 59 || min > 59 || hour > 23) return false;
    if (day < 1 || day > 31)               return false;
    if (mon < 1 || mon > 12)               return false;
    if (year2 > 99 || century > 99)        return false;
    if (full_year < 1970)                  return false;
    return true;
}

void rtc_init(void) {
    rtc_state.wall_known = false;

    if (acpi_says_no_cmos_rtc()) {
        rtc_declare_no_wall_clock("the FADT sets IA-PC Boot Architecture "
                                  "flag bit 5, CMOS RTC Not Present");
        return;
    }

    /* Register D first and once — reading it is what sets VRT back. */
    if ((cmos_read(CMOS_REG_STD) & STD_VRT) == 0) {
        rtc_declare_no_wall_clock("Register D reports VRT = 0 — the RTC lost "
                                  "power, so its RAM and its time are void");
        return;
    }

    /* Asked before the reads begin: it is a look at a mapped ACPI table, not
     * a CMOS cycle, and it has no business inside the update window. */
    uint8_t cent_index = acpi_century_index();

    if (!wait_not_uip()) {
        rtc_declare_no_wall_clock("UIP never cleared — the CMOS is not "
                                  "answering at ports 0x70/0x71");
        return;
    }

    uint8_t sec  = cmos_read(CMOS_REG_SEC);
    uint8_t min  = cmos_read(CMOS_REG_MIN);
    uint8_t hour = cmos_read(CMOS_REG_HOUR);
    uint8_t day  = cmos_read(CMOS_REG_DAY);
    uint8_t mon  = cmos_read(CMOS_REG_MON);
    uint8_t year = cmos_read(CMOS_REG_YEAR);
    uint8_t cent = (cent_index != 0) ? cmos_read(cent_index) : 0;
    uint8_t stb  = cmos_read(CMOS_REG_STB);

    if (!wait_not_uip()) {
        rtc_declare_no_wall_clock("UIP never cleared between the two reads — "
                                  "the CMOS stopped answering mid-init");
        return;
    }

    uint8_t sec2  = cmos_read(CMOS_REG_SEC);
    uint8_t min2  = cmos_read(CMOS_REG_MIN);
    uint8_t hour2 = cmos_read(CMOS_REG_HOUR);
    uint8_t day2  = cmos_read(CMOS_REG_DAY);
    uint8_t mon2  = cmos_read(CMOS_REG_MON);
    uint8_t year2 = cmos_read(CMOS_REG_YEAR);

    if (sec2 != sec || min2 != min || hour2 != hour ||
        day2 != day || mon2 != mon || year2 != year) {
        sec  = sec2;
        min  = min2;
        hour = hour2;
        day  = day2;
        mon  = mon2;
        year = year2;
    }

    bool binary_mode = (stb & STB_BIN) != 0;
    bool mode_24h    = (stb & STB_24H) != 0;

    if (!binary_mode) {
        bool pm = (!mode_24h) && (hour & 0x80);
        hour = bcd_to_bin(hour & 0x7F);
        if (!mode_24h) {
            if (pm && hour != 12) hour += 12;
            if (!pm && hour == 12) hour = 0;
        }
        sec  = bcd_to_bin(sec);
        min  = bcd_to_bin(min);
        day  = bcd_to_bin(day);
        mon  = bcd_to_bin(mon);
        year = bcd_to_bin(year);
    } else {
        if (!mode_24h) {
            bool pm = (hour & 0x80) != 0;
            hour &= 0x7F;
            if (pm && hour != 12) hour += 12;
            if (!pm && hour == 12) hour = 0;
        }
    }

    /* No century register — the platform named no index, or named one that
     * reads back zero — means the year is taken as 20xx. That is the same
     * assumption linux makes in the same situation (arch/x86/kernel/rtc.c:
     * "year += CMOS_EPOCH - 1900", CMOS_EPOCH = 2000), and it is bounded:
     * it is wrong only for a part whose two-digit year has rolled past 2099,
     * and it never turns into a refusal, because 20 is in range by
     * construction.
     *
     * When there IS one, taking it raw in binary mode is a decode and not
     * an omission, because the byte itself carries the century in both
     * encodings. ACPI 6.5 §4.8.2.4, RTC-CMOS registers, CENTURY: "8-bit BCD
     * or binary value. This value indicates the thousand year and hundred
     * year (Centenary) variables of the date in BCD (19 for this century,
     * 20 for the next) or binary (x13 for this century, x14 for the next)."
     * Raw 0x14 is 20 and bcd_to_bin(0x20) is 20 — the same century by two
     * spellings.
     *
     * WHICH spelling a platform used is a question ACPI does not answer: it
     * names both and no selector, and this byte lives in CMOS RAM at an
     * index the FADT gives rather than among the part's own calendar
     * registers, which is what Status B bit 2 governs in MC146818A. Reading
     * it under that bit anyway is what the reference implementation does —
     * linux decodes the century inside the very same gate as the calendar
     * fields (drivers/rtc/rtc-mc146818-lib.c: "if (!(p.ctrl &
     * RTC_DM_BINARY) || RTC_ALWAYS_BCD) { ... p.century = bcd2bin(
     * p.century); }") — and it is the only statement about encoding the
     * machine makes anywhere. */
    uint8_t century = (cent != 0) ? cent : 20;
    if (!binary_mode && cent != 0) {
        century = bcd_to_bin(cent);
    }

    uint16_t full_year = (uint16_t)century * 100 + year;

    if (!rtc_reading_is_sane(sec, min, hour, day, mon, year, century, full_year)) {
        kprintf("[RTC] CMOS decoded to %u-%u-%u %u:%u:%u (century %u) — "
                "outside the registers' own ranges\n",
                full_year, mon, day, hour, min, sec, century);
        rtc_declare_no_wall_clock("the CMOS registers did not decode to a date");
        return;
    }

    rtc_state.year    = full_year;
    rtc_state.month   = mon;
    rtc_state.day     = day;
    rtc_state.hour    = hour;
    rtc_state.minute  = min;
    rtc_state.second  = sec;

    /* The weekday comes from the date, not from CMOS register 0x06. That
     * register is not maintained by every part — linux refuses to read it
     * for exactly that reason ("Even though the RTC has RTC_DAY_OF_WEEK, we
     * ignore it, as it is only updated by the RTC when initially set to a
     * non-zero value", drivers/rtc/rtc-mc146818-lib.c) — and alone among
     * the fields its value cannot be range-checked into correctness: a
     * stale weekday is in range and still wrong. 1970-01-01 was a Thursday,
     * so epoch day 0 is weekday 4 counting Sunday as 0. */
    uint64_t epoch_days = days_since_epoch(full_year, mon, day);
    rtc_state.weekday   = (uint8_t)((epoch_days + 4ULL) % 7ULL);

    rtc_state.base_seconds   = epoch_days * 86400ULL
                             + (uint64_t)hour * 3600
                             + (uint64_t)min * 60
                             + sec;
    rtc_state.base_uptime_us = pit_get_uptime_us();
    rtc_state.wall_known     = true;

    /* Leave the index register at CMOS_REG_SEC (0x00) with the
     * current NMI mask state so any subsequent port-0x71 reader
     * (firmware SMI, third-party driver) gets a deterministic
     * register selection rather than whatever was last addressed. */
    outb(CMOS_ADDR, g_cmos_nmi_select | CMOS_REG_SEC);

    kprintf("[RTC] the CMOS reads %04u-%02u-%02u %02u:%02u:%02u (unix %llu) — "
            "this kernel takes that as the wall clock's zero and carries it "
            "forward on the same monotonic counter it reports uptime with, so "
            "the two can no longer disagree about how long the machine has "
            "been up\n",
            full_year, mon, day, hour, min, sec, rtc_state.base_seconds);
}

void rtc_get_boxtime(BoxTime* out) {
    /* The refusal rtc_get_unix64() makes, in the shape this accessor can make
     * it. BoxTime is the 20-byte crate userspace reads and it carries no
     * "unknown" flag, so the refusal IS the value: every field zero. It cannot
     * be mistaken for a reading — a reading that got past rtc_reading_is_sane()
     * has month 1..12 and day 1..31, so month == 0 is reachable from here and
     * nowhere else — and seconds == 0 is the signal the rest of the tree
     * already understands, the same one boxlib's clock.c reads off a
     * ClockBoard whose boot_unix_secs is zero.
     *
     * Copying the fields out unguarded produced those zeros by accident of
     * rtc_state having been memset, and presented them as a date read from a
     * part. The accident is now the contract. */
    if (!rtc_state.wall_known) {
        memset(out, 0, sizeof(*out));
        return;
    }

    out->year    = rtc_state.year;
    out->month   = rtc_state.month;
    out->day     = rtc_state.day;
    out->hour    = rtc_state.hour;
    out->minute  = rtc_state.minute;
    out->second  = rtc_state.second;
    out->weekday = rtc_state.weekday;
    out->seconds = rtc_state.base_seconds;
    out->nanosec = 0;
}

uint64_t rtc_get_unix64(void) {
    /* No wall clock means no wall clock. Returning seconds-since-boot here
     * would hand every caller a 1970 date that looks like one and gets
     * written to the medium; zero is both the truth and the signal the tree
     * already understands — boxlib's clock.c refuses a ClockBoard whose
     * boot_unix_secs is zero. */
    if (!rtc_state.wall_known) return 0;

    /* Elapsed on the same clock the base was taken on — pit_get_uptime_us()
     * is the HPET main counter wherever there is one, and the tick
     * accumulator only on a board without. There is no frequency to divide by
     * and no tick to wait for, so this advances while interrupts are off,
     * which is where a boot spends most of its length. */
    uint64_t elapsed_us = pit_get_uptime_us() - rtc_state.base_uptime_us;
    return rtc_state.base_seconds + elapsed_us / 1000000ULL;
}

uint64_t rtc_get_uptime_ns(void) {
    /* Use the monotonic microsecond counter — surviving freq changes — and
     * extend to nanoseconds. The old `(ticks - base) / freq * 1e9 + …`
     * derivation was non-monotonic when the scheduler reprogrammed the PIT
     * (see HwTimerGetMs comment). */
    return pit_get_uptime_us() * 1000ULL;
}
