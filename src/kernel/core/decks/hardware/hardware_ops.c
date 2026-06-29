/*
 * Hardware Deck — Manifest-native handlers (Phase 8 complete).
 *
 * Every prefix-chain handler in hardware_deck.c has a corresponding op here,
 * but the ABI is freed from the 192-byte cargo cult: bytes flow through
 * Crates of arbitrary size, fixed inputs through op->params.
 *
 * Param/Crate layouts are documented inline next to each handler so the
 * userspace builder can be regenerated mechanically.
 */

#include "klib.h"
#include "op_registry.h"
#include "manifest_auth.h"
#include "boxos_manifest.h"
#include "boxos_crate.h"
#include "hardware_deck.h"
#include "crate_io.h"
#include "process.h"
#include "video.h"
#include "pit.h"
#include "serial.h"
#include "rtc.h"
#include "keyboard.h"
#include "io.h"
#include "irqchip.h"
#include "ata.h"
#include "tagfs.h"
#include "system_halt.h"
#include "xhci.h"
#include "xhci_port.h"
#include "xhci_enumeration.h"
#include "serial.h"
#include "touch.h"
#include "kring.h"
#include "kernel_config.h"

#define VGA_PUTSTRING_FLAG_KEEP_COLOR 0x02u

/* Mirror userspace VGA prints to COM1 so headless QEMU runs leave a
 * shell-output trace in build/serial.log. Kernel-side prints already
 * reach serial via kputchar; this fills the gap for user Manifest VGA
 * ops. Gated behind CONFIG_VIDEO_SERIAL_MIRROR — keep OFF on real HW
 * where serial is not wired or where 115200 baud (~87us/char) would
 * dominate latency on long output. */
static inline void HwVgaMirrorChar(char ch)
{
#if CONFIG_VIDEO_SERIAL_MIRROR
    if (ch == '\n') serial_putchar('\r');
    serial_putchar(ch);
#else
    (void)ch;
#endif
}

static bool hw_irq_is_valid(uint8_t irq)
{
    return irq < irqchip_max_irqs() && irq != 0 && irq != 2;
}

/* =========================================================================
 *  VGA
 * ========================================================================= */

/* HW_VGA_PUTCHAR  params:[u8 row][u8 col][u8 char][u8 color] */
static int HwVgaPutChar(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                        const OpContext *ctx)
{
    (void)crates; (void)crate_count; (void)ctx;
    if (op->param_size < 4) return ERR_INVALID_ARGUMENT;

    uint8_t row   = op->params[0];
    uint8_t col   = op->params[1];
    uint8_t ch    = op->params[2];
    uint8_t color = op->params[3];

    if (row >= (uint8_t)VideoGetRows() || col >= (uint8_t)VideoGetCols()) {
        return ERR_OUT_OF_RANGE;
    }

    console_lock_acquire();

    uint8_t old_x     = (uint8_t)VideoGetCursorX();
    uint8_t old_y     = (uint8_t)VideoGetCursorY();
    uint8_t old_color = VideoGetColor();

    VideoSetCursor(col, row);
    VideoSetColor(color);
    VideoPrintChar((char)ch, color);
    HwVgaMirrorChar((char)ch);

    VideoSetCursor(old_x, old_y);
    VideoSetColor(old_color);

    console_lock_release();
    return OK;
}

/* HW_VGA_PUTSTRING  params:[u8 color][u8 flags]
 *                   in_crate: string bytes (size = byte count, no length cap)
 *                   out_crate (optional): [u8 chars_written][u8 row][u8 col] */
static int HwVgaPutString(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                          const OpContext *ctx)
{
    (void)crate_count;
    if (op->in_crate == CRATE_INDEX_NONE) return ERR_INVALID_ARGUMENT;
    if (op->param_size < 2)                return ERR_INVALID_ARGUMENT;

    uint8_t color = op->params[0];
    uint8_t flags = op->params[1];

    Crate *str_crate = &crates[op->in_crate];
    /* Snapshot the user string BEFORE taking the console lock, so user memory
     * is page-walked (and any page straddle handled) outside the lock.
     *
     * str_crate->size is attacker-controlled and this op is OP_AUTH_NONE, so
     * snapshot at most one page — a console line is a screenful, never the
     * gigabytes a caller could claim. CrateIsValid (manifest_exec) already
     * guarantees size <= capacity, so `want` is always a safe read length. */
    uint64_t want = str_crate->size < 4096u ? str_crate->size : 4096u;
    if (want == 0) return ERR_INVALID_ADDRESS;
    char *str = kmalloc((size_t)want);
    if (!str) return ERR_NO_MEMORY;
    if (crate_read(str_crate, ctx, str, want) != OK) {
        kfree(str);
        return ERR_INVALID_ADDRESS;
    }

    /* Hold the console lock around the whole VGA + serial-mirror run.
     * The framebuffer and cursor are global state; without serialisation
     * a kprintf from another core (or another user process calling
     * vga_puts in parallel) would interleave at cell-level and produce
     * the character-salad screen the user saw on 2026-05-15. */
    console_lock_acquire();

    uint8_t old_color = VideoGetColor();
    VideoSetColor(color);

    uint64_t chars_written = 0;
    VideoBatchBegin();
    for (uint64_t i = 0; i < want; i++) {
        char c = str[i];
        if (c == '\0') break;
        VideoPrintChar(c, color);
        chars_written++;
    }
    VideoBatchEnd();
    VideoUpdateCursor();

#if CONFIG_VIDEO_SERIAL_MIRROR
    serial_write(str, (size_t)chars_written);
#endif

    if (!(flags & VGA_PUTSTRING_FLAG_KEEP_COLOR)) {
        VideoSetColor(old_color);
    }

    console_lock_release();

    if (op->out_crate != CRATE_INDEX_NONE) {
        Crate *out = &crates[op->out_crate];
        if (out->capacity >= 3) {
            uint8_t blob[3];
            blob[0] = (uint8_t)(chars_written > 255 ? 255 : chars_written);
            blob[1] = (uint8_t)VideoGetCursorY();
            blob[2] = (uint8_t)VideoGetCursorX();
            (void)crate_write(out, ctx, blob, 3);
        }
    }

    kfree(str);
    return OK;
}

