
#include "klib.h"
#include "klib_logring.h"
#include "op_registry.h"
#include "manifest_auth.h"
#include "boxos_manifest.h"
#include "boxos_crate.h"
#include "hardware_deck.h"
#include "crate_io.h"
#include "process.h"
#include "video.h"
#include "pit.h"
#include "rtc.h"
#include "keyboard.h"
#include "io.h"
#include "irqchip.h"
#include "ata.h"
#include "tagfs.h"
#include "boardroom.h"
#include "system_halt.h"
#include "xhci.h"
#include "xhci_port.h"
#include "xhci_enumeration.h"
#include "touch.h"
#include "kring.h"
#include "kernel_config.h"

#define VGA_PUTSTRING_FLAG_KEEP_COLOR 0x02u

static inline void HwVgaMirrorChar(char ch)
{
    LogRingPut(ch);
}

static bool hw_irq_is_valid(uint8_t irq)
{
    return irq < irqchip_max_irqs() && irq != 0 && irq != 2;
}

static inline uint32_t hw_color_param(const uint8_t *p)
{
    uint32_t v;
    memcpy(&v, p, sizeof(v));
    return v;
}


static int HwVgaPutChar(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                        const OpContext *ctx)
{
    (void)crates; (void)crate_count; (void)ctx;
    if (op->param_size < 11) return ERR_INVALID_ARGUMENT;

    uint8_t  row = op->params[0];
    uint8_t  col = op->params[1];
    uint8_t  ch  = op->params[2];
    uint32_t fg  = hw_color_param(&op->params[3]);
    uint32_t bg  = hw_color_param(&op->params[7]);

    if ((int)row >= VideoGetRows() || (int)col >= VideoGetCols()) {
        return ERR_OUT_OF_RANGE;
    }

    console_lock_acquire();

    int old_x = VideoGetCursorX();
    int old_y = VideoGetCursorY();

    VideoBatchBegin();
    VideoSetCursor(col, row);
    VideoPrintCharRgb((char)ch, fg, bg);
    HwVgaMirrorChar((char)ch);
    VideoSetCursor(old_x, old_y);
    VideoBatchEnd();

    console_lock_release();
    return OK;
}

