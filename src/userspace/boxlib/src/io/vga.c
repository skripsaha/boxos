#include "box/io/vga.h"
#include "box/notify.h"
#include "box/result.h"
#include "box/string.h"

// Helper: Decode big-endian uint16_t from payload
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

    notify_prepare();
    notify_add_prefix(BOX_DECK_HARDWARE, VGA_OP_PUTCHAR);

    uint8_t data[4];
    data[0] = pos.row;
    data[1] = pos.col;
    data[2] = (uint8_t)c;
    data[3] = (uint8_t)color;

    notify_write_data(data, 4);
    notify_execute();

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

        notify_prepare();
        notify_add_prefix(BOX_DECK_HARDWARE, VGA_OP_PUTSTRING);

        uint8_t data[192];
        memset(data, 0, 192);

        data[0] = (uint8_t)chunk_size;  // length (single byte)
        data[1] = (uint8_t)color;       // color (single byte)
        data[2] = 0;                    // flags
        data[3] = 0;                    // reserved

        memcpy(data + 4, str + offset, chunk_size);

        notify_write_data(data, chunk_size + 4);
        notify_execute();

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
    notify_prepare();
    notify_add_prefix(BOX_DECK_HARDWARE, VGA_OP_CLEAR_SCREEN);

    uint8_t data[1];
    data[0] = color;

    notify_write_data(data, 1);
    notify_execute();

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
    notify_prepare();
    notify_add_prefix(BOX_DECK_HARDWARE, VGA_OP_CLEAR_LINE);

    uint8_t data[2];
    data[0] = row;
    data[1] = color;

    notify_write_data(data, 2);
    notify_execute();

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
    notify_prepare();
    notify_add_prefix(BOX_DECK_HARDWARE, VGA_OP_CLEAR_TO_EOL);

    uint8_t data[1];
    data[0] = color;

    notify_write_data(data, 1);
    notify_execute();

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
    notify_prepare();
    notify_add_prefix(BOX_DECK_HARDWARE, VGA_OP_SCROLL_UP);

    uint8_t data[2];
    data[0] = lines;
    data[1] = fill_color;

    notify_write_data(data, 2);
    notify_execute();

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
    notify_prepare();
    notify_add_prefix(BOX_DECK_HARDWARE, VGA_OP_NEWLINE);
    notify_execute();

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

    notify_prepare();
    notify_add_prefix(BOX_DECK_HARDWARE, VGA_OP_GET_CURSOR);
    notify_execute();

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
    notify_prepare();
    notify_add_prefix(BOX_DECK_HARDWARE, VGA_OP_SET_CURSOR);

    uint8_t data[2];
    data[0] = row;
    data[1] = col;

    notify_write_data(data, 2);
    notify_execute();

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
    notify_prepare();
    notify_add_prefix(BOX_DECK_HARDWARE, VGA_OP_GET_COLOR);
    notify_execute();

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
    notify_prepare();
    notify_add_prefix(BOX_DECK_HARDWARE, VGA_OP_SET_COLOR);

    uint8_t data[1];
    data[0] = color;

    notify_write_data(data, 1);
    notify_execute();

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

    notify_prepare();
    notify_add_prefix(BOX_DECK_HARDWARE, VGA_OP_GET_DIMENSIONS);
    notify_execute();

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
