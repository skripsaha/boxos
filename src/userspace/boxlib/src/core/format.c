#include "box/io.h"
#include "box/string.h"
#include "box/convert.h"

static void fmt_htoa64(unsigned long value, char* buf) {
    const char* digits = "0123456789abcdef";
    if (value == 0) { buf[0] = '0'; buf[1] = '\0'; return; }
    char tmp[17];
    int i = 0;
    while (value > 0) { tmp[i++] = digits[value & 0xF]; value >>= 4; }
    int j = 0;
    while (i > 0) buf[j++] = tmp[--i];
    buf[j] = '\0';
}

int printf(const char* fmt, ...) {
    if (!fmt) return -1;

    va_list args;
    va_start(args, fmt);

    char out[512];
    int pos = 0;
    char numbuf[20];

    while (*fmt && pos < 510) {
        if (*fmt != '%') {
            out[pos++] = *fmt++;
            continue;
        }
        fmt++;
        switch (*fmt) {
        case 's': {
            const char* s = va_arg(args, const char*);
            if (!s) s = "(null)";
            while (*s && pos < 510) out[pos++] = *s++;
            break;
        }
        case 'd': {
            int v = va_arg(args, int);
            to_str(v, numbuf, sizeof(numbuf));
            for (int i = 0; numbuf[i] && pos < 510; i++) out[pos++] = numbuf[i];
            break;
        }
        case 'u': {
            unsigned int v = va_arg(args, unsigned int);
            uint_to_str(v, numbuf, sizeof(numbuf));
            for (int i = 0; numbuf[i] && pos < 510; i++) out[pos++] = numbuf[i];
            break;
        }
        case 'x': {
            unsigned int v = va_arg(args, unsigned int);
            to_hex(v, numbuf, sizeof(numbuf));
            for (int i = 0; numbuf[i] && pos < 510; i++) out[pos++] = numbuf[i];
            break;
        }
        case 'X': {
            unsigned int v = va_arg(args, unsigned int);
            to_hex(v, numbuf, sizeof(numbuf));
            for (int i = 0; numbuf[i] && pos < 510; i++) out[pos++] = numbuf[i];
            break;
        }
        case 'c': {
            char c = (char)va_arg(args, int);
            out[pos++] = c;
            break;
        }
        case 'p': {
            unsigned long v = va_arg(args, unsigned long);
            if (pos < 508) { out[pos++] = '0'; out[pos++] = 'x'; }
            fmt_htoa64(v, numbuf);
            for (int i = 0; numbuf[i] && pos < 510; i++) out[pos++] = numbuf[i];
            break;
        }
        case '%':
            out[pos++] = '%';
            break;
        case '\0':
            goto done;
        default:
            out[pos++] = '%';
            if (pos < 510) out[pos++] = *fmt;
            break;
        }
        fmt++;
    }
done:
    out[pos] = '\0';
    va_end(args);

    print(out);
    return pos;
}
