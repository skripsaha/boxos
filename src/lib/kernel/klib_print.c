/* klib_print.c — formatted output for the kernel.
 *
 * Single source of truth for `%`-format parsing lives in `kvformat()`, which
 * runs against a `kfmt_ops_t` callback set. Two callback sets are wired:
 *   SCREEN_OPS  emits to serial + framebuffer with colour and cursor state
 *   BUF_OPS     emits into a bounded char buffer
 *
 * Why callback-driven rather than two parsers: the legacy split parser had
 * silently divergent feature sets — kvsnprintf knew %d/%u/%x/%s/%c only,
 * kprintf knew the full %lx/%p/%zu/%[X] set, and `panic()` calls into
 * kvsnprintf with format strings that include %lx and %p (then kprintf the
 * result). On the legacy code the panic banner contained literal "%lx" in
 * place of an address — fatal for real-HW post-mortem diagnostics. Now both
 * surfaces walk the same parser so feature parity is structural.
 *
 * Locking: kprintf serialises behind `g_kprintf_lock` (IRQ-safe spinlock),
 * which `console_lock_acquire/release` expose so other producers of the
 * framebuffer (HwVga* writes from interrupt handlers) can serialise too.
 * The lock is BSS-zero before mem_init() runs spinlock_init(); BSS-zero
 * happens to mean unlocked for the current spinlock_t layout. */
#include "klib.h"
#include "video.h"
#include "serial.h"

/* Module-local state — neither escapes outside this TU. */
static spinlock_t g_kprintf_lock;
static uint8_t    current_attr = VIDEO_ATTR_DEFAULT;

void console_lock_acquire(void) { spin_lock(&g_kprintf_lock); }
void console_lock_release(void) { spin_unlock(&g_kprintf_lock); }

/* mem_init() runs before any kprintf-class call that needs the lock to be
 * formally initialised, so expose a hook it can drive. */
void klib_print_lock_init(void)
{
    spinlock_init(&g_kprintf_lock);
}

int kputnl(void)
{
    VideoClearToEol();
    VideoPrintNewline();
    VideoUpdateCursor();
    return 1;
}

void kputchar(char c)
{
    if (c == '\n')
    {
        serial_putchar('\r');
        serial_putchar('\n');
    }
    else
    {
        serial_putchar(c);
    }

    if (c == '\n')
    {
        kputnl();
    }
    else if (c == '\r')
    {
        VideoSetCursor(0, VideoGetCursorY());
    }
    else if (c == '\b')
    {
        int x = VideoGetCursorX();
        if (x > 0)
        {
            VideoSetCursor(x - 1, VideoGetCursorY());
            VideoUpdateCursor();
        }
    }
    else
    {
        VideoPrintChar(c, VideoGetColor());
        VideoUpdateCursor();
    }
}

/* ============================================================================
 *  Unified formatter
 * ========================================================================== */

typedef int  (*kfmt_emit_t)(void *ctx, char c);
typedef void (*kfmt_attr_t)(void *ctx, uint8_t attr);
typedef void (*kfmt_cursor_t)(void *ctx, int x, int y);

typedef struct
{
    kfmt_emit_t   emit_char;
    kfmt_attr_t   set_attr;    /* NULL → attribute escapes are no-ops */
    kfmt_cursor_t set_cursor;  /* NULL → cursor escapes are no-ops    */
} kfmt_ops_t;