/* HW_VGA_CLEAR_SCREEN  params:[u8 color] */
static int HwVgaClearScreen(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                            const OpContext *ctx)
{
    (void)crates; (void)crate_count; (void)ctx;
    if (op->param_size < 1) return ERR_INVALID_ARGUMENT;

    uint8_t color     = op->params[0];
    uint8_t old_color = VideoGetColor();
    VideoSetColor(color);
    VideoClearScreen();
    VideoSetColor(old_color);
    return OK;
}

/* HW_VGA_CLEAR_LINE  params:[u8 row][u8 color] */
static int HwVgaClearLine(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                          const OpContext *ctx)
{
    (void)crates; (void)crate_count; (void)ctx;
    if (op->param_size < 2) return ERR_INVALID_ARGUMENT;

    uint8_t row   = op->params[0];
    uint8_t color = op->params[1];

    if ((int)row >= VideoGetRows()) return ERR_OUT_OF_RANGE;

    uint8_t old_color = VideoGetColor();
    VideoSetColor(color);
    VideoClearLine(row);
    VideoSetColor(old_color);
    return OK;
}

/* HW_VGA_CLEAR_TO_EOL  params: none */
static int HwVgaClearToEol(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                           const OpContext *ctx)
{
    (void)op; (void)crates; (void)crate_count; (void)ctx;
    VideoClearToEol();
    return OK;
}

/* HW_VGA_GET_CURSOR  out_crate:[u8 row][u8 col] */
static int HwVgaGetCursor(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                          const OpContext *ctx)
{
    (void)crate_count;
    if (op->out_crate == CRATE_INDEX_NONE) return ERR_INVALID_ARGUMENT;
    Crate *out = &crates[op->out_crate];
    if (out->capacity < 2) return ERR_BUFFER_TOO_SMALL;

    uint8_t blob[2];
    blob[0] = (uint8_t)VideoGetCursorY();
    blob[1] = (uint8_t)VideoGetCursorX();
    if (crate_write(out, ctx, blob, 2) != OK) return ERR_INVALID_ADDRESS;
    return OK;
}

/* HW_VGA_SET_CURSOR  params:[u8 row][u8 col]
 *                    out_crate (optional): clamped [u8 row][u8 col] */
static int HwVgaSetCursor(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                          const OpContext *ctx)
{
    (void)crate_count;
    if (op->param_size < 2) return ERR_INVALID_ARGUMENT;

    uint8_t row = op->params[0];
    uint8_t col = op->params[1];
    VideoSetCursor(col, row);

    if (op->out_crate != CRATE_INDEX_NONE) {
        Crate *out = &crates[op->out_crate];
        if (out->capacity >= 2) {
            uint8_t blob[2];
            blob[0] = (uint8_t)VideoGetCursorY();
            blob[1] = (uint8_t)VideoGetCursorX();
            (void)crate_write(out, ctx, blob, 2);
        }
    }
    return OK;
}

/* HW_VGA_SET_COLOR  params:[u8 color]
 *                   out_crate (optional): u8 old_color */
static int HwVgaSetColor(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                         const OpContext *ctx)
{
    (void)crate_count;
    if (op->param_size < 1) return ERR_INVALID_ARGUMENT;

    uint8_t color     = op->params[0];
    uint8_t old_color = VideoGetColor();
    VideoSetColor(color);

    if (op->out_crate != CRATE_INDEX_NONE) {
        Crate *out = &crates[op->out_crate];
        if (out->capacity >= 1) {
            (void)crate_write(out, ctx, &old_color, 1);
        }
    }
    return OK;
}

/* HW_VGA_GET_COLOR  out_crate: u8 color */
static int HwVgaGetColor(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                         const OpContext *ctx)
{
    (void)crate_count;
    if (op->out_crate == CRATE_INDEX_NONE) return ERR_INVALID_ARGUMENT;
    Crate *out = &crates[op->out_crate];
    if (out->capacity < 1) return ERR_BUFFER_TOO_SMALL;

    uint8_t color = VideoGetColor();
    if (crate_write(out, ctx, &color, 1) != OK) return ERR_INVALID_ADDRESS;
    return OK;
}

/* HW_VGA_SCROLL_UP  no params */
static int HwVgaScrollUp(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                         const OpContext *ctx)
{
    (void)op; (void)crates; (void)crate_count; (void)ctx;
    console_lock_acquire();
    VideoScrollUp();
    console_lock_release();
    return OK;
}

/* HW_VGA_NEWLINE  out_crate (optional): [u8 row][u8 col] */
static int HwVgaNewline(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                        const OpContext *ctx)
{
    (void)crate_count;
    console_lock_acquire();
    VideoPrintNewline();
    HwVgaMirrorChar('\n');
    uint8_t blob[2];
    blob[0] = (uint8_t)VideoGetCursorY();
    blob[1] = (uint8_t)VideoGetCursorX();
    console_lock_release();

    if (op->out_crate != CRATE_INDEX_NONE) {
        Crate *out = &crates[op->out_crate];
        if (out->capacity >= 2) {
            (void)crate_write(out, ctx, blob, 2);
        }
    }
    return OK;
}

