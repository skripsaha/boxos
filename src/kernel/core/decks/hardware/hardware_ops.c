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
#include "vmm.h"
#include "process.h"
#include "video.h"
#include "pit.h"
#include "rtc.h"
#include "keyboard.h"
#include "io.h"
#include "irqchip.h"
#include "ata.h"
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

/* -------------------------------------------------------------------------
 * Crate translation
 * ------------------------------------------------------------------------- */

static void *HwCrateMap(const Crate *c, const OpContext *ctx, uint64_t bytes)
{
    if (!c || bytes == 0)            return NULL;
    if (bytes > c->capacity)         return NULL;
    if (ctx && ctx->proc && ctx->proc->cabin) {
        return vmm_translate_user_addr(ctx->proc->cabin, (uintptr_t)c->addr, (size_t)bytes);
    }
    return (void *)(uintptr_t)c->addr;
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

    uint8_t old_x     = (uint8_t)VideoGetCursorX();
    uint8_t old_y     = (uint8_t)VideoGetCursorY();
    uint8_t old_color = VideoGetColor();

    VideoSetCursor(col, row);
    VideoSetColor(color);
    VideoPrintChar((char)ch, color);
    HwVgaMirrorChar((char)ch);

    VideoSetCursor(old_x, old_y);
    VideoSetColor(old_color);
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
    const char *str = HwCrateMap(str_crate, ctx, str_crate->size);
    if (!str) return ERR_INVALID_ADDRESS;

    uint8_t old_color = VideoGetColor();
    VideoSetColor(color);

    uint64_t chars_written = 0;
    VideoBatchBegin();
    for (uint64_t i = 0; i < str_crate->size; i++) {
        char c = str[i];
        if (c == '\0') break;
        VideoPrintChar(c, color);
        HwVgaMirrorChar(c);
        chars_written++;
    }
    VideoBatchEnd();
    VideoUpdateCursor();

    if (!(flags & VGA_PUTSTRING_FLAG_KEEP_COLOR)) {
        VideoSetColor(old_color);
    }

    if (op->out_crate != CRATE_INDEX_NONE) {
        Crate *out = &crates[op->out_crate];
        if (out->capacity >= 3) {
            uint8_t *out_kp = HwCrateMap(out, ctx, 3);
            if (out_kp) {
                out_kp[0] = (uint8_t)(chars_written > 255 ? 255 : chars_written);
                out_kp[1] = (uint8_t)VideoGetCursorY();
                out_kp[2] = (uint8_t)VideoGetCursorX();
                out->size = 3;
            }
        }
    }
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

    uint8_t *kp = HwCrateMap(out, ctx, 2);
    if (!kp) return ERR_INVALID_ADDRESS;

    kp[0] = (uint8_t)VideoGetCursorY();
    kp[1] = (uint8_t)VideoGetCursorX();
    out->size = 2;
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
            uint8_t *kp = HwCrateMap(out, ctx, 2);
            if (kp) {
                kp[0] = (uint8_t)VideoGetCursorY();
                kp[1] = (uint8_t)VideoGetCursorX();
                out->size = 2;
            }
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
            uint8_t *kp = HwCrateMap(out, ctx, 1);
            if (kp) { kp[0] = old_color; out->size = 1; }
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

    uint8_t *kp = HwCrateMap(out, ctx, 1);
    if (!kp) return ERR_INVALID_ADDRESS;

    kp[0] = VideoGetColor();
    out->size = 1;
    return OK;
}

/* HW_VGA_SCROLL_UP  no params */
static int HwVgaScrollUp(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                         const OpContext *ctx)
{
    (void)op; (void)crates; (void)crate_count; (void)ctx;
    VideoScrollUp();
    return OK;
}

