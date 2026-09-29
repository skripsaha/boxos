/* klib_conv.c — character classifiers, integer-to-string conversion,
 * UTF-8 encode/decode, tag matching, busy-wait delay.
 *
 * No state beyond a base-36 digit table (read-only). All routines are
 * pure functions of their inputs except `delay` (consults TSC / PIT). */
#include "klib.h"
#include "cpu_calibrate.h"  /* cpu_tsc_is_calibrated, cpu_ms_to_tsc */
#include "atomics.h"        /* rdtsc */
#include "pit.h"            /* pit_delay_busy */

static const char digits[] = "0123456789abcdefghijklmnopqrstuvwxyz";

/* ---- Character classifiers ---------------------------------------------- */

int toupper(int c)
{
    if (c >= 'a' && c <= 'z')
        return c - ('a' - 'A');
    return c;
}

int tolower(int c)
{
    if (c >= 'A' && c <= 'Z')
        return c + ('a' - 'A');
    return c;
}

bool isdigit(int c)
{
    return c >= '0' && c <= '9';
}

bool isalpha(int c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

bool isalnum(int c)
{
    return isalpha(c) || isdigit(c);
}

bool isspace(int c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

/* ---- UTF-8 -------------------------------------------------------------- */

int utf8_encode(uint32_t codepoint, char out[4])
{
    if (codepoint <= 0x7F)
    {
        out[0] = (char)codepoint;
        return 1;
    }
    if (codepoint <= 0x7FF)
    {
        out[0] = (char)(0xC0 | ((codepoint >> 6) & 0x1F));
        out[1] = (char)(0x80 | (codepoint & 0x3F));
        return 2;
    }
    if (codepoint <= 0xFFFF)
    {
        out[0] = (char)(0xE0 | ((codepoint >> 12) & 0x0F));
        out[1] = (char)(0x80 | ((codepoint >> 6) & 0x3F));
        out[2] = (char)(0x80 | (codepoint & 0x3F));
        return 3;
    }
    if (codepoint <= 0x10FFFF)
    {
        out[0] = (char)(0xF0 | ((codepoint >> 18) & 0x07));
        out[1] = (char)(0x80 | ((codepoint >> 12) & 0x3F));
        out[2] = (char)(0x80 | ((codepoint >> 6) & 0x3F));
        out[3] = (char)(0x80 | (codepoint & 0x3F));
        return 4;
    }
    return 0;
}

int utf8_decode(const char *utf8, uint32_t *codepoint)
{
    if (!utf8 || !codepoint)
        return 0;

    uint8_t first = (uint8_t)utf8[0];

    if (first <= 0x7F)
    {
        *codepoint = first;
        return 1;
    }
    if ((first & 0xE0) == 0xC0)
    {
        *codepoint = ((first & 0x1F) << 6) | ((uint8_t)utf8[1] & 0x3F);
        return 2;
    }
    if ((first & 0xF0) == 0xE0)
    {
        *codepoint = ((first & 0x0F) << 12) |
                     (((uint8_t)utf8[1] & 0x3F) << 6) |
                     ((uint8_t)utf8[2] & 0x3F);
        return 3;
    }
    if ((first & 0xF8) == 0xF0)
    {
        *codepoint = ((first & 0x07) << 18) |
                     (((uint8_t)utf8[1] & 0x3F) << 12) |
                     (((uint8_t)utf8[2] & 0x3F) << 6) |
                     ((uint8_t)utf8[3] & 0x3F);
        return 4;
    }

    return 0;
}

/* ---- Integer-to-string (variants by signedness and width) --------------- */

char *itoa(int value, char *str, int base)
{
    if (!str || base < 2 || base > 36)
    {
        if (str)
            *str = '\0';
        return str;
    }

    char *orig = str;
    bool negative = false;

    if (value < 0 && base == 10)
    {
        negative = true;
        value = -value;
    }

    do
    {
        *str++ = digits[value % base];
        value /= base;
    } while (value);

    if (negative)
        *str++ = '-';
    *str = '\0';
    return reverse_str(orig);
}

char *utoa(unsigned int value, char *str, int base)
{
    if (!str || base < 2 || base > 36)
    {
        if (str)
            *str = '\0';
        return str;
    }

    char *orig = str;

    do
    {
        *str++ = digits[value % base];
        value /= base;
    } while (value);

    *str = '\0';
    return reverse_str(orig);
}

char *itoa64(int64_t value, char *str, int base)
{
    if (!str || base < 2 || base > 36)
    {
        if (str)
            *str = '\0';
        return NULL;
    }

    char *orig = str;

    if (value == 0)
    {
        *str++ = '0';
        *str = '\0';
        return orig;
    }

    bool negative = false;
    if (value < 0 && base == 10)
    {
        negative = true;
        value = -value;
    }

    char *ptr = str;
    do
    {
        *ptr++ = digits[value % base];
        value /= base;
    } while (value);

    if (negative)
        *ptr++ = '-';
    *ptr = '\0';

    reverse_range(str, ptr - 1);
    return orig;
}

char *utoa64(uint64_t value, char *str, int base)
{
    if (!str || base < 2 || base > 36)
    {
        if (str)
            *str = '\0';
        return str;
    }

    char *ptr = str, *start = str;

    if (value == 0)
    {
        *ptr++ = '0';
        *ptr = '\0';
        return str;
    }

    while (value)
    {
        *ptr++ = digits[value % base];
        value /= base;
    }
    *ptr = '\0';

    for (char *a = start, *b = ptr - 1; a < b; ++a, --b)
    {
        char t = *a;
        *a = *b;
        *b = t;
    }
    return str;
}

int itoa_s(int value, char *str, size_t size, int base)
{
    if (!str || size == 0)
        return -1;

    char buf[33];
    char *result = itoa(value, buf, base);
    if (!result)
        return -1;

    size_t len = strlen(buf);
    if (len >= size)
    {
        str[0] = '\0';
        return -1;
    }

    strcpy(str, buf);
    return 0;
}

int itoa64_s(int64_t value, char *str, size_t size, int base)
{
    if (!str || size == 0)
        return -1;

    char buf[65];
    char *result = itoa64(value, buf, base);
    if (!result)
    {
        str[0] = '\0';
        return -1;
    }

    size_t len = strlen(buf);
    if (len >= size)
    {
        str[0] = '\0';
        return -2;
    }

    strcpy(str, buf);
    return 0;
}

int utoa64_s(uint64_t value, char *str, size_t size, int base)
{
    if (!str || size == 0)
        return -1;

    char buf[65];
    char *result = utoa64(value, buf, base);
    if (!result)
    {
        str[0] = '\0';
        return -1;
    }

    size_t len = strlen(buf);
    if (len >= size)
    {
        str[0] = '\0';
        return -2;
    }

    strcpy(str, buf);
    return 0;
}

char *ltoa(long value, char *str, int base)        { return itoa64((int64_t)value, str, base); }
char *ultoa(unsigned long value, char *str, int base)  { return utoa64((uint64_t)value, str, base); }
char *lltoa(long long value, char *str, int base)      { return itoa64((int64_t)value, str, base); }
char *ulltoa(unsigned long long value, char *str, int base) { return utoa64((uint64_t)value, str, base); }

/* ---- String-to-integer -------------------------------------------------- */

int atoi(const char *str)
{
    int result = 0;
    int sign = 1;

    while (isspace(*str))
        str++;

    if (*str == '-')
    {
        sign = -1;
        str++;
    }
    else if (*str == '+')
    {
        str++;
    }

    while (isdigit(*str))
    {
        result = result * 10 + (*str - '0');
        str++;
    }

    return sign * result;
}

long atol(const char *str)
{
    long result = 0;
    long sign = 1;

    while (isspace(*str))
        str++;

    if (*str == '-')
    {
        sign = -1;
        str++;
    }
    else if (*str == '+')
    {
        str++;
    }

    while (isdigit(*str))
    {
        result = result * 10 + (*str - '0');
        str++;
    }

    return sign * result;
}

long long atoll(const char *str)
{
    long long result = 0;
    long long sign = 1;

    while (isspace(*str))
        str++;

    if (*str == '-')
    {
        sign = -1;
        str++;
    }
    else if (*str == '+')
    {
        str++;
    }

    while (isdigit(*str))
    {
        result = result * 10 + (*str - '0');
        str++;
    }

    return sign * result;
}

/* ---- Time-source-aware delay ------------------------------------------- */

void delay(uint32_t milliseconds)
{
    if (cpu_tsc_is_calibrated())
    {
        uint64_t deadline = rdtsc() + cpu_ms_to_tsc(milliseconds);
        while (rdtsc() < deadline)
            asm volatile("pause");
    }
    else
    {
        /* Pre-calibration fallback: PIT busy-wait. */
        pit_delay_busy(milliseconds);
    }
}
