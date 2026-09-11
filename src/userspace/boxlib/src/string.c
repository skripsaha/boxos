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

void* memccpy(void* dest, const void* src, int c, size_t n) {
    if (!dest || !src || n == 0) return 0;
    unsigned char *d = (unsigned char *)dest;
    const unsigned char *s = (const unsigned char *)src;
    const unsigned char stop = (unsigned char)c;
    while (n--) {
        *d++ = *s;
        if (*s++ == stop) return d;
    }
    return 0;
}

void* memset_explicit(void* ptr, int value, size_t n) {
    memset(ptr, value, n);
    __asm__ __volatile__("" : : "r"(ptr) : "memory");
    return ptr;
}

void* memmove(void* dest, const void* src, size_t n) {
    if (!dest || !src || n == 0 || dest == src) return dest;
    unsigned char *d = (unsigned char *)dest;
    const unsigned char *s = (const unsigned char *)src;

    if (d < s) {
        if (((uintptr_t)d & 7) == ((uintptr_t)s & 7)) {
            while (((uintptr_t)d & 7) && n > 0) { *d++ = *s++; n--; }
            while (n >= 8) {
                *(uint64_t *)d = *(const uint64_t *)s;
                d += 8; s += 8; n -= 8;
            }
        }
        while (n--) *d++ = *s++;
    } else {
        d += n;
        s += n;
        if (((uintptr_t)d & 7) == ((uintptr_t)s & 7)) {
            while (((uintptr_t)d & 7) && n > 0) { *--d = *--s; n--; }
            while (n >= 8) {
                d -= 8; s -= 8; n -= 8;
                *(uint64_t *)d = *(const uint64_t *)s;
            }
        }
        while (n--) *--d = *--s;
    }
    return dest;
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


char* strcat(char* dest, const char* src) {
    if (!dest) return NULL;
    if (!src) return dest;
    char* d = dest;
    while (*d) d++;
    while ((*d++ = *src++)) { }
    return dest;
}

char* strncat(char* dest, const char* src, size_t n) {
    if (!dest) return NULL;
    if (!src) return dest;
    char* d = dest;
    while (*d) d++;
    while (n-- && *src) *d++ = *src++;
    *d = '\0';
    return dest;
}

int strcoll(const char* s1, const char* s2) {
    return strcmp(s1, s2);
}

size_t strxfrm(char* dest, const char* src, size_t n) {
    size_t len = strlen(src);
    if (dest && n > 0) {
        size_t copy = (len < n - 1) ? len : n - 1;
        for (size_t i = 0; i < copy; i++) dest[i] = src[i];
        dest[copy] = '\0';
    }
    return len;
}

char* strchr(const char* s, int c) {
    if (!s) return NULL;
    const char ch = (char)c;
    for (;; s++) {
        if (*s == ch) return (char*)s;
        if (!*s) return NULL;
    }
}

char* strrchr(const char* s, int c) {
    if (!s) return NULL;
    const char ch = (char)c;
    const char* found = NULL;
    for (;; s++) {
        if (*s == ch) found = s;
        if (!*s) return (char*)found;
    }
}

char* strstr(const char* haystack, const char* needle) {
    if (!haystack || !needle) return NULL;
    if (!*needle) return (char*)haystack;
    for (; *haystack; haystack++) {
        const char* h = haystack;
        const char* n = needle;
        while (*h && *n && *h == *n) { h++; n++; }
        if (!*n) return (char*)haystack;
    }
    return NULL;
}

static void charset_build(unsigned char set[32], const char* chars) {
    for (int i = 0; i < 32; i++) set[i] = 0;
    if (!chars) return;
    for (const unsigned char* p = (const unsigned char*)chars; *p; p++)
        set[*p >> 3] |= (unsigned char)(1u << (*p & 7));
}

static int charset_has(const unsigned char set[32], unsigned char c) {
    return (set[c >> 3] >> (c & 7)) & 1u;
}

size_t strspn(const char* s, const char* accept) {
    if (!s) return 0;
    unsigned char set[32];
    charset_build(set, accept);
    size_t n = 0;
    while (s[n] && charset_has(set, (unsigned char)s[n])) n++;
    return n;
}

size_t strcspn(const char* s, const char* reject) {
    if (!s) return 0;
    unsigned char set[32];
    charset_build(set, reject);
    size_t n = 0;
    while (s[n] && !charset_has(set, (unsigned char)s[n])) n++;
    return n;
}

char* strpbrk(const char* s, const char* accept) {
    if (!s) return NULL;
    unsigned char set[32];
    charset_build(set, accept);
    for (; *s; s++)
        if (charset_has(set, (unsigned char)*s)) return (char*)s;
    return NULL;
}

static __thread char* g_strtok_cursor = NULL;

char* strtok(char* s, const char* delim) {
    unsigned char set[32];
    charset_build(set, delim);

    char* p = s ? s : g_strtok_cursor;
    if (!p) return NULL;

    while (*p && charset_has(set, (unsigned char)*p)) p++;
    if (!*p) { g_strtok_cursor = NULL; return NULL; }

    char* token = p;
    while (*p && !charset_has(set, (unsigned char)*p)) p++;
    if (*p) { *p = '\0'; g_strtok_cursor = p + 1; }
    else      g_strtok_cursor = NULL;
    return token;
}

void* memchr(const void* s, int c, size_t n) {
    if (!s) return NULL;
    const unsigned char* p = (const unsigned char*)s;
    const unsigned char ch = (unsigned char)c;
    while (n--) {
        if (*p == ch) return (void*)p;
        p++;
    }
    return NULL;
}