/* HW_VGA_GET_DIMENSIONS  out_crate:[u8 cols][u8 rows] */
static int HwVgaGetDimensions(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                              const OpContext *ctx)
{
    (void)crate_count;
    if (op->out_crate == CRATE_INDEX_NONE) return ERR_INVALID_ARGUMENT;
    Crate *out = &crates[op->out_crate];
    if (out->capacity < 2) return ERR_BUFFER_TOO_SMALL;

    uint8_t blob[2];
    blob[0] = (uint8_t)VideoGetCols();
    blob[1] = (uint8_t)VideoGetRows();
    if (crate_write(out, ctx, blob, 2) != OK) return ERR_INVALID_ADDRESS;
    return OK;
}

/* =========================================================================
 *  Timer
 * ========================================================================= */

static int HwTimerGetTicks(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                           const OpContext *ctx)
{
    (void)crate_count;
    if (op->out_crate == CRATE_INDEX_NONE) return ERR_INVALID_ARGUMENT;
    Crate *out = &crates[op->out_crate];
    if (out->capacity < sizeof(uint64_t)) return ERR_BUFFER_TOO_SMALL;

    uint64_t ticks = pit_get_ticks();
    if (crate_write(out, ctx, &ticks, sizeof(uint64_t)) != OK) return ERR_INVALID_ADDRESS;
    return OK;
}

static int HwTimerGetMs(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                        const OpContext *ctx)
{
    (void)crate_count;
    if (op->out_crate == CRATE_INDEX_NONE) return ERR_INVALID_ARGUMENT;
    Crate *out = &crates[op->out_crate];
    if (out->capacity < sizeof(uint64_t)) return ERR_BUFFER_TOO_SMALL;

    /* Use the dedicated monotonic uptime counter (advanced per-tick by
     * 1_000_000/freq µs) instead of deriving from `ticks * 1000 / freq`,
     * which is NOT monotonic when the scheduler reprograms the PIT under
     * load. The old derivation produced backwards-jumps causing S1's
     * "elapsed=0xFFFFFFFFFFFF…" underflow. */
    uint64_t ms = pit_get_uptime_ms();
    if (crate_write(out, ctx, &ms, sizeof(uint64_t)) != OK) return ERR_INVALID_ADDRESS;
    return OK;
}

static int HwTimerGetFreq(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                          const OpContext *ctx)
{
    (void)crate_count;
    if (op->out_crate == CRATE_INDEX_NONE) return ERR_INVALID_ARGUMENT;
    Crate *out = &crates[op->out_crate];
    if (out->capacity < sizeof(uint32_t)) return ERR_BUFFER_TOO_SMALL;

    uint32_t freq = pit_get_frequency();
    if (crate_write(out, ctx, &freq, sizeof(uint32_t)) != OK) return ERR_INVALID_ADDRESS;
    return OK;
}

/* =========================================================================
 *  RTC
 * ========================================================================= */

static int HwRtcGetUnix64(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                          const OpContext *ctx)
{
    (void)crate_count;
    if (op->out_crate == CRATE_INDEX_NONE) return ERR_INVALID_ARGUMENT;
    Crate *out = &crates[op->out_crate];
    if (out->capacity < sizeof(uint64_t)) return ERR_BUFFER_TOO_SMALL;

    uint64_t secs = rtc_get_unix64();
    if (crate_write(out, ctx, &secs, sizeof(uint64_t)) != OK) return ERR_INVALID_ADDRESS;
    return OK;
}

/* HW_RTC_GET_TIME  out_crate: time_t (20 bytes packed) */
static int HwRtcGetTime(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                        const OpContext *ctx)
{
    (void)crate_count;
    if (op->out_crate == CRATE_INDEX_NONE) return ERR_INVALID_ARGUMENT;
    Crate *out = &crates[op->out_crate];
    if (out->capacity < sizeof(time_t)) return ERR_BUFFER_TOO_SMALL;

    time_t t;
    rtc_get_boxtime(&t);
    if (crate_write(out, ctx, &t, sizeof(time_t)) != OK) return ERR_INVALID_ADDRESS;
    return OK;
}

/* HW_RTC_GET_UPTIME  out_crate: u64 ns */
static int HwRtcGetUptime(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                          const OpContext *ctx)
{
    (void)crate_count;
    if (op->out_crate == CRATE_INDEX_NONE) return ERR_INVALID_ARGUMENT;
    Crate *out = &crates[op->out_crate];
    if (out->capacity < sizeof(uint64_t)) return ERR_BUFFER_TOO_SMALL;

    uint64_t ns = rtc_get_uptime_ns();
    if (crate_write(out, ctx, &ns, sizeof(uint64_t)) != OK) return ERR_INVALID_ADDRESS;
    return OK;
}

/* =========================================================================
 *  Port I/O
 * ========================================================================= */

static int HwPortInb(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                     const OpContext *ctx)
{
    (void)crate_count;
    if (op->param_size < 2) return ERR_INVALID_ARGUMENT;
    if (op->out_crate == CRATE_INDEX_NONE) return ERR_INVALID_ARGUMENT;

    uint16_t port = (uint16_t)op->params[0] | ((uint16_t)op->params[1] << 8);
    Crate *out = &crates[op->out_crate];
    if (out->capacity < 1) return ERR_BUFFER_TOO_SMALL;

    uint8_t value = inb(port);
    if (crate_write(out, ctx, &value, 1) != OK) return ERR_INVALID_ADDRESS;
    return OK;
}

