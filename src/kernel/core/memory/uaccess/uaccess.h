#ifndef UACCESS_H
#define UACCESS_H

#include "ktypes.h"

#define UACCESS_USER_VA_MAX     0x0000800000000000ULL

extern bool g_uaccess_smap;

void uaccess_set_smap(bool present);

static inline void stac(void) {
    if (g_uaccess_smap) __asm__ volatile("stac" ::: "cc", "memory");
}
static inline void clac(void) {
    if (g_uaccess_smap) __asm__ volatile("clac" ::: "cc", "memory");
}

static inline void user_access_begin(void) { stac(); }
static inline void user_access_end(void)   { clac(); }

static inline bool access_ok(const void *addr, size_t size) {
    uintptr_t a = (uintptr_t)addr;
    if (a == 0) return false;
    if (a >= UACCESS_USER_VA_MAX) return false;
    if (size == 0) return true;
    if (a > UACCESS_USER_VA_MAX - size) return false;
    return true;
}

size_t copy_to_user(void *dst, const void *src, size_t n);

size_t copy_from_user(void *dst, const void *src, size_t n);

int put_user_u32(uint32_t val, uint32_t *ptr);
int put_user_u64(uint64_t val, uint64_t *ptr);

int get_user_u32(uint32_t *out, const uint32_t *ptr);
int get_user_u64(uint64_t *out, const uint64_t *ptr);

uintptr_t uaccess_lookup_fixup(uintptr_t fault_rip);

void uaccess_init(void);

#endif