static int HwVgaPutString(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                          const OpContext *ctx)
{
    (void)crate_count;
    if (op->in_crate == CRATE_INDEX_NONE) return ERR_INVALID_ARGUMENT;
    if (op->param_size < 9)                return ERR_INVALID_ARGUMENT;

    uint32_t fg    = hw_color_param(&op->params[0]);
    uint32_t bg    = hw_color_param(&op->params[4]);
    uint8_t  flags = op->params[8];

    Crate *str_crate = &crates[op->in_crate];
    uint64_t want = str_crate->size < 4096u ? str_crate->size : 4096u;
    if (want == 0) return ERR_INVALID_ADDRESS;
    char *str = kmalloc((size_t)want);
    if (!str) return ERR_NO_MEMORY;
    if (crate_read(str_crate, ctx, str, want) != OK) {
        kfree(str);
        return ERR_INVALID_ADDRESS;
    }

    console_lock_acquire();

    uint32_t old_fg, old_bg;
    VideoGetColorRgb(&old_fg, &old_bg);
    VideoSetColorRgb(fg, bg);

    uint64_t chars_written = 0;
    VideoBatchBegin();
    for (uint64_t i = 0; i < want; i++) {
        char c = str[i];
        if (c == '\0') break;
        VideoPrintCharCur(c);
        LogRingPut(c);
        chars_written++;
    }
    VideoBatchEnd();
    VideoUpdateCursor();

    if (!(flags & VGA_PUTSTRING_FLAG_KEEP_COLOR)) {
        VideoSetColorRgb(old_fg, old_bg);
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

static int HwVgaPaint(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                      const OpContext *ctx)
{
    (void)crate_count;
    if (op->param_size < 4)               return ERR_INVALID_ARGUMENT;
    if (op->in_crate == CRATE_INDEX_NONE) return ERR_INVALID_ARGUMENT;

    uint32_t row    = op->params[0];
    uint32_t col    = op->params[1];
    uint32_t height = op->params[2];
    uint32_t width  = op->params[3];
    if (height == 0 || width == 0) return ERR_INVALID_ARGUMENT;

    Crate   *src  = &crates[op->in_crate];
    uint64_t want = (uint64_t)height * width * sizeof(TextCell);
    if (src->size != want) return ERR_INVALID_ARGUMENT;

    TextCell *cells = (TextCell *)crate_in_buf(src, ctx);
    if (!cells) return ERR_INVALID_ADDRESS;

    for (uint64_t i = 0, n = (uint64_t)height * width; i < n; i++) {
        cells[i].fg = BoxColorResolveFg(cells[i].fg);
        cells[i].bg = BoxColorResolveBg(cells[i].bg);
    }

    console_lock_acquire();
    bool fit = VideoPaintCells(row, col, height, width, cells);
    console_lock_release();

    crate_buf_free(cells);
    return fit ? OK : ERR_OUT_OF_RANGE;
}

static int HwVgaClearScreen(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                            const OpContext *ctx)
{
    (void)crates; (void)crate_count; (void)ctx;
    if (op->param_size < 8) return ERR_INVALID_ARGUMENT;

    VideoClearScreenRgb(hw_color_param(&op->params[0]),
                        hw_color_param(&op->params[4]));
    return OK;
}

static int HwVgaClearLine(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                          const OpContext *ctx)
{
    (void)crates; (void)crate_count; (void)ctx;
    if (op->param_size < 9) return ERR_INVALID_ARGUMENT;

    uint8_t row = op->params[0];
    if ((int)row >= VideoGetRows()) return ERR_OUT_OF_RANGE;

    VideoClearLineRgb(row, hw_color_param(&op->params[1]),
                           hw_color_param(&op->params[5]));
    return OK;
}

static int HwVgaClearToEol(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                           const OpContext *ctx)
{
    (void)op; (void)crates; (void)crate_count; (void)ctx;
    VideoClearToEol();
    return OK;
}

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

static int HwVgaStepCursor(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                           const OpContext *ctx)
{
    (void)crates; (void)crate_count; (void)ctx;
    if (op->param_size < 4) return ERR_INVALID_ARGUMENT;

    int32_t delta;
    memcpy(&delta, &op->params[0], sizeof(delta));
    if (delta == 0) return OK;

    console_lock_acquire();

    int cols = VideoGetCols();
    int rows = VideoGetRows();
    if (cols <= 0 || rows <= 0) {
        console_lock_release();
        return ERR_UNSUPPORTED;
    }

    int64_t linear = (int64_t)VideoGetCursorY() * cols + VideoGetCursorX() + delta;
    int64_t last   = (int64_t)rows * cols - 1;
    if (linear < 0)    linear = 0;
    if (linear > last) linear = last;

    VideoSetCursor((int)(linear % cols), (int)(linear / cols));

    console_lock_release();
    return OK;
}

static int HwVgaSetColor(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                         const OpContext *ctx)
{
    (void)crate_count;
    if (op->param_size < 8) return ERR_INVALID_ARGUMENT;

    uint32_t old[2];
    VideoGetColorRgb(&old[0], &old[1]);
    VideoSetColorRgb(hw_color_param(&op->params[0]),
                     hw_color_param(&op->params[4]));

    if (op->out_crate != CRATE_INDEX_NONE) {
        Crate *out = &crates[op->out_crate];
        if (out->capacity >= sizeof(old)) {
            (void)crate_write(out, ctx, old, sizeof(old));
        }
    }
    return OK;
}

static int HwVgaGetColor(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                         const OpContext *ctx)
{
    (void)crate_count;
    if (op->out_crate == CRATE_INDEX_NONE) return ERR_INVALID_ARGUMENT;
    Crate *out = &crates[op->out_crate];
    if (out->capacity < 8) return ERR_BUFFER_TOO_SMALL;

    uint32_t pair[2];
    VideoGetColorRgb(&pair[0], &pair[1]);
    if (crate_write(out, ctx, pair, sizeof(pair)) != OK) return ERR_INVALID_ADDRESS;
    return OK;
}

static int HwVgaScrollUp(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                         const OpContext *ctx)
{
    (void)op; (void)crates; (void)crate_count; (void)ctx;
    console_lock_acquire();
    VideoScrollUp();
    console_lock_release();
    return OK;
}

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

static int HwVgaGetDimensions(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                              const OpContext *ctx)
{
    (void)crate_count;
    if (op->out_crate == CRATE_INDEX_NONE) return ERR_INVALID_ARGUMENT;
    Crate *out = &crates[op->out_crate];
    if (out->capacity < 2) return ERR_BUFFER_TOO_SMALL;

    int cols = VideoGetCols();
    int rows = VideoGetRows();

    uint8_t blob[2];
    blob[0] = (uint8_t)(cols > 255 ? 255 : (cols < 0 ? 0 : cols));
    blob[1] = (uint8_t)(rows > 255 ? 255 : (rows < 0 ? 0 : rows));
    if (crate_write(out, ctx, blob, 2) != OK) return ERR_INVALID_ADDRESS;
    return OK;
}


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

static int HwRtcGetTime(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                        const OpContext *ctx)
{
    (void)crate_count;
    if (op->out_crate == CRATE_INDEX_NONE) return ERR_INVALID_ARGUMENT;
    Crate *out = &crates[op->out_crate];
    if (out->capacity < sizeof(BoxTime)) return ERR_BUFFER_TOO_SMALL;

    BoxTime t;
    rtc_get_boxtime(&t);
    if (crate_write(out, ctx, &t, sizeof(BoxTime)) != OK) return ERR_INVALID_ADDRESS;
    return OK;
}

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


static int HwCpuHalt(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                     const OpContext *ctx)
{
    (void)op; (void)crates; (void)crate_count; (void)ctx;
    hlt();
    return OK;
}


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

static int HwDiskFlush(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                       const OpContext *ctx)
{
    (void)crates; (void)crate_count; (void)ctx;
    if (op->param_size < 1) return ERR_INVALID_ARGUMENT;
    (void)op->params[0];
    return tagfs_flush_cache();
}



static int HwSystemReboot(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                          const OpContext *ctx)
{
    (void)op; (void)crates; (void)crate_count;
    debug_printf("[HardwareDeck] reboot requested by PID %u\n",
                 (ctx && ctx->proc) ? ctx->proc->pid : 0);
    system_halt(true);
    return OK;
}

static int HwSystemShutdown(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                            const OpContext *ctx)
{
    (void)op; (void)crates; (void)crate_count;
    debug_printf("[HardwareDeck] shutdown requested by PID %u\n",
                 (ctx && ctx->proc) ? ctx->proc->pid : 0);
    system_halt(false);
    return OK;
}



static xhci_controller_t *usb_named_controller(const ManifestOp *op)
{
    if (op->param_size < 1) return NULL;
    return xhci_controller_at(op->params[0]);
}

static int HwUsbReset(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                      const OpContext *ctx)
{
    (void)crates; (void)crate_count;
    xhci_controller_t *c = usb_named_controller(op);
    if (!c) return ERR_NOT_INITIALIZED;

    kprintf("[HardwareDeck] PID %u asked for the controller on %s to be put "
            "back in service\n",
            (ctx && ctx->proc) ? ctx->proc->pid : 0, c->name);

    return xhci_put_back_in_service(c) ? OK : ERR_INTERNAL;
}

static int HwUsbStart(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                      const OpContext *ctx)
{
    (void)crates; (void)crate_count; (void)ctx;
    xhci_controller_t *c = usb_named_controller(op);
    if (!c) return ERR_NOT_INITIALIZED;
    return xhci_start(c) == 0 ? OK : ERR_INTERNAL;
}

static int HwUsbPortStatus(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                           const OpContext *ctx)
{
    (void)crate_count;
    if (op->param_size < 1) return ERR_INVALID_ARGUMENT;
    if (op->out_crate == CRATE_INDEX_NONE) return ERR_INVALID_ARGUMENT;

    if (op->param_size < 2) return ERR_INVALID_ARGUMENT;
    uint8_t port = op->params[1];
    xhci_controller_t *c = usb_named_controller(op);
    if (!c || !c->initialized) return ERR_NOT_INITIALIZED;
    if (port == 0 || port > c->max_ports) return ERR_INVALID_ARGUMENT;

    Crate *out = &crates[op->out_crate];
    if (out->capacity < sizeof(uint32_t)) return ERR_BUFFER_TOO_SMALL;
    uint32_t portsc = xhci_get_port_status(c, port);

    if (crate_write(out, ctx, &portsc, sizeof(uint32_t)) != OK) return ERR_INVALID_ADDRESS;
    return OK;
}

static int HwUsbPortReset(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                          const OpContext *ctx)
{
    (void)crates; (void)crate_count; (void)ctx;
    if (op->param_size < 2) return ERR_INVALID_ARGUMENT;
    uint8_t port = op->params[1];

    xhci_controller_t *c = usb_named_controller(op);
    if (!c || !c->initialized)            return ERR_NOT_INITIALIZED;
    if (port == 0 || port > c->max_ports) return ERR_INVALID_ARGUMENT;
    if (!xhci_port_has_device(c, port))   return ERR_DEVICE_NOT_READY;

    int rc = xhci_port_begin_reset(c, port, NULL);
    return rc >= 0 ? OK : ERR_INTERNAL;
}

static int HwUsbPortQuery(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                          const OpContext *ctx)
{
    (void)crate_count;
    if (op->out_crate == CRATE_INDEX_NONE) return ERR_INVALID_ARGUMENT;
    xhci_controller_t *c = usb_named_controller(op);
    if (!c || !c->initialized) return ERR_NOT_INITIALIZED;

    Crate *out = &crates[op->out_crate];
    if (out->capacity < 5) return ERR_BUFFER_TOO_SMALL;
    uint8_t blob[5];
    blob[0] = c->max_ports;
    blob[1] = c->max_slots;
    blob[2] = c->irq_line;
    blob[3] = c->use_polling ? 1 : 0;
    blob[4] = xhci_controller_count();
    if (crate_write(out, ctx, blob, 5) != OK) return ERR_INVALID_ADDRESS;
    return OK;
}

static int HwUsbEnumDevice(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                           const OpContext *ctx)
{
    (void)crates; (void)crate_count; (void)ctx;
    if (op->param_size < 2) return ERR_INVALID_ARGUMENT;
    uint8_t port = op->params[1];
    xhci_controller_t *c = usb_named_controller(op);
    if (!c || !c->running) return ERR_NOT_INITIALIZED;

    int rc = xhci_enumerate_device(c, port);
    if (rc == 0)        return OK;
    if (rc == -2 || rc == -3) return ERR_INVALID_ARGUMENT;
    return ERR_INTERNAL;
}

static int HwUsbGetInfo(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                        const OpContext *ctx)
{
    (void)crate_count;
    if (op->param_size < 1) return ERR_INVALID_ARGUMENT;
    if (op->out_crate == CRATE_INDEX_NONE) return ERR_INVALID_ARGUMENT;

    if (op->param_size < 2) return ERR_INVALID_ARGUMENT;
    uint8_t slot_id = op->params[1];
    xhci_controller_t *c = usb_named_controller(op);
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

#if CONFIG_USB_RECOVER_PROOF
void HardwareDeckUsbRecoverProof(void)
{
    static volatile uint32_t done = 0;

    if (__atomic_load_n(&done, __ATOMIC_ACQUIRE) != 0)   return;
    if (xhci_controller_count() == 0)                    return;
    if (tagfs_get_seat() == BOARDROOM_NO_SEAT)           return;
    if (__atomic_exchange_n(&done, 1u, __ATOMIC_ACQUIRE) != 0) return;

    kprintf("[USB RECOVER TEST] begin — a controller in service and a volume "
            "on seat %u\n", tagfs_get_seat());

    int gone = 0;
    if (!OpRegistryLookup(OP_KIND(HARDWARE_DECK_ID, 0x90u))) gone++;
    if (!OpRegistryLookup(OP_KIND(HARDWARE_DECK_ID, 0x93u))) gone++;
    if (gone == 2) {
        kprintf("[USB RECOVER TEST] PASS: the two withdrawn opcodes answer to "
                "nothing\n");
    } else {
        kprintf("[USB RECOVER TEST] FAIL: a withdrawn opcode is registered "
                "again\n");
    }

    const OpRegistration *reg =
        OpRegistryLookup(OP_KIND(HARDWARE_DECK_ID, HW_USB_RESET));
    if (!reg || !reg->handler) {
        kprintf("[USB RECOVER TEST] FAILED: hw.usb.reset is not registered\n");
        return;
    }
    if (reg->security_mask != OP_AUTH_SYSTEM) {
        kprintf("[USB RECOVER TEST] FAIL: hw.usb.reset is not system-only\n");
    } else {
        kprintf("[USB RECOVER TEST] PASS: hw.usb.reset is registered, "
                "system-only\n");
    }

    struct __packed {
        ManifestOp head;
        uint8_t    params[1];
    } req;
    req.head.op_kind    = OP_KIND(HARDWARE_DECK_ID, HW_USB_RESET);
    req.head.flags      = 0;
    req.head.in_crate   = CRATE_INDEX_NONE;
    req.head.out_crate  = CRATE_INDEX_NONE;
    req.head.param_size = 1;
    req.params[0]       = 0;

    OpContext ctx;
    memset(&ctx, 0, sizeof(ctx));

    int rc = reg->handler(&req.head, NULL, 0, &ctx);

    if (rc == OK) {
        kprintf("[USB RECOVER TEST] %[S]PASSED%[D]: the controller was put back "
                "in service through hw.usb.reset\n");
    } else {
        kprintf("[USB RECOVER TEST] %[R]FAILED%[D]: hw.usb.reset answered %d\n",
                rc);
    }
}
#endif


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


#define HW_LOG_READ_HEADER  24u
#define HW_LOG_READ_CHUNK   4096u

typedef uint64_t (*LogSource)(uint64_t, void *, uint64_t, uint64_t *, uint64_t *);

static int HwLogReadFrom(LogSource source, const ManifestOp *op, Crate *crates,
                         const OpContext *ctx)
{
    if (op->out_crate == CRATE_INDEX_NONE) return ERR_INVALID_ARGUMENT;
    if (op->param_size < 8)                return ERR_INVALID_ARGUMENT;

    uint64_t from = 0;
    for (unsigned i = 0; i < 8; i++) {
        from |= (uint64_t)op->params[i] << (i * 8);
    }

    Crate *out = &crates[op->out_crate];
    if (out->capacity < HW_LOG_READ_HEADER) return ERR_BUFFER_TOO_SMALL;

    uint64_t room = out->capacity - HW_LOG_READ_HEADER;
    if (room > HW_LOG_READ_CHUNK) room = HW_LOG_READ_CHUNK;

    uint8_t *kp = crate_out_alloc(out, HW_LOG_READ_HEADER + room);
    if (!kp) return ERR_INVALID_ADDRESS;

    uint64_t oldest = 0, written = 0;
    uint64_t copied = source(from, kp + HW_LOG_READ_HEADER, room,
                             &oldest, &written);

    memcpy(kp,      &oldest,  sizeof(uint64_t));
    memcpy(kp + 8,  &written, sizeof(uint64_t));
    memcpy(kp + 16, &copied,  sizeof(uint64_t));

    int crc = crate_out_commit(out, ctx, kp, HW_LOG_READ_HEADER + copied);
    crate_buf_free(kp);
    return (crc == OK) ? OK : ERR_INVALID_ADDRESS;
}

static int HwLogPrevious(const ManifestOp *op, Crate *crates,
                         uint16_t crate_count, const OpContext *ctx)
{
    (void)crate_count;
#ifndef CONFIG_PRINTTOFILE
    (void)op; (void)crates; (void)ctx;
    return ERR_UNSUPPORTED;
#else
    return HwLogReadFrom(LogKeepPreviousRead, op, crates, ctx);
#endif
}

static int HwLogRead(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                     const OpContext *ctx)
{
    (void)crate_count;
    return HwLogReadFrom(LogRingRead, op, crates, ctx);
}


error_t HardwareDeckRegister(void)
{
    struct {
        uint16_t    opcode;
        OpHandler   handler;
        uint32_t    auth;
        const char *name;
    } table[] = {
        { HW_TIMER_GET_TICKS,    HwTimerGetTicks,    OP_AUTH_NONE,   "hw.timer.ticks"   },
        { HW_TIMER_GET_MS,       HwTimerGetMs,       OP_AUTH_NONE,   "hw.timer.ms"      },
        { HW_TIMER_GET_FREQ,     HwTimerGetFreq,     OP_AUTH_NONE,   "hw.timer.freq"    },
        { HW_RTC_GET_TIME,       HwRtcGetTime,       OP_AUTH_NONE,   "hw.rtc.time"      },
        { HW_RTC_GET_UNIX64,     HwRtcGetUnix64,     OP_AUTH_NONE,   "hw.rtc.unix64"    },
        { HW_RTC_GET_UPTIME,     HwRtcGetUptime,     OP_AUTH_NONE,   "hw.rtc.uptime"    },
        { HW_PORT_INB,           HwPortInb,          OP_AUTH_SYSTEM, "hw.port.inb"      },
        { HW_PORT_OUTB,          HwPortOutb,         OP_AUTH_SYSTEM, "hw.port.outb"     },
        { HW_PORT_INW,           HwPortInw,          OP_AUTH_SYSTEM, "hw.port.inw"      },
        { HW_PORT_OUTW,          HwPortOutw,         OP_AUTH_SYSTEM, "hw.port.outw"     },
        { HW_PORT_INL,           HwPortInl,          OP_AUTH_SYSTEM, "hw.port.inl"      },
        { HW_PORT_OUTL,          HwPortOutl,         OP_AUTH_SYSTEM, "hw.port.outl"     },
        { HW_IRQ_ENABLE,         HwIrqEnable,        OP_AUTH_SYSTEM, "hw.irq.enable"    },
        { HW_IRQ_DISABLE,        HwIrqDisable,       OP_AUTH_SYSTEM, "hw.irq.disable"   },
        { HW_IRQ_GET_ISR,        HwIrqGetIsr,        OP_AUTH_SYSTEM, "hw.irq.isr"       },
        { HW_IRQ_GET_IRR,        HwIrqGetIrr,        OP_AUTH_SYSTEM, "hw.irq.irr"       },
        { HW_IRQ_SEND_EOI,       HwIrqSendEoi,       OP_AUTH_SYSTEM, "hw.irq.eoi"       },
        { HW_CPU_HALT,           HwCpuHalt,          OP_AUTH_SYSTEM, "hw.cpu.halt"      },
        { HW_DISK_INFO,          HwDiskInfo,         OP_AUTH_NONE,   "hw.disk.info"     },
        { HW_DISK_FLUSH,         HwDiskFlush,        OP_AUTH_NONE,   "hw.disk.flush"    },
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
        { HW_VGA_PAINT,          HwVgaPaint,         OP_AUTH_NONE,   "hw.vga.paint"     },
        { HW_VGA_STEP_CURSOR,    HwVgaStepCursor,    OP_AUTH_NONE,   "hw.vga.step"      },
        { HW_SYSTEM_REBOOT,      HwSystemReboot,     OP_AUTH_SYSTEM, "hw.system.reboot"  },
        { HW_SYSTEM_SHUTDOWN,    HwSystemShutdown,   OP_AUTH_SYSTEM, "hw.system.shutdown"},
        { HW_DEBUG_PRINT,        HwDebugPrint,       OP_AUTH_NONE,   "hw.debug.print"    },
        { HW_LOG_READ,           HwLogRead,          OP_AUTH_UTILITY,"hw.log.read"       },
        { HW_LOG_PREVIOUS,       HwLogPrevious,      OP_AUTH_UTILITY,"hw.log.previous"   },
        { HW_USB_RESET,          HwUsbReset,         OP_AUTH_SYSTEM, "hw.usb.reset"     },
        { HW_USB_START,          HwUsbStart,         OP_AUTH_SYSTEM, "hw.usb.start"     },
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