static int HwPortOutb(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                      const OpContext *ctx)
{
    (void)crates; (void)crate_count; (void)ctx;
    if (op->param_size < 3) return ERR_INVALID_ARGUMENT;

    uint16_t port  = (uint16_t)op->params[0] | ((uint16_t)op->params[1] << 8);
    uint8_t  value = op->params[2];
    outb(port, value);
    return OK;
}

static int HwPortInw(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                     const OpContext *ctx)
{
    (void)crate_count;
    if (op->param_size < 2) return ERR_INVALID_ARGUMENT;
    if (op->out_crate == CRATE_INDEX_NONE) return ERR_INVALID_ARGUMENT;

    uint16_t port = (uint16_t)op->params[0] | ((uint16_t)op->params[1] << 8);
    Crate *out = &crates[op->out_crate];
    if (out->capacity < sizeof(uint16_t)) return ERR_BUFFER_TOO_SMALL;

    uint16_t value = inw(port);
    if (crate_write(out, ctx, &value, sizeof(uint16_t)) != OK) return ERR_INVALID_ADDRESS;
    return OK;
}

static int HwPortOutw(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                      const OpContext *ctx)
{
    (void)crates; (void)crate_count; (void)ctx;
    if (op->param_size < 4) return ERR_INVALID_ARGUMENT;

    uint16_t port  = (uint16_t)op->params[0] | ((uint16_t)op->params[1] << 8);
    uint16_t value;
    memcpy(&value, &op->params[2], sizeof(uint16_t));
    outw(port, value);
    return OK;
}

static int HwPortInl(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                     const OpContext *ctx)
{
    (void)crate_count;
    if (op->param_size < 2) return ERR_INVALID_ARGUMENT;
    if (op->out_crate == CRATE_INDEX_NONE) return ERR_INVALID_ARGUMENT;

    uint16_t port = (uint16_t)op->params[0] | ((uint16_t)op->params[1] << 8);
    Crate *out = &crates[op->out_crate];
    if (out->capacity < sizeof(uint32_t)) return ERR_BUFFER_TOO_SMALL;

    uint32_t value = inl(port);
    if (crate_write(out, ctx, &value, sizeof(uint32_t)) != OK) return ERR_INVALID_ADDRESS;
    return OK;
}

static int HwPortOutl(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                      const OpContext *ctx)
{
    (void)crates; (void)crate_count; (void)ctx;
    if (op->param_size < 6) return ERR_INVALID_ARGUMENT;

    uint16_t port = (uint16_t)op->params[0] | ((uint16_t)op->params[1] << 8);
    uint32_t value;
    memcpy(&value, &op->params[2], sizeof(uint32_t));
    outl(port, value);
    return OK;
}

/* =========================================================================
 *  IRQ
 * ========================================================================= */

static int HwIrqEnable(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                       const OpContext *ctx)
{
    (void)crates; (void)crate_count; (void)ctx;
    if (op->param_size < 1) return ERR_INVALID_ARGUMENT;
    uint8_t irq = op->params[0];
    if (!hw_irq_is_valid(irq)) return ERR_ACCESS_DENIED;
    irqchip_enable_irq(irq);
    return OK;
}

static int HwIrqDisable(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                        const OpContext *ctx)
{
    (void)crates; (void)crate_count; (void)ctx;
    if (op->param_size < 1) return ERR_INVALID_ARGUMENT;
    uint8_t irq = op->params[0];
    if (!hw_irq_is_valid(irq)) return ERR_ACCESS_DENIED;
    irqchip_disable_irq(irq);
    return OK;
}

static int HwIrqSendEoi(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                        const OpContext *ctx)
{
    (void)crates; (void)crate_count; (void)ctx;
    if (op->param_size < 1) return ERR_INVALID_ARGUMENT;
    uint8_t irq = op->params[0];
    if (!hw_irq_is_valid(irq)) return ERR_ACCESS_DENIED;
    irqchip_send_eoi(irq);
    return OK;
}

static int HwIrqGetIsr(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                       const OpContext *ctx)
{
    (void)crate_count;
    if (op->out_crate == CRATE_INDEX_NONE) return ERR_INVALID_ARGUMENT;
    Crate *out = &crates[op->out_crate];
    if (out->capacity < sizeof(uint32_t)) return ERR_BUFFER_TOO_SMALL;

    uint32_t isr = irqchip_get_isr();
    if (crate_write(out, ctx, &isr, sizeof(uint32_t)) != OK) return ERR_INVALID_ADDRESS;
    return OK;
}

static int HwIrqGetIrr(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                       const OpContext *ctx)
{
    (void)crate_count;
    if (op->out_crate == CRATE_INDEX_NONE) return ERR_INVALID_ARGUMENT;
    Crate *out = &crates[op->out_crate];
    if (out->capacity < sizeof(uint32_t)) return ERR_BUFFER_TOO_SMALL;

    uint32_t irr = irqchip_get_irr();
    if (crate_write(out, ctx, &irr, sizeof(uint32_t)) != OK) return ERR_INVALID_ADDRESS;
    return OK;
}

/* =========================================================================
 *  CPU
 * ========================================================================= */

static int HwCpuHalt(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                     const OpContext *ctx)
{
    (void)op; (void)crates; (void)crate_count; (void)ctx;
    hlt();
    return OK;
}

/* =========================================================================
 *  Disk (ATA primary master/slave info + cache flush)
 * ========================================================================= */

/* HW_DISK_INFO  params:[u8 is_master]
 *               out_crate: [u8 exists][char model[40]][char serial[20]]
 *                          [u64 total_sectors][u64 size_mb]   = 77 bytes */