/* HW_VGA_NEWLINE  out_crate (optional): [u8 row][u8 col] */
static int HwVgaNewline(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                        const OpContext *ctx)
{
    (void)crate_count;
    VideoPrintNewline();
    HwVgaMirrorChar('\n');

    if (op->out_crate != CRATE_INDEX_NONE) {
        Crate *out = &crates[op->out_crate];
        if (out->capacity >= 2) {
            uint8_t *kp = HwCrateMap(out, ctx, 2);
            if (kp) {
                kp[0] = (uint8_t)VideoGetCursorY();
                kp[1] = (uint8_t)VideoGetCursorX();
                out->size = 2;
            }
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

    uint8_t *kp = HwCrateMap(out, ctx, 2);
    if (!kp) return ERR_INVALID_ADDRESS;

    kp[0] = (uint8_t)VideoGetCols();
    kp[1] = (uint8_t)VideoGetRows();
    out->size = 2;
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

    void *kp = HwCrateMap(out, ctx, sizeof(uint64_t));
    if (!kp) return ERR_INVALID_ADDRESS;

    uint64_t ticks = pit_get_ticks();
    memcpy(kp, &ticks, sizeof(uint64_t));
    out->size = sizeof(uint64_t);
    return OK;
}

static int HwTimerGetMs(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                        const OpContext *ctx)
{
    (void)crate_count;
    if (op->out_crate == CRATE_INDEX_NONE) return ERR_INVALID_ARGUMENT;
    Crate *out = &crates[op->out_crate];
    if (out->capacity < sizeof(uint64_t)) return ERR_BUFFER_TOO_SMALL;

    uint32_t freq = pit_get_frequency();
    if (freq == 0) return ERR_NOT_INITIALIZED;

    void *kp = HwCrateMap(out, ctx, sizeof(uint64_t));
    if (!kp) return ERR_INVALID_ADDRESS;

    uint64_t ms = (pit_get_ticks() * 1000ULL) / freq;
    memcpy(kp, &ms, sizeof(uint64_t));
    out->size = sizeof(uint64_t);
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
    void *kp = HwCrateMap(out, ctx, sizeof(uint32_t));
    if (!kp) return ERR_INVALID_ADDRESS;

    memcpy(kp, &freq, sizeof(uint32_t));
    out->size = sizeof(uint32_t);
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

    void *kp = HwCrateMap(out, ctx, sizeof(uint64_t));
    if (!kp) return ERR_INVALID_ADDRESS;

    uint64_t secs = rtc_get_unix64();
    memcpy(kp, &secs, sizeof(uint64_t));
    out->size = sizeof(uint64_t);
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

    void *kp = HwCrateMap(out, ctx, sizeof(time_t));
    if (!kp) return ERR_INVALID_ADDRESS;

    time_t t;
    rtc_get_boxtime(&t);
    memcpy(kp, &t, sizeof(time_t));
    out->size = sizeof(time_t);
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

    void *kp = HwCrateMap(out, ctx, sizeof(uint64_t));
    if (!kp) return ERR_INVALID_ADDRESS;

    uint64_t ns = rtc_get_uptime_ns();
    memcpy(kp, &ns, sizeof(uint64_t));
    out->size = sizeof(uint64_t);
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

    uint8_t *kp = HwCrateMap(out, ctx, 1);
    if (!kp) return ERR_INVALID_ADDRESS;

    kp[0] = inb(port);
    out->size = 1;
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
    void *kp = HwCrateMap(out, ctx, sizeof(uint16_t));
    if (!kp) return ERR_INVALID_ADDRESS;
    memcpy(kp, &value, sizeof(uint16_t));
    out->size = sizeof(uint16_t);
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
    void *kp = HwCrateMap(out, ctx, sizeof(uint32_t));
    if (!kp) return ERR_INVALID_ADDRESS;
    memcpy(kp, &value, sizeof(uint32_t));
    out->size = sizeof(uint32_t);
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
    void *kp = HwCrateMap(out, ctx, sizeof(uint32_t));
    if (!kp) return ERR_INVALID_ADDRESS;
    memcpy(kp, &isr, sizeof(uint32_t));
    out->size = sizeof(uint32_t);
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
    void *kp = HwCrateMap(out, ctx, sizeof(uint32_t));
    if (!kp) return ERR_INVALID_ADDRESS;
    memcpy(kp, &irr, sizeof(uint32_t));
    out->size = sizeof(uint32_t);
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

    uint8_t *kp = HwCrateMap(out, ctx, 77);
    if (!kp) return ERR_INVALID_ADDRESS;

    uint8_t       is_master = op->params[0];
    ATADevice    *dev       = is_master ? &ata_primary_master : &ata_primary_slave;

    kp[0] = (uint8_t)dev->exists;
    memcpy(kp + 1,  dev->model,  40);
    memcpy(kp + 41, dev->serial, 20);
    memcpy(kp + 61, &dev->total_sectors, sizeof(uint64_t));
    memcpy(kp + 69, &dev->size_mb,       sizeof(uint64_t));
    out->size = 77;
    return OK;
}

/* HW_DISK_FLUSH  params:[u8 is_master] */
static int HwDiskFlush(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                       const OpContext *ctx)
{
    (void)crates; (void)crate_count; (void)ctx;
    if (op->param_size < 1) return ERR_INVALID_ARGUMENT;
    uint8_t is_master = op->params[0];
    return ata_flush_cache(is_master) == 0 ? OK : ERR_INTERNAL;
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

    uint8_t *kp = HwCrateMap(out, ctx, 4);
    if (!kp) return ERR_INVALID_ADDRESS;

    if (!keyboard_has_input()) {
        kp[0] = 0; kp[1] = 0; kp[2] = 0; kp[3] = HW_KB_NO_DATA;
        out->size = 4;
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

    kp[0] = (uint8_t)ch;
    kp[1] = __atomic_load_n(&kbs->last_keycode, __ATOMIC_RELAXED);
    kp[2] = mods;
    kp[3] = HW_KB_SUCCESS;
    out->size = 4;
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

    uint8_t *kp = HwCrateMap(out, ctx, out->capacity);
    if (!kp) return ERR_INVALID_ADDRESS;

    char *line_dst = (char *)(kp + 4);
    for (uint16_t i = 0; i < (uint16_t)(max_length + 1) && (uint64_t)(4u + i) < out->capacity; i++) {
        line_dst[i] = 0;
    }

    int ready = keyboard_readline_async(line_dst, max_length);
    if (ready) {
        size_t len = 0;
        while (len < max_length && line_dst[len] != '\0') len++;
        uint32_t len32 = (uint32_t)len;
        memcpy(kp, &len32, sizeof(uint32_t));
        kp[out->capacity - 1] = HW_KB_SUCCESS;
        out->size = 4 + len + 1;
        return OK;
    }

    uint32_t zero = 0;
    memcpy(kp, &zero, sizeof(uint32_t));
    kp[out->capacity - 1] = HW_KB_WOULD_BLOCK;
    out->size = 5;
    return ERR_WOULD_BLOCK;
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

    uint8_t *kp = HwCrateMap(out, ctx, 8);
    if (!kp) return ERR_INVALID_ADDRESS;

    memcpy(kp,     &available, sizeof(uint32_t));
    memcpy(kp + 4, &buf_size,  sizeof(uint32_t));
    out->size = 8;
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

    void *kp = HwCrateMap(out, ctx, sizeof(uint32_t));
    if (!kp) return ERR_INVALID_ADDRESS;
    memcpy(kp, &portsc, sizeof(uint32_t));
    out->size = sizeof(uint32_t);
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
    uint8_t *kp = HwCrateMap(out, ctx, 4);
    if (!kp) return ERR_INVALID_ADDRESS;

    kp[0] = c->max_ports;
    kp[1] = c->max_slots;
    kp[2] = c->irq_line;
    kp[3] = c->use_polling ? 1 : 0;
    out->size = 4;
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
    uint8_t *kp = HwCrateMap(out, ctx, 3);
    if (!kp) return ERR_INVALID_ADDRESS;

    kp[0] = slot->slot_id;
    kp[1] = slot->port_num;
    kp[2] = slot->state;
    out->size = 3;
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

    const char *src = HwCrateMap(c, ctx, bytes);
    if (!src) return ERR_INVALID_ADDRESS;

    char buf[257];
    uint64_t copy = bytes < 256 ? bytes : 256;
    memcpy(buf, src, copy);
    buf[copy] = '\0';

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