static int kvformat(const kfmt_ops_t *ops, void *ctx,
                    const char *fmt, va_list args)
{
    int count = 0;

    while (*fmt)
    {
        if (*fmt != '%')
        {
            if (!ops->emit_char(ctx, *fmt))
                return count;
            ++count;
            ++fmt;
            continue;
        }

        ++fmt;  /* past '%' */

        /* BoxOS extensions: %[X] for colour/cursor/codepoint. Buffer-mode
         * emitters supply NULL set_attr/set_cursor so these escapes drop
         * silently when formatting into a string. */
        if (*fmt == '[')
        {
            ++fmt;
            switch (*fmt)
            {
            case 'E':
                if (ops->set_attr) ops->set_attr(ctx, VIDEO_ATTR_ERROR);
                break;
            case 'S':
                if (ops->set_attr) ops->set_attr(ctx, VIDEO_ATTR_SUCCESS);
                break;
            case 'H':
                if (ops->set_attr) ops->set_attr(ctx, VIDEO_ATTR_HINT);
                break;
            case 'D':
                if (ops->set_attr) ops->set_attr(ctx, VIDEO_ATTR_DEFAULT);
                break;
            case 'W':
                if (ops->set_attr) ops->set_attr(ctx, VIDEO_ATTR_WARNING);
                break;
            case 'P':
            {
                int x = va_arg(args, int);
                int y = va_arg(args, int);
                if (ops->set_cursor) ops->set_cursor(ctx, x, y);
                break;
            }
            case 'U':
            {
                uint32_t codepoint = va_arg(args, uint32_t);
                char enc[4];
                int n = utf8_encode(codepoint, enc);
                for (int i = 0; i < n; ++i)
                {
                    if (!ops->emit_char(ctx, enc[i])) return count;
                    ++count;
                }
                break;
            }
            default:
                break;
            }
            while (*fmt && *fmt != ']') ++fmt;
            if (*fmt == ']') ++fmt;
            continue;
        }

        int left_align = 0, pad_zero = 0;
        if (*fmt == '-')      { left_align = 1; ++fmt; }
        else if (*fmt == '0') { pad_zero = 1;   ++fmt; }

        int pad_width = 0;
        while (*fmt >= '0' && *fmt <= '9')
        {
            pad_width = pad_width * 10 + (*fmt - '0');
            ++fmt;
        }

        int longflag = 0, longlongflag = 0, sizeflag = 0;
        if (*fmt == 'z')
        {
            sizeflag = 1;
            ++fmt;
        }
        else
        {
            while (*fmt == 'l')
            {
                ++fmt;
                if (longflag) { longlongflag = 1; longflag = 0; }
                else          { longflag = 1; }
            }
        }

        char num_buf[32];
        const char *str = NULL;
        size_t str_len = 0;
        int negative = 0;
        int hex_upper = 0;
        int emit_0x = 0;

        switch (*fmt)
        {
        case 'd':
        case 'i':
        {
            long long v;
            if      (sizeflag)     v = (long long)va_arg(args, size_t);
            else if (longlongflag) v = va_arg(args, long long);
            else if (longflag)     v = va_arg(args, long);
            else                   v = va_arg(args, int);
            if (v < 0) { negative = 1; v = -v; }
            utoa64((uint64_t)v, num_buf, 10);
            str = num_buf;
            str_len = strlen(num_buf);
            break;
        }
        case 'u':
        {
            unsigned long long v;
            if      (sizeflag)     v = (unsigned long long)va_arg(args, size_t);
            else if (longlongflag) v = va_arg(args, unsigned long long);
            else if (longflag)     v = va_arg(args, unsigned long);
            else                   v = va_arg(args, unsigned int);
            utoa64((uint64_t)v, num_buf, 10);
            str = num_buf;
            str_len = strlen(num_buf);
            break;
        }
        case 'X':
            hex_upper = 1;
            /* fallthrough */
        case 'x':
        {
            unsigned long long v;
            if      (sizeflag)     v = (unsigned long long)va_arg(args, size_t);
            else if (longlongflag) v = va_arg(args, unsigned long long);
            else if (longflag)     v = va_arg(args, unsigned long);
            else                   v = va_arg(args, unsigned int);
            utoa64((uint64_t)v, num_buf, 16);
            if (hex_upper)
                for (char *p = num_buf; *p; ++p) *p = (char)toupper(*p);
            str = num_buf;
            str_len = strlen(num_buf);
            break;
        }
        case 'p':
        {
            void *p = va_arg(args, void *);
            utoa64((uint64_t)(uintptr_t)p, num_buf, 16);
            str = num_buf;
            str_len = strlen(num_buf);
            emit_0x    = 1;
            pad_width  = 16;
            pad_zero   = 1;
            left_align = 0;
            break;
        }
        case 's':
        {
            str = va_arg(args, const char *);
            if (!str) str = "(null)";
            str_len = strlen(str);
            break;
        }
        case 'c':
        {
            num_buf[0] = (char)va_arg(args, int);
            num_buf[1] = '\0';
            str = num_buf;
            str_len = 1;
            break;
        }
        case '%':
        {
            num_buf[0] = '%';
            num_buf[1] = '\0';
            str = num_buf;
            str_len = 1;
            break;
        }
        case 'f':
        {
            /* Kernel: -mno-sse, no FPU state in printf path. */
            str = "<no-float>";
            str_len = strlen(str);
            break;
        }
        default:
        {
            if (!ops->emit_char(ctx, '%')) return count;
            ++count;
            if (*fmt)
            {
                if (!ops->emit_char(ctx, *fmt)) return count;
                ++count;
                ++fmt;
            }
            continue;
        }
        }

        ++fmt;

        size_t prefix_len = (negative ? 1 : 0) + (emit_0x ? 2 : 0);
        size_t total_len  = prefix_len + str_len;
        int pad = ((int)total_len < pad_width) ? pad_width - (int)total_len : 0;

#define EMIT(c) do { if (!ops->emit_char(ctx, (c))) return count; ++count; } while (0)

        if (left_align)
        {
            if (negative) EMIT('-');
            if (emit_0x)  { EMIT('0'); EMIT('x'); }
            for (size_t i = 0; i < str_len; ++i) EMIT(str[i]);
            for (int i = 0; i < pad; ++i)        EMIT(' ');
        }
        else if (pad_zero)
        {
            if (negative) EMIT('-');
            if (emit_0x)  { EMIT('0'); EMIT('x'); }
            for (int i = 0; i < pad; ++i)        EMIT('0');
            for (size_t i = 0; i < str_len; ++i) EMIT(str[i]);
        }
        else
        {
            for (int i = 0; i < pad; ++i)        EMIT(' ');
            if (negative) EMIT('-');
            if (emit_0x)  { EMIT('0'); EMIT('x'); }
            for (size_t i = 0; i < str_len; ++i) EMIT(str[i]);
        }

#undef EMIT
    }

    return count;
}