static int HwDiskInfo(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                      const OpContext *ctx)
{
    (void)crate_count;
    if (op->param_size < 1) return ERR_INVALID_ARGUMENT;
    if (op->out_crate == CRATE_INDEX_NONE) return ERR_INVALID_ARGUMENT;

    Crate *out = &crates[op->out_crate];
    if (out->capacity < 77) return ERR_BUFFER_TOO_SMALL;

    uint8_t       is_master = op->params[0];
    ATADevice    *dev       = is_master ? &ata_primary_master : &ata_primary_slave;

    uint8_t blob[77];
    blob[0] = (uint8_t)dev->exists;
    memcpy(blob + 1,  dev->model,  40);
    memcpy(blob + 41, dev->serial, 20);
    memcpy(blob + 61, &dev->total_sectors, sizeof(uint64_t));
    memcpy(blob + 69, &dev->size_mb,       sizeof(uint64_t));
    if (crate_write(out, ctx, blob, 77) != OK) return ERR_INVALID_ADDRESS;
    return OK;
}

/* HW_DISK_FLUSH  params:[u8 is_master]
 *
 * The userspace ABI still carries the legacy 0/1 master/slave flag,
 * but the only durable thing it can mean today is "flush the TagFS
 * volume" — that is the device any commit really cares about. Route
 * through tagfs_flush_cache(), which resolves the volume location
 * (AHCI port number or ATA drive index) at the storage layer and
 * therefore stays correct on every (ATA, AHCI) × (boot port) combo.
 * The is_master parameter is preserved for ABI stability and logged
 * for diagnostics.
 */
static int HwDiskFlush(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                       const OpContext *ctx)
{
    (void)crates; (void)crate_count; (void)ctx;
    if (op->param_size < 1) return ERR_INVALID_ARGUMENT;
    (void)op->params[0];
    return tagfs_flush_cache();
}

/* =========================================================================
 *  Keyboard
 * ========================================================================= */

static int HwKbGetChar(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                       const OpContext *ctx)
{
    (void)crate_count;
    if (op->out_crate == CRATE_INDEX_NONE) return ERR_INVALID_ARGUMENT;
    Crate *out = &crates[op->out_crate];
    if (out->capacity < 4) return ERR_BUFFER_TOO_SMALL;

    uint8_t blob[4];

    if (!keyboard_has_input()) {
        blob[0] = 0; blob[1] = 0; blob[2] = 0; blob[3] = HW_KB_NO_DATA;
        if (crate_write(out, ctx, blob, 4) != OK) return ERR_INVALID_ADDRESS;
        return OK;
    }

    char ch = keyboard_getchar();
    keyboard_state_t *kbs = keyboard_get_state();
    /* Atomic loads — kb_state is mutated from PS/2 IRQ (BSP) and xHCI HID
     * IRQ; this op runs on the calling user-core. */
    uint8_t mods = 0;
    if (__atomic_load_n(&kbs->shift_pressed, __ATOMIC_RELAXED)) mods |= 0x01;
    if (__atomic_load_n(&kbs->ctrl_pressed,  __ATOMIC_RELAXED)) mods |= 0x02;
    if (__atomic_load_n(&kbs->alt_pressed,   __ATOMIC_RELAXED)) mods |= 0x04;

    blob[0] = (uint8_t)ch;
    blob[1] = __atomic_load_n(&kbs->last_keycode, __ATOMIC_RELAXED);
    blob[2] = mods;
    blob[3] = HW_KB_SUCCESS;
    if (crate_write(out, ctx, blob, 4) != OK) return ERR_INVALID_ADDRESS;
    return OK;
}

static int HwKbReadLine(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                        const OpContext *ctx)
{
    (void)crate_count;
    if (op->out_crate == CRATE_INDEX_NONE) return ERR_INVALID_ARGUMENT;
    if (op->param_size < 3)                return ERR_INVALID_ARGUMENT;

    uint16_t max_length = (uint16_t)op->params[0] | ((uint16_t)op->params[1] << 8);
    uint8_t  echo_mode  = op->params[2];

    Crate *out = &crates[op->out_crate];
    if (out->capacity < 5) return ERR_BUFFER_TOO_SMALL;

    uint64_t data_capacity = out->capacity - 5;
    if (max_length == 0 || max_length > data_capacity) {
        max_length = (uint16_t)(data_capacity > 0xFFFFu ? 0xFFFFu : data_capacity);
    }

    keyboard_set_echo(echo_mode != 0);

    /* out->capacity is attacker-controlled and this op is OP_AUTH_NONE, so the
     * kernel bounce is bounded to the head region this op actually produces —
     * a 4-byte length, the clamped line, and one NUL (<= ~64 KiB) — never the
     * claimed capacity, which could be gigabytes. The trailing status byte
     * lives at the ABI's last slot (out->addr + capacity - 1) and is committed
     * separately below. */
    uint64_t head_size = 4 + (uint64_t)max_length + 1;
    uint8_t *kp = crate_out_alloc(out, head_size);
    if (!kp) return ERR_INVALID_ADDRESS;

    /* crate_out_alloc zero-fills the bounce, so the length field and the line's
     * NUL terminator are already 0; readline fills the line slot at +4. */
    char *line_dst = (char *)(kp + 4);
    int ready = keyboard_readline_async(line_dst, max_length);

    uint8_t  status;
    uint64_t head_bytes;
    if (ready) {
        size_t len = 0;
        while (len < max_length && line_dst[len] != '\0') len++;
        uint32_t len32 = (uint32_t)len;
        memcpy(kp, &len32, sizeof(uint32_t));
        status     = HW_KB_SUCCESS;
        head_bytes = 4 + (uint64_t)len + 1;
    } else {
        status     = HW_KB_WOULD_BLOCK;
        head_bytes = 5;
    }

    /* Commit the head region (length + line + NUL) at offset 0 (out->addr). */
    int crc = crate_out_commit(out, ctx, kp, head_bytes);
    crate_buf_free(kp);
    if (crc != OK) return ERR_INVALID_ADDRESS;

    /* Stamp the single status byte at its ABI slot, out->addr + capacity - 1,
     * which the head commit does not cover. crate_write targets offset 0, so
     * retarget a one-byte view of the crate at that exact address — same
     * page-walked write (and no-cabin memcpy) path as the head commit. */
    Crate status_slot    = *out;
    status_slot.addr     = out->addr + out->capacity - 1;
    status_slot.capacity = 1;
    if (crate_write(&status_slot, ctx, &status, 1) != OK) return ERR_INVALID_ADDRESS;

    out->size = head_bytes;
    return ready ? OK : ERR_WOULD_BLOCK;
}

