#ifndef BOXOS_PIXEL_H
#define BOXOS_PIXEL_H

/*
 * boxos_pixel.h — one #RRGGBB value into a framebuffer's pixel layout, shared
 * kernel/userspace (the display program and the kernel's Emergency Screen draw
 * into the same glass).
 *
 * The includer supplies uint32_t.
 *
 * The format numbers are the ones the loaders put in boot_info.fb_format and
 * the Screen hands on in ScreenDescription.format.
 */

#define BOX_PIXEL_RGB   0u   /* PixelRedGreenBlueReserved8BitPerColor */
#define BOX_PIXEL_BGR   1u   /* PixelBlueGreenRedReserved8BitPerColor */
#define BOX_PIXEL_BGRX  2u

/*
 * A #RRGGBB uint32_t (R in bits 23..16, G in 15..8, B in 7..0) written to
 * memory little-endian puts B in byte 0 and R in byte 2 — in-memory order
 * B, G, R, X. That IS the BGR layout, so BGR is written as it is.
 *
 * For RGB hardware byte 0 must hold R, so R and B swap.
 *
 * This logic was once inverted: BGR was swapped (red came out blue) and RGB
 * passed through. The classic symptom was COLOR_CYAN rendering as yellow and
 * COLOR_RED as blue under UEFI GOP boots that report BGR (the QEMU/OVMF default).
 */
static inline uint32_t BoxPixelEncode(uint32_t rgb, uint32_t format)
{
    if (format == BOX_PIXEL_RGB) {
        uint32_t r = (rgb >> 16) & 0xFFu;
        uint32_t g = (rgb >>  8) & 0xFFu;
        uint32_t b =  rgb        & 0xFFu;
        return (b << 16) | (g << 8) | r;
    }
    return rgb;
}

#endif /* BOXOS_PIXEL_H */