/* --- Screen ops: drives kputchar + colour/cursor state for kprintf. --- */
static int screen_emit(void *ctx, char c)
{
    (void)ctx;
    kputchar(c);
    return 1;
}
static void screen_set_attr(void *ctx, uint8_t attr)
{
    (void)ctx;
    current_attr = attr;
}
static void screen_set_cursor(void *ctx, int x, int y)
{
    (void)ctx;
    VideoSetCursor(x, y);
}
static const kfmt_ops_t SCREEN_OPS = {
    .emit_char  = screen_emit,
    .set_attr   = screen_set_attr,
    .set_cursor = screen_set_cursor,
};

/* --- Buffer ops: drives a bounded char buffer for ksnprintf/kvsnprintf. --- */
typedef struct { char *buf; size_t pos; size_t size; } kfmt_buf_ctx_t;

static int buf_emit(void *ctx, char c)
{
    kfmt_buf_ctx_t *b = (kfmt_buf_ctx_t *)ctx;
    if (b->pos + 1 < b->size)
    {
        b->buf[b->pos++] = c;
        return 1;
    }
    return 0;  /* buffer full — parser will stop */
}
static const kfmt_ops_t BUF_OPS = {
    .emit_char  = buf_emit,
    .set_attr   = NULL,
    .set_cursor = NULL,
};

int kprintf(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    spin_lock(&g_kprintf_lock);
    int n = kvformat(&SCREEN_OPS, NULL, format, args);
    spin_unlock(&g_kprintf_lock);
    va_end(args);
    return n;
}

int kvsnprintf(char *buf, size_t size, const char *fmt, va_list args)
{
    if (!buf || size == 0) return 0;
    kfmt_buf_ctx_t ctx = { buf, 0, size };
    (void)kvformat(&BUF_OPS, &ctx, fmt, args);
    buf[ctx.pos] = '\0';
    return (int)ctx.pos;
}

int ksnprintf(char *buf, size_t size, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    int n = kvsnprintf(buf, size, fmt, args);
    va_end(args);
    return n;
}

__attribute__((noreturn)) void panic(const char *message, ...)
{
    /* Defensive lock break BEFORE the first kprintf — see klib.h
     * console-lock invariant. spin_force_release skips the saved-flags
     * restore; that is fine here because we never return, and IPI_PANIC
     * (sent from exception_handler) halts peer cores before they ever
     * spin_unlock again. */
    spin_force_release(&g_kprintf_lock);

    asm volatile("cli");

    va_list args;
    va_start(args, message);

    kprintf("\n%[E]KERNEL PANIC:%[D] ");

    char temp_buf[512];
    kvsnprintf(temp_buf, sizeof(temp_buf), message, args);
    kprintf("%s", temp_buf);

    kprintf("\n\nDebug info:");
    kprintf("\n- Stack pointer: %p", __builtin_frame_address(0));
    kprintf("\n- Instruction pointer: %p", __builtin_return_address(0));

    va_end(args);

    while (1)
        asm volatile("hlt");
}
