#include "klib.h"
#include "klib_logring.h"
#include "video.h"
#include "canvas.h"
#include "serial.h"
#include "atomics.h"
#include "cpu_calibrate.h"

static spinlock_t g_kprintf_lock;

void console_lock_acquire(void) { spin_lock(&g_kprintf_lock); }
void console_lock_release(void) { spin_unlock(&g_kprintf_lock); }

void klib_print_lock_init(void)
{
    spinlock_init(&g_kprintf_lock);
    LogRingLockInit();
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
    LogRingPut(c);

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
        VideoPrintCharCur(c);
        VideoUpdateCursor();
    }
}


typedef int  (*kfmt_emit_t)(void *ctx, char c);
typedef void (*kfmt_attr_t)(void *ctx, uint8_t attr);
typedef void (*kfmt_cursor_t)(void *ctx, int x, int y);

typedef struct
{
    kfmt_emit_t   emit_char;
    kfmt_attr_t   set_attr;
    kfmt_cursor_t set_cursor;
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

        ++fmt;

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

        int precision = -1;
        if (*fmt == '.')
        {
            ++fmt;
            precision = 0;
            if (*fmt == '*')
            {
                precision = va_arg(args, int);
                if (precision < 0) precision = -1;
                ++fmt;
            }
            else
            {
                while (*fmt >= '0' && *fmt <= '9')
                {
                    precision = precision * 10 + (*fmt - '0');
                    ++fmt;
                }
            }
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
            if (precision >= 0)
            {
                str_len = 0;
                while (str_len < (size_t)precision && str[str_len] != '\0')
                    ++str_len;
            }
            else
            {
                str_len = strlen(str);
            }
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

static int screen_emit(void *ctx, char c)
{
    (void)ctx;
    kputchar(c);
    return 1;
}
static void screen_set_attr(void *ctx, uint8_t attr)
{
    (void)ctx;
    VideoSetColor(attr);
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

typedef struct { char *buf; size_t pos; size_t size; } kfmt_buf_ctx_t;

static int buf_emit(void *ctx, char c)
{
    kfmt_buf_ctx_t *b = (kfmt_buf_ctx_t *)ctx;
    if (b->pos + 1 < b->size)
    {
        b->buf[b->pos++] = c;
        return 1;
    }
    return 0;
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
    VideoBatchBegin();
    int n = kvformat(&SCREEN_OPS, NULL, format, args);
    VideoBatchEnd();
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
    spin_force_release(&g_kprintf_lock);
    CanvasForceReset();

    asm volatile("cli");

    va_list args;
    va_start(args, message);

    kprintf("\n%[E]KERNEL PANIC:%[D] ");

    char temp_buf[512];
    kvsnprintf(temp_buf, sizeof(temp_buf), message, args);
    kprintf("%s", temp_buf);

    kprintf("\n\nDebug info:");
    kprintf("\n- Stack pointer: %p", __builtin_frame_address(0));
    kprintf("\n- Instruction pointer: %p\n", __builtin_return_address(0));

    va_end(args);

    WireForceRelease();
    WireDrain();

    while (1)
        asm volatile("hlt");
}
