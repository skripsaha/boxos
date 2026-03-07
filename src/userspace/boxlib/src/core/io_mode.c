#include "box/io.h"

static uint8_t g_io_mode = IO_MODE_VGA;

void io_set_mode(uint8_t mode) {
    g_io_mode = mode;
}

uint8_t io_get_mode(void) {
    return g_io_mode;
}
