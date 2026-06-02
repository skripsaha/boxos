#include "box/string.h"

size_t strlen(const char* str) {
    if (!str) return 0;
    size_t len = 0;
    while (str[len]) len++;
    return len;
}

char* strcpy(char* dest, const char* src) {
    if (!dest) return NULL;
    if (!src) { dest[0] = '\0'; return dest; }
    char* d = dest;
    while ((*d++ = *src++));
    return dest;
}

char* strncpy(char* dest, const char* src, size_t n) {
    if (!dest) return NULL;
    if (!src) {
        /* Match strcpy() semantics: dest gets a single NUL when src is
         * NULL. C99 says strncpy is UB on NULL src; boxlib chooses the
         * safe-default behaviour because every userspace caller relies
         * on strncpy not faulting on misuse. */
        for (size_t i = 0; i < n; i++) dest[i] = '\0';
        return dest;
    }
    size_t i;
    for (i = 0; i < n && src[i]; i++) {
        dest[i] = src[i];
    }
    for (; i < n; i++) {
        dest[i] = '\0';
    }
    return dest;
}


int strcmp(const char* s1, const char* s2) {
    while (*s1 && (*s1 == *s2)) { s1++; s2++; }
    return *(unsigned char*)s1 - *(unsigned char*)s2;
}

int strncmp(const char* s1, const char* s2, size_t n) {
    while (n && *s1 && (*s1 == *s2)) { s1++; s2++; n--; }
    if (n == 0) return 0;
    return *(unsigned char*)s1 - *(unsigned char*)s2;
}

void* memcpy(void* dest, const void* src, size_t n) {
    if (!dest || !src || n == 0) return dest;
    unsigned char *d = (unsigned char *)dest;
    const unsigned char *s = (const unsigned char *)src;

    /* Fast path: both pointers same alignment → 8-byte qword copy.
     * Only safe when both are aligned — avoids misaligned read
     * crossing unmapped page boundaries. */
    if (((uintptr_t)d & 7) == ((uintptr_t)s & 7)) {
        while (((uintptr_t)d & 7) && n > 0) { *d++ = *s++; n--; }
        while (n >= 8) {
            *(uint64_t *)d = *(const uint64_t *)s;
            d += 8; s += 8; n -= 8;
        }
    }
    while (n--) *d++ = *s++;
    return dest;
}

void* memset(void* ptr, int value, size_t n) {
    if (!ptr || n == 0) return ptr;
    unsigned char *p = (unsigned char *)ptr;
    unsigned char uc = (unsigned char)value;

    /* Align then fill 8 bytes at a time (safe — memset only writes) */
    while (((uintptr_t)p & 7) && n > 0) { *p++ = uc; n--; }
    if (n >= 8) {
        uint64_t pattern = (uint64_t)uc | ((uint64_t)uc << 8) |
                           ((uint64_t)uc << 16) | ((uint64_t)uc << 24) |
                           ((uint64_t)uc << 32) | ((uint64_t)uc << 40) |
                           ((uint64_t)uc << 48) | ((uint64_t)uc << 56);
        while (n >= 8) {
            *(uint64_t *)p = pattern;
            p += 8; n -= 8;
        }
    }
    while (n--) *p++ = uc;
    return ptr;
}

int memcmp(const void* s1, const void* s2, size_t n) {
    const unsigned char* p1 = (const unsigned char*)s1;
    const unsigned char* p2 = (const unsigned char*)s2;
    while (n--) {
        if (*p1 != *p2) return *p1 - *p2;
        p1++; p2++;
    }
    return 0;
}