/* HW_KEYBOARD_STATUS  out_crate:[u32 available][u32 buffer_size] */
static int HwKbStatus(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                      const OpContext *ctx)
{
    (void)crate_count;
    if (op->out_crate == CRATE_INDEX_NONE) return ERR_INVALID_ARGUMENT;
    Crate *out = &crates[op->out_crate];
    if (out->capacity < 8) return ERR_BUFFER_TOO_SMALL;

    uint32_t available = keyboard_available();
    uint32_t buf_size  = KEYBOARD_LINE_BUFFER_SIZE;

    uint8_t blob[8];
    memcpy(blob,     &available, sizeof(uint32_t));
    memcpy(blob + 4, &buf_size,  sizeof(uint32_t));
    if (crate_write(out, ctx, blob, 8) != OK) return ERR_INVALID_ADDRESS;
    return OK;
}

/* =========================================================================
 *  System power (reboot / shutdown — noreturn)
 * ========================================================================= */

static int HwSystemReboot(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                          const OpContext *ctx)
{
    (void)op; (void)crates; (void)crate_count;
    debug_printf("[HardwareDeck] reboot requested by PID %u\n",
                 (ctx && ctx->proc) ? ctx->proc->pid : 0);
    system_halt(true);
    return OK; /* unreachable */
}

static int HwSystemShutdown(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                            const OpContext *ctx)
{
    (void)op; (void)crates; (void)crate_count;
    debug_printf("[HardwareDeck] shutdown requested by PID %u\n",
                 (ctx && ctx->proc) ? ctx->proc->pid : 0);
    system_halt(false);
    return OK; /* unreachable */
}

/* =========================================================================
 *  USB (xHCI control surface)
 * ========================================================================= */

static int HwUsbInit(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                     const OpContext *ctx)
{
    (void)op; (void)crates; (void)crate_count; (void)ctx;
    return xhci_init() == 0 ? OK : ERR_INTERNAL;
}

static int HwUsbReset(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                      const OpContext *ctx)
{
    (void)op; (void)crates; (void)crate_count; (void)ctx;
    xhci_controller_t *c = xhci_get_controller();
    if (!c) return ERR_NOT_INITIALIZED;
    return xhci_reset(c) == 0 ? OK : ERR_INTERNAL;
}

static int HwUsbStart(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                      const OpContext *ctx)
{
    (void)op; (void)crates; (void)crate_count; (void)ctx;
    xhci_controller_t *c = xhci_get_controller();
    if (!c) return ERR_NOT_INITIALIZED;
    return xhci_start(c) == 0 ? OK : ERR_INTERNAL;
}

static int HwUsbStop(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                     const OpContext *ctx)
{
    (void)op; (void)crates; (void)crate_count; (void)ctx;
    /* No xhci_stop API in the driver — keep semantics aligned with legacy:
     * acknowledge the request without actually halting the controller. */
    return OK;
}

/* HW_USB_PORT_STATUS  params:[u8 port]  out_crate: u32 portsc */
static int HwUsbPortStatus(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                           const OpContext *ctx)
{
    (void)crate_count;
    if (op->param_size < 1) return ERR_INVALID_ARGUMENT;
    if (op->out_crate == CRATE_INDEX_NONE) return ERR_INVALID_ARGUMENT;

    uint8_t port = op->params[0];
    xhci_controller_t *c = xhci_get_controller();
    if (!c || !c->initialized) return ERR_NOT_INITIALIZED;
    if (port == 0 || port > c->max_ports) return ERR_INVALID_ARGUMENT;

    Crate *out = &crates[op->out_crate];
    if (out->capacity < sizeof(uint32_t)) return ERR_BUFFER_TOO_SMALL;
    uint32_t portsc = xhci_get_port_status(c, port);

    if (crate_write(out, ctx, &portsc, sizeof(uint32_t)) != OK) return ERR_INVALID_ADDRESS;
    return OK;
}

/* HW_USB_PORT_RESET  params:[u8 port] */
static int HwUsbPortReset(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                          const OpContext *ctx)
{
    (void)crates; (void)crate_count; (void)ctx;
    if (op->param_size < 1) return ERR_INVALID_ARGUMENT;
    uint8_t port = op->params[0];

    xhci_controller_t *c = xhci_get_controller();
    if (!c || !c->initialized)            return ERR_NOT_INITIALIZED;
    if (port == 0 || port > c->max_ports) return ERR_INVALID_ARGUMENT;
    if (!xhci_port_has_device(c, port))   return ERR_DEVICE_NOT_READY;

    return xhci_reset_port(c, port) == 0 ? OK : ERR_INTERNAL;
}

