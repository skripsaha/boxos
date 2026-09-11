#ifndef HW_VGA_H
#define HW_VGA_H

#include "ktypes.h"

#define HW_VGA_BUF_ADDR        0xB8000u
#define HW_VGA_COLS            80u
#define HW_VGA_ROWS            25u
#define HW_VGA_BYTES_PER_CELL  2u
#define HW_VGA_BUF_SIZE        (HW_VGA_COLS * HW_VGA_ROWS * HW_VGA_BYTES_PER_CELL)
#define HW_VGA_LINE_BYTES      (HW_VGA_COLS * HW_VGA_BYTES_PER_CELL)

#define HW_VGA_CRTC_ADDR_PORT   0x3D4u
#define HW_VGA_CRTC_DATA_PORT   0x3D5u
#define HW_VGA_CURSOR_HIGH_REG  0x0Eu
#define HW_VGA_CURSOR_LOW_REG   0x0Fu
#define HW_VGA_START_HIGH_REG   0x0Cu
#define HW_VGA_START_LOW_REG    0x0Du

struct DisplayBackend;

struct DisplayBackend *HwVgaBackendInit(void);

#endif