#include "box/debug.h"
#include "box/defs.h"
#include "box/core/manifest.h"
#include "box/string.h"
#include "box/error.h"
#include "boxos_decks.h"

#define HW_DEBUG_PRINT  0x82

static void uint64_to_dec(uint64_t v, char *buf, int *len)
{
    char tmp[24];
    int n = 0;
    if (v == 0) { tmp[n++] = '0'; }
    else { while (v) { tmp[n++] = (char)('0' + v % 10); v /= 10; } }
    for (int i = n - 1; i >= 0; i--) buf[(*len)++] = tmp[i];
}

static void int64_to_dec(int64_t v, char *buf, int *len)
{
    if (v < 0) { buf[(*len)++] = '-'; uint64_to_dec((uint64_t)-v, buf, len); }
    else        uint64_to_dec((uint64_t)v, buf, len);
}

static void uint64_to_hex(uint64_t v, char *buf, int *len)
{
    static const char hex[] = "0123456789abcdef";
    char tmp[16]; int n = 0;
    if (v == 0) { tmp[n++] = '0'; }
    else { while (v) { tmp[n++] = hex[v & 0xf]; v >>= 4; } }
    for (int i = n - 1; i >= 0; i--) buf[(*len)++] = tmp[i];
}

/* Minimal vsnprintf supporting %s %d %u %x %lu %lx — no width/prec. */
static int debug_vsnprintf(char *dst, int cap, const char *fmt, va_list ap)
{
    int pos = 0;
#define OUT(c) do { if (pos < cap - 1) dst[pos++] = (c); } while(0)

    while (*fmt) {
        if (*fmt != '%') { OUT(*fmt++); continue; }
        fmt++;
        int is_long = 0;
        if (*fmt == 'l') { is_long = 1; fmt++; }

        switch (*fmt) {
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (!s) s = "(null)";
            while (*s && pos < cap - 1) dst[pos++] = *s++;
            break;
        }
        case 'd': {
            int64_t v = is_long ? va_arg(ap, long) : (int64_t)va_arg(ap, int);
            char tmp[24]; int tl = 0;
            int64_to_dec(v, tmp, &tl);
            for (int i = 0; i < tl && pos < cap - 1; i++) dst[pos++] = tmp[i];
            break;
        }
        case 'u': {
            uint64_t v = is_long ? va_arg(ap, unsigned long) : (uint64_t)va_arg(ap, unsigned int);
            char tmp[24]; int tl = 0;
            uint64_to_dec(v, tmp, &tl);
            for (int i = 0; i < tl && pos < cap - 1; i++) dst[pos++] = tmp[i];
            break;
        }
        case 'x': {
            uint64_t v = is_long ? va_arg(ap, unsigned long) : (uint64_t)va_arg(ap, unsigned int);
            char tmp[24]; int tl = 0;
            uint64_to_hex(v, tmp, &tl);
            for (int i = 0; i < tl && pos < cap - 1; i++) dst[pos++] = tmp[i];
            break;
        }
        case '%': OUT('%'); break;
        default:  OUT('%'); OUT(*fmt); break;
        }
        fmt++;
    }
#undef OUT
    dst[pos] = '\0';
    return pos;
}

void kdbg(const char *msg)
{
    if (!msg) return;
    MfCall1(DECK_HARDWARE, HW_DEBUG_PRINT,
            NULL, 0,
            msg, (uint32_t)(strlen(msg) + 1),
            NULL, 0, NULL,
            100, NULL);
}

int kdbg_print(const char *fmt, ...)
{
    if (!fmt) return -ERR_INVALID_ARGUMENT;

    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    int len = debug_vsnprintf(buf, (int)sizeof(buf), fmt, ap);
    va_end(ap);

    kdbg(buf);
    return len;
}
