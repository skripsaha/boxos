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

/* C23 memccpy: copy at most n bytes, stopping AFTER the first byte equal to
 * (unsigned char)c. Returns the address just past that byte in dest, or NULL
 * when c never appeared — the null return is the whole point, because it is
 * how the caller learns the record was truncated. Byte at a time on purpose:
 * the loop has to stop on a value, so there is no word-sized fast path that
 * would not have to re-examine what it just copied. */
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

/* C23 memset_explicit: memset that MAY NOT be optimized away. A plain memset
 * over an object nothing reads again is a dead store, and a compiler is
 * entitled to delete it — which is exactly the wrong answer when the object
 * held a key. The empty asm with "memory" tells the compiler the bytes were
 * read by something it cannot see, so the writes have to be there.
 *
 * ‼ The barrier is not observable from inside a conforming program, and a
 * mutation that deleted it was NOT caught by the suite — correctly. Reading an
 * object to prove its erasure was not elided means reading a dead object,
 * which is the undefined behaviour the whole exercise is about. It is also
 * belt and braces in THIS build: the function lives in its own translation
 * unit and nothing links with LTO, so the call cannot be inlined and the store
 * cannot be seen to be dead. The barrier is here for the build where one day
 * it can be. What would pin it is reading generated code, not running it. */
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
        /* Forward copy — same qword fast path as memcpy. */
        if (((uintptr_t)d & 7) == ((uintptr_t)s & 7)) {
            while (((uintptr_t)d & 7) && n > 0) { *d++ = *s++; n--; }
            while (n >= 8) {
                *(uint64_t *)d = *(const uint64_t *)s;
                d += 8; s += 8; n -= 8;
            }
        }
        while (n--) *d++ = *s++;
    } else {
        /* Backward copy from the top — safe for overlapping d > s. */
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

/* ---------------------------------------------------------------------------
 * Ф41 — the rest of [cstring.syn].
 *
 * These thirteen were missing for as long as boxlib existed, and nothing had
 * asked for them: BoxOS code says box_* and C++ code said std::string. A
 * <cstring> is what asks. They are here rather than in boxcxx because a C
 * string function has no C++ in it, and one definition shared by both languages
 * cannot drift.
 * ------------------------------------------------------------------------- */

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
    *d = '\0';                      /* always terminated, unlike strncpy */
    return dest;
}

/* The "C" locale collates by character code, so ordering IS strcmp and the
 * transform that would make it so is a copy. Both are here for the callers
 * [cstring.syn] promises them to, not because they add anything. */
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
    return len;                     /* length NEEDED, terminator excluded */
}

char* strchr(const char* s, int c) {
    if (!s) return NULL;
    const char ch = (char)c;
    for (;; s++) {
        if (*s == ch) return (char*)s;   /* '\0' is findable, as C requires */
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
    if (!*needle) return (char*)haystack;   /* empty needle matches at 0 */
    for (; *haystack; haystack++) {
        const char* h = haystack;
        const char* n = needle;
        while (*h && *n && *h == *n) { h++; n++; }
        if (!*n) return (char*)haystack;
    }
    return NULL;
}

/* One pass over the 256-bit set beats a nested scan the moment `accept` is
 * longer than a couple of characters, and costs 32 bytes of stack. */
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

/* PER-STRAND cursor. C describes one static object and every hosted libc makes
 * it exactly that, which is why strtok is the textbook example of a function
 * two threads must not both call. Here the state is __thread, so two strands
 * tokenizing two strings are independent — the standard says the state is
 * unspecified, so this is conformance and not extension. */
static __thread char* g_strtok_cursor = NULL;

char* strtok(char* s, const char* delim) {
    unsigned char set[32];
    charset_build(set, delim);

    char* p = s ? s : g_strtok_cursor;
    if (!p) return NULL;

    while (*p && charset_has(set, (unsigned char)*p)) p++;   /* leading delims */
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
