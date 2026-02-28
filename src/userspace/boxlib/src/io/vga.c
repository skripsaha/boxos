#include "box/io/vga.h"
#include "box/chain.h"
#include "box/notify.h"
#include "box/result.h"
#include "box/string.h"

static uint16_t vga_decode_u16(const uint8_t* payload, size_t offset) {
    return ((uint16_t)payload[offset] << 8) | ((uint16_t)payload[offset + 1]);
}

int vga_putchar(char c) {
    vga_pos_t pos = {0, 0};
    vga_getcursor(&pos);

    int color = vga_getcolor();
    if (color < 0) {
        color = VGA_SCHEME_DEFAULT;
    }

    hw_vga_putchar(pos.row, pos.col, c, (uint8_t)color);
    notify();

    result_entry_t result;
    if (!result_wait(&result, 100000)) {
        return -BOX_ERR_TIMEOUT;
    }

    if (result.error_code != BOX_OK) {
        return -(int)result.error_code;
    }

    return 0;
}

int vga_puts(const char* str) {
    if (!str) {
        return -BOX_ERR_INVALID_ARGS;
    }

    size_t total_len = strlen(str);
    size_t offset = 0;
    int total_written = 0;

    while (offset < total_len) {
        size_t chunk_size = total_len - offset;
        if (chunk_size > 185) {
            chunk_size = 185;
        }

        int color = vga_getcolor();
        if (color < 0) {
            color = VGA_SCHEME_DEFAULT;
        }

        hw_vga_putstring(str + offset, (uint8_t)chunk_size, (uint8_t)color);
        notify();

        result_entry_t result;
        if (!result_wait(&result, 100000)) {
            return -BOX_ERR_TIMEOUT;
        }

        if (result.error_code != BOX_OK) {
            return -(int)result.error_code;
        }

        uint16_t chars_written = vga_decode_u16(result.payload, 0);
        total_written += chars_written;
        offset += chunk_size;
    }

    return total_written;
}

int vga_clear(uint8_t color) {
    hw_vga_clear(color);
    notify();

    result_entry_t result;
    if (!result_wait(&result, 100000)) {
        return -BOX_ERR_TIMEOUT;
    }

    if (result.error_code != BOX_OK) {
        return -(int)result.error_code;
    }

    return 0;
}

int vga_clear_line(uint8_t row, uint8_t color) {
    hw_vga_clear_line(row, color);
    notify();

    result_entry_t result;
    if (!result_wait(&result, 100000)) {
        return -BOX_ERR_TIMEOUT;
    }

    if (result.error_code != BOX_OK) {
        return -(int)result.error_code;
    }

    return 0;
}

int vga_clear_to_eol(uint8_t color) {
    hw_vga_clear_to_eol(color);
    notify();

    result_entry_t result;
    if (!result_wait(&result, 100000)) {
        return -BOX_ERR_TIMEOUT;
    }

    if (result.error_code != BOX_OK) {
        return -(int)result.error_code;
    }

    return 0;
}

int vga_scroll_up(uint8_t lines, uint8_t fill_color) {
    hw_vga_scroll(lines, fill_color);
    notify();

    result_entry_t result;
    if (!result_wait(&result, 100000)) {
        return -BOX_ERR_TIMEOUT;
    }

    if (result.error_code != BOX_OK) {
        return -(int)result.error_code;
    }

    return 0;
}

int vga_newline(void) {
    hw_vga_newline();
    notify();

    result_entry_t result;
    if (!result_wait(&result, 100000)) {
        return -BOX_ERR_TIMEOUT;
    }

    if (result.error_code != BOX_OK) {
        return -(int)result.error_code;
    }

    return 0;
}

int vga_getcursor(vga_pos_t* pos) {
    if (!pos) {
        return -BOX_ERR_INVALID_ARGS;
    }

    hw_vga_getcursor();
    notify();

    result_entry_t result;
    if (!result_wait(&result, 100000)) {
        return -BOX_ERR_TIMEOUT;
    }

    if (result.error_code != BOX_OK) {
        return -(int)result.error_code;
    }

    pos->row = result.payload[1];
    pos->col = result.payload[2];

    return 0;
}

int vga_setcursor(uint8_t row, uint8_t col) {
    hw_vga_setcursor(row, col);
    notify();

    result_entry_t result;
    if (!result_wait(&result, 100000)) {
        return -BOX_ERR_TIMEOUT;
    }

    if (result.error_code != BOX_OK) {
        return -(int)result.error_code;
    }

    return 0;
}

int vga_getcolor(void) {
    hw_vga_getcolor();
    notify();

    result_entry_t result;
    if (!result_wait(&result, 100000)) {
        return -BOX_ERR_TIMEOUT;
    }

    if (result.error_code != BOX_OK) {
        return -(int)result.error_code;
    }

    return (int)(uint8_t)result.payload[1];
}

int vga_setcolor(uint8_t color) {
    hw_vga_setcolor(color);
    notify();

    result_entry_t result;
    if (!result_wait(&result, 100000)) {
        return -BOX_ERR_TIMEOUT;
    }

    if (result.error_code != BOX_OK) {
        return -(int)result.error_code;
    }

    return 0;
}

int vga_getdimensions(vga_dimensions_t* dims) {
    if (!dims) {
        return -BOX_ERR_INVALID_ARGS;
    }

    hw_vga_getdimensions();
    notify();

    result_entry_t result;
    if (!result_wait(&result, 100000)) {
        return -BOX_ERR_TIMEOUT;
    }

    if (result.error_code != BOX_OK) {
        return -(int)result.error_code;
    }

    dims->rows = result.payload[0];
    dims->cols = result.payload[1];

    return 0;
}