/* HW_USB_PORT_QUERY  out_crate:[u8 max_ports][u8 max_slots][u8 irq][u8 polling] */
static int HwUsbPortQuery(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                          const OpContext *ctx)
{
    (void)crate_count;
    if (op->out_crate == CRATE_INDEX_NONE) return ERR_INVALID_ARGUMENT;
    xhci_controller_t *c = xhci_get_controller();
    if (!c || !c->initialized) return ERR_NOT_INITIALIZED;

    Crate *out = &crates[op->out_crate];
    if (out->capacity < 4) return ERR_BUFFER_TOO_SMALL;
    uint8_t blob[4];
    blob[0] = c->max_ports;
    blob[1] = c->max_slots;
    blob[2] = c->irq_line;
    blob[3] = c->use_polling ? 1 : 0;
    if (crate_write(out, ctx, blob, 4) != OK) return ERR_INVALID_ADDRESS;
    return OK;
}

/* HW_USB_ENUM_DEVICE  params:[u8 port] */
static int HwUsbEnumDevice(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                           const OpContext *ctx)
{
    (void)crates; (void)crate_count; (void)ctx;
    if (op->param_size < 1) return ERR_INVALID_ARGUMENT;
    uint8_t port = op->params[0];
    xhci_controller_t *c = xhci_get_controller();
    if (!c || !c->running) return ERR_NOT_INITIALIZED;

    int rc = xhci_enumerate_device(c, port);
    if (rc == 0)        return OK;
    if (rc == -2 || rc == -3) return ERR_INVALID_ARGUMENT;
    return ERR_INTERNAL;
}

/* HW_USB_GET_INFO  params:[u8 slot_id]  out_crate:[u8 slot][u8 port][u8 state] */
static int HwUsbGetInfo(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                        const OpContext *ctx)
{
    (void)crate_count;
    if (op->param_size < 1) return ERR_INVALID_ARGUMENT;
    if (op->out_crate == CRATE_INDEX_NONE) return ERR_INVALID_ARGUMENT;

    uint8_t slot_id = op->params[0];
    xhci_controller_t *c = xhci_get_controller();
    if (!c || !c->running) return ERR_NOT_INITIALIZED;
    xhci_device_slot_t *slot = xhci_get_device_slot(c, slot_id);
    if (!slot) return ERR_DEVICE_NOT_READY;

    Crate *out = &crates[op->out_crate];
    if (out->capacity < 3) return ERR_BUFFER_TOO_SMALL;
    uint8_t blob[3];
    blob[0] = slot->slot_id;
    blob[1] = slot->port_num;
    blob[2] = slot->state;
    if (crate_write(out, ctx, blob, 3) != OK) return ERR_INVALID_ADDRESS;
    return OK;
}

/* =========================================================================
 *  Debug print — serial output from userspace via kernel kprintf
 *  HW_DEBUG_PRINT  in_crate: NUL-terminated string (max 256 bytes)
 * ========================================================================= */

static int HwDebugPrint(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                        const OpContext *ctx)
{
    (void)crate_count;
    if (op->in_crate == CRATE_INDEX_NONE) return ERR_INVALID_ARGUMENT;

    Crate *c = &crates[op->in_crate];
    uint64_t bytes = c->size < 256 ? c->size : 256;
    if (bytes == 0) return ERR_INVALID_ARGUMENT;

    char buf[257];
    if (crate_read(c, ctx, buf, bytes) != OK) return ERR_INVALID_ADDRESS;
    buf[bytes] = '\0';

    uint32_t pid = (ctx && ctx->proc) ? ctx->proc->pid : 0;
    kprintf("[%u] %s\n", pid, buf);
    return OK;
}

/* =========================================================================
 *  Registration
 * ========================================================================= */

