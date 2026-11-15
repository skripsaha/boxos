/*
 * Gangway — bytes moved, compared and summed with no library under them: a
 * UEFI application links no libc, and these few loops are all the loader
 * needs of one.
 */

#include "gangway.h"

void MemZero(void *dst, size_t n)
{
    uint8_t *p = (uint8_t *)dst;
    while (n--) *p++ = 0;
}

void MemCopy(void *dst, const void *src, size_t n)
{
    uint8_t       *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    while (n--) *d++ = *s++;
}

int MemEqual(const void *a, const void *b, size_t n)
{
    const uint8_t *p = (const uint8_t *)a;
    const uint8_t *q = (const uint8_t *)b;
    while (n--) {
        if (*p++ != *q++) return 0;
    }
    return 1;
}

int GuidEqual(const EFI_GUID *a, const EFI_GUID *b)
{
    return MemEqual(a, b, sizeof(EFI_GUID));
}
/* CRC-32 (ISO 3309), bitwise. The same sum GPT is specified in and the same
 * one a Deed carries, so one routine checks both. A table would be a kilobyte
 * of a loader that runs once. */
uint32_t Crc32(const uint8_t *data, uint32_t len)
{
    uint32_t crc = 0xFFFFFFFFU;
    for (uint32_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++)
            crc = (crc >> 1) ^ (0xEDB88320U & (uint32_t)(-(int32_t)(crc & 1)));
    }
    return ~crc;
}

uint32_t Le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

uint64_t Le64(const uint8_t *p)
{
    return (uint64_t)Le32(p) | ((uint64_t)Le32(p + 4) << 32);
}
