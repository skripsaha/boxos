/* klib_str.c — string and memory primitives for the kernel.
 *
 * mem* are tuned for real hardware:
 *   - When CPUID.07H:0.EBX[9] (ERMS) reports support, the bulk path uses
 *     `rep stosb` / `rep movsb`. Intel SDM Vol 1 §7.3.9.4 documents these
 *     as the fastest sequence for n >= a small threshold on Ivy Bridge
 *     and newer (FSRM — bit EDX[4] — extends that down to very small n).
 *   - Below the threshold the scalar 8-byte path stays in service. The
 *     legacy scalar code also remains the fallback for early-boot calls
 *     that happen before cpu_detect_features() has populated g_cpu_caps
 *     (the struct is BSS-zero, so has_erms = false → scalar). */
#include "klib.h"
#include "cpuid.h"   /* g_cpu_caps for has_erms / has_fsrm */

/* Bulk-path threshold. Below this, scalar wins on every microarchitecture
 * because rep movsb/rep stosb pays a fixed setup cost; ERMS amortises only
 * over larger n. 64 bytes is the conventional break-even on Ivy Bridge
 * through Sapphire Rapids. With FSRM the threshold can be 0 — handled by
 * the `g_cpu_caps.has_fsrm` short-circuit below. */
#define KLIB_ERMS_THRESHOLD  64

size_t strlen(const char *s)
{
    size_t len = 0;
    while (*s++)
        len++;
    return len;
}

size_t strnlen(const char *s, size_t maxlen)
{
    size_t len = 0;
    while (len < maxlen && *s++)
        len++;
    return len;
}

char *strcpy(char *dest, const char *src)
{
    char *ret = dest;
    while ((*dest++ = *src++))
        ;
    return ret;
}

char *strncpy(char *dest, const char *src, size_t n)
{
    size_t i;
    for (i = 0; i < n && src[i] != '\0'; i++)
        dest[i] = src[i];
    for (; i < n; i++)
        dest[i] = '\0';
    return dest;
}

int strcmp(const char *s1, const char *s2)
{
    while (*s1 && (*s1 == *s2))
        s1++, s2++;
    return *(const unsigned char *)s1 - *(const unsigned char *)s2;
}

int strncmp(const char *s1, const char *s2, size_t n)
{
    if (n == 0)
        return 0;
    while (n > 0 && *s1 && (*s1 == *s2))
    {
        s1++;
        s2++;
        n--;
    }
    return (n == 0) ? 0 : (*(unsigned char *)s1 - *(unsigned char *)s2);
}

char *strchr(const char *s, int c)
{
    while (*s != (char)c && *s)
        s++;
    return (*s == (char)c) ? (char *)s : NULL;
}

char *strrchr(const char *s, int c)
{
    const char *last = NULL;
    while (*s)
    {
        if (*s == (char)c)
            last = s;
        s++;
    }
    return (char *)last;
}

char *strstr(const char *haystack, const char *needle)
{
    size_t needle_len = strlen(needle);
    if (!needle_len)
        return (char *)haystack;

    while (*haystack)
    {
        if (*haystack == *needle)
        {
            if (!strncmp(haystack, needle, needle_len))
                return (char *)haystack;
        }
        haystack++;
    }
    return NULL;
}

char *strcat(char *dest, const char *src)
{
    char *ret = dest;
    while (*dest)
        dest++;
    while ((*dest++ = *src++))
        ;
    return ret;
}

char *strncat(char *dest, const char *src, size_t n)
{
    char *ret = dest;
    while (*dest)
        dest++;
    size_t i;
    for (i = 0; i < n && src[i] != '\0'; i++)
        dest[i] = src[i];
    dest[i] = '\0';
    return ret;
}

/* strtok() removed: a kernel-wide static `strtok_saveptr` is not AMP-safe.
 * Callers must use strtok_r() with a local or per-task saveptr. */
char *strtok_r(char *str, const char *delim, char **saveptr)
{
    if (str)
        *saveptr = str;
    if (!*saveptr)
        return NULL;

    *saveptr += strspn(*saveptr, delim);
    if (!**saveptr)
        return NULL;

    char *token = *saveptr;
    *saveptr += strcspn(*saveptr, delim);

    if (**saveptr)
    {
        **saveptr = '\0';
        (*saveptr)++;
    }
    else
    {
        *saveptr = NULL;
    }

    return token;
}

size_t strspn(const char *s, const char *accept)
{
    size_t count = 0;
    while (*s && strchr(accept, *s))
    {
        s++;
        count++;
    }
    return count;
}

size_t strcspn(const char *s, const char *reject)
{
    size_t count = 0;
    while (*s && !strchr(reject, *s))
    {
        s++;
        count++;
    }
    return count;
}

char *strpbrk(const char *s, const char *accept)
{
    while (*s)
    {
        if (strchr(accept, *s))
            return (char *)s;
        s++;
    }
    return NULL;
}

void *memset(void *s, int c, size_t n)
{
    unsigned char *p = (unsigned char *)s;
    unsigned char uc = (unsigned char)c;

    /* Real-HW fast path: `rep stosb` runs at full memory bandwidth on
     * any CPU advertising ERMS, and FSRM extends that down to very small
     * n. g_cpu_caps is BSS-zero until cpu_detect_features() runs late in
     * boot — early callers harmlessly take the scalar path. */
    if (g_cpu_caps.has_erms && (g_cpu_caps.has_fsrm || n >= KLIB_ERMS_THRESHOLD))
    {
        __asm__ volatile (
            "rep stosb"
            : "+D"(p), "+c"(n)
            : "a"(uc)
            : "memory"
        );
        return s;
    }

    /* Scalar fallback — also used during early boot before ERMS detection. */
    if (n < 8)
    {
        while (n--)
            *p++ = uc;
        return s;
    }

    while (((uintptr_t)p & 7) && n > 0)
    {
        *p++ = uc;
        n--;
    }

    if (n >= 8)
    {
        uint64_t pattern = (uint64_t)uc | ((uint64_t)uc << 8) |
                           ((uint64_t)uc << 16) | ((uint64_t)uc << 24) |
                           ((uint64_t)uc << 32) | ((uint64_t)uc << 40) |
                           ((uint64_t)uc << 48) | ((uint64_t)uc << 56);

        uint64_t *p64 = (uint64_t *)p;
        while (n >= 8)
        {
            *p64++ = pattern;
            n -= 8;
        }
        p = (unsigned char *)p64;
    }

    while (n--)
        *p++ = uc;

    return s;
}

void *memcpy(void *dest, const void *src, size_t n)
{
    unsigned char *d = (unsigned char *)dest;
    const unsigned char *s = (const unsigned char *)src;

    /* Real-HW fast path — see memset above for the ERMS/FSRM rationale. */
    if (g_cpu_caps.has_erms && (g_cpu_caps.has_fsrm || n >= KLIB_ERMS_THRESHOLD))
    {
        __asm__ volatile (
            "rep movsb"
            : "+D"(d), "+S"(s), "+c"(n)
            :
            : "memory"
        );
        return dest;
    }

    /* Scalar fast path: equal-alignment qword copy.  If alignments differ,
     * the source would be misaligned even after aligning the destination,
     * risking a read across an unmapped page boundary — fall through to
     * byte-by-byte which is always safe. */
    if (((uintptr_t)d & 7) == ((uintptr_t)s & 7))
    {
        while (((uintptr_t)d & 7) && n > 0)
        {
            *d++ = *s++;
            n--;
        }
        while (n >= 8)
        {
            *(uint64_t *)d = *(const uint64_t *)s;
            d += 8;
            s += 8;
            n -= 8;
        }
    }

    while (n--)
        *d++ = *s++;

    return dest;
}

void *memmem(const void *haystack, size_t haystacklen, const void *needle, size_t needlelen)
{
    if (!haystack || !needle || needlelen == 0 || haystacklen < needlelen)
        return NULL;

    const uint8_t *h = (const uint8_t *)haystack;
    const uint8_t *n = (const uint8_t *)needle;

    for (size_t i = 0; i <= haystacklen - needlelen; i++)
    {
        if (memcmp(h + i, n, needlelen) == 0)
            return (void *)(h + i);
    }

    return NULL;
}

void *memmove(void *dest, const void *src, size_t n)
{
    unsigned char *d = (unsigned char *)dest;
    const unsigned char *s = (const unsigned char *)src;

    /* If the ranges do not overlap, memmove == memcpy and we can use the
     * full ERMS-accelerated path. Overlap is detected geometrically:
     * `[dest, dest+n)` intersects `[src, src+n)` iff `dest < src+n` AND
     * `src < dest+n`. */
    const unsigned char *src_end  = s + n;
    const unsigned char *dest_end = d + n;
    if (d >= src_end || s >= dest_end)
        return memcpy(dest, src, n);

    /* Overlap. Forward copy (d < s) is still safe — and we can still use
     * `rep movsb` because the CPU dispatches it as ascending-address copy
     * (Intel SDM Vol 2B "REP/REPE/REPZ … MOVSB"). */
    if (d < s)
    {
        if (g_cpu_caps.has_erms && (g_cpu_caps.has_fsrm || n >= KLIB_ERMS_THRESHOLD))
        {
            __asm__ volatile (
                "rep movsb"
                : "+D"(d), "+S"(s), "+c"(n)
                :
                : "memory"
            );
            return dest;
        }
        while (n--)
            *d++ = *s++;
        return dest;
    }

    /* d > s: must copy backwards. ERMS does not natively support
     * descending copies (DF set + rep movsb works but is slow on most
     * modern micro-architectures), so use the scalar backward path —
     * still correct, and rare in practice. */
    d += n;
    s += n;
    while (n--)
        *--d = *--s;
    return dest;
}

int memcmp(const void *s1, const void *s2, size_t n)
{
    const unsigned char *p1 = s1, *p2 = s2;
    while (n--)
    {
        if (*p1 != *p2)
            return *p1 - *p2;
        p1++, p2++;
    }
    return 0;
}

void *memchr(const void *s, int c, size_t n)
{
    const unsigned char *p = s;
    while (n--)
    {
        if (*p == (unsigned char)c)
            return (void *)p;
        p++;
    }
    return NULL;
}

char *reverse_str(char *str)
{
    if (!str)
        return NULL;

    /* Return the ORIGINAL start so callers can chain with strlen/strcpy.
     * The legacy version returned the meeting-pointer in the middle of the
     * buffer, which silently broke the few callers that did use the result
     * (none currently, but the surface bug is latent). */
    char *orig = str;
    char *end  = str + strlen(str) - 1;
    while (str < end)
    {
        char tmp = *str;
        *str++ = *end;
        *end-- = tmp;
    }
    return orig;
}

char *reverse_range(char *start, char *end)
{
    if (!start || !end)
        return NULL;
    while (start < end)
    {
        char tmp = *start;
        *start++ = *end;
        *end-- = tmp;
    }
    return start;
}