error_t HardwareDeckRegister(void)
{
    struct {
        uint16_t    opcode;
        OpHandler   handler;
        uint32_t    auth;
        const char *name;
    } table[] = {
        /* Timer + RTC: read-only telemetry, anyone can use. */
        { HW_TIMER_GET_TICKS,    HwTimerGetTicks,    OP_AUTH_NONE,   "hw.timer.ticks"   },
        { HW_TIMER_GET_MS,       HwTimerGetMs,       OP_AUTH_NONE,   "hw.timer.ms"      },
        { HW_TIMER_GET_FREQ,     HwTimerGetFreq,     OP_AUTH_NONE,   "hw.timer.freq"    },
        { HW_RTC_GET_TIME,       HwRtcGetTime,       OP_AUTH_NONE,   "hw.rtc.time"      },
        { HW_RTC_GET_UNIX64,     HwRtcGetUnix64,     OP_AUTH_NONE,   "hw.rtc.unix64"    },
        { HW_RTC_GET_UPTIME,     HwRtcGetUptime,     OP_AUTH_NONE,   "hw.rtc.uptime"    },
        /* Port I/O: arbitrary I/O space access — system+ only. */
        { HW_PORT_INB,           HwPortInb,          OP_AUTH_SYSTEM, "hw.port.inb"      },
        { HW_PORT_OUTB,          HwPortOutb,         OP_AUTH_SYSTEM, "hw.port.outb"     },
        { HW_PORT_INW,           HwPortInw,          OP_AUTH_SYSTEM, "hw.port.inw"      },
        { HW_PORT_OUTW,          HwPortOutw,         OP_AUTH_SYSTEM, "hw.port.outw"     },
        { HW_PORT_INL,           HwPortInl,          OP_AUTH_SYSTEM, "hw.port.inl"      },
        { HW_PORT_OUTL,          HwPortOutl,         OP_AUTH_SYSTEM, "hw.port.outl"     },
        /* IRQ control: privileged. */
        { HW_IRQ_ENABLE,         HwIrqEnable,        OP_AUTH_SYSTEM, "hw.irq.enable"    },
        { HW_IRQ_DISABLE,        HwIrqDisable,       OP_AUTH_SYSTEM, "hw.irq.disable"   },
        { HW_IRQ_GET_ISR,        HwIrqGetIsr,        OP_AUTH_SYSTEM, "hw.irq.isr"       },
        { HW_IRQ_GET_IRR,        HwIrqGetIrr,        OP_AUTH_SYSTEM, "hw.irq.irr"       },
        { HW_IRQ_SEND_EOI,       HwIrqSendEoi,       OP_AUTH_SYSTEM, "hw.irq.eoi"       },
        /* CPU halt: privileged (would freeze the system if app called it). */
        { HW_CPU_HALT,           HwCpuHalt,          OP_AUTH_SYSTEM, "hw.cpu.halt"      },
        /* Disk: info is read-only; flush is cooperative — both NONE. */
        { HW_DISK_INFO,          HwDiskInfo,         OP_AUTH_NONE,   "hw.disk.info"     },
        { HW_DISK_FLUSH,         HwDiskFlush,        OP_AUTH_NONE,   "hw.disk.flush"    },
        /* Keyboard: anyone reading their own focused input. */
        { HW_KEYBOARD_GETCHAR,   HwKbGetChar,        OP_AUTH_NONE,   "hw.kb.getchar"    },
        { HW_KEYBOARD_READLINE,  HwKbReadLine,       OP_AUTH_NONE,   "hw.kb.readline"   },
        { HW_KEYBOARD_STATUS,    HwKbStatus,         OP_AUTH_NONE,   "hw.kb.status"     },
        /* VGA: cosmetic, anyone. */
        { HW_VGA_PUTCHAR,        HwVgaPutChar,       OP_AUTH_NONE,   "hw.vga.putchar"   },
        { HW_VGA_PUTSTRING,      HwVgaPutString,     OP_AUTH_NONE,   "hw.vga.putstring" },
        { HW_VGA_CLEAR_SCREEN,   HwVgaClearScreen,   OP_AUTH_NONE,   "hw.vga.clear"     },
        { HW_VGA_CLEAR_LINE,     HwVgaClearLine,     OP_AUTH_NONE,   "hw.vga.clear_line"},
        { HW_VGA_CLEAR_TO_EOL,   HwVgaClearToEol,    OP_AUTH_NONE,   "hw.vga.clear_eol" },
        { HW_VGA_GET_CURSOR,     HwVgaGetCursor,     OP_AUTH_NONE,   "hw.vga.cursor.get"},
        { HW_VGA_SET_CURSOR,     HwVgaSetCursor,     OP_AUTH_NONE,   "hw.vga.cursor.set"},
        { HW_VGA_SET_COLOR,      HwVgaSetColor,      OP_AUTH_NONE,   "hw.vga.color.set" },
        { HW_VGA_GET_COLOR,      HwVgaGetColor,      OP_AUTH_NONE,   "hw.vga.color.get" },
        { HW_VGA_SCROLL_UP,      HwVgaScrollUp,      OP_AUTH_NONE,   "hw.vga.scroll"    },
        { HW_VGA_NEWLINE,        HwVgaNewline,       OP_AUTH_NONE,   "hw.vga.newline"   },
        { HW_VGA_GET_DIMENSIONS, HwVgaGetDimensions, OP_AUTH_NONE,   "hw.vga.dims"      },
        /* System power: only system-tagged processes can reboot/shutdown. */
        { HW_SYSTEM_REBOOT,      HwSystemReboot,     OP_AUTH_SYSTEM, "hw.system.reboot"  },
        { HW_SYSTEM_SHUTDOWN,    HwSystemShutdown,   OP_AUTH_SYSTEM, "hw.system.shutdown"},
        { HW_DEBUG_PRINT,        HwDebugPrint,       OP_AUTH_NONE,   "hw.debug.print"    },
        /* USB: hardware control, system+. */
        { HW_USB_INIT,           HwUsbInit,          OP_AUTH_SYSTEM, "hw.usb.init"      },
        { HW_USB_RESET,          HwUsbReset,         OP_AUTH_SYSTEM, "hw.usb.reset"     },
        { HW_USB_START,          HwUsbStart,         OP_AUTH_SYSTEM, "hw.usb.start"     },
        { HW_USB_STOP,           HwUsbStop,          OP_AUTH_SYSTEM, "hw.usb.stop"      },
        { HW_USB_PORT_STATUS,    HwUsbPortStatus,    OP_AUTH_SYSTEM, "hw.usb.port.status"},
        { HW_USB_PORT_RESET,     HwUsbPortReset,     OP_AUTH_SYSTEM, "hw.usb.port.reset"},
        { HW_USB_PORT_QUERY,     HwUsbPortQuery,     OP_AUTH_SYSTEM, "hw.usb.port.query"},
        { HW_USB_ENUM_DEVICE,    HwUsbEnumDevice,    OP_AUTH_SYSTEM, "hw.usb.enum"      },
        { HW_USB_GET_INFO,       HwUsbGetInfo,       OP_AUTH_SYSTEM, "hw.usb.info"      },
    };

    for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
        error_t rc = OpRegistryRegister(OP_KIND(DECK_HARDWARE, table[i].opcode),
                                        table[i].handler,
                                        table[i].auth,
                                        table[i].name);
        if (rc != OK && rc != ERR_ALREADY_EXISTS) {
            kprintf("[HardwareDeck] register %s failed: %s\n",
                    table[i].name, ErrorString(rc));
            return rc;
        }
    }
    debug_printf("[HardwareDeck] registered %zu ops (full surface, gated)\n",
                 sizeof(table) / sizeof(table[0]));
    return OK;
}
