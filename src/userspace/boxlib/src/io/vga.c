#include "box/io/vga.h"
#include "box/notify.h"
#include "box/result.h"
#include "box/string.h"

int vga_putchar(char c) {
    vga_pos_t pos = {0, 0};
    vga_getcursor(&pos);

    int color = vga_getcolor();
    if (color < 0) {
        color = VGA_SCHEME_DEFAULT;
    }

    uint8_t buf[4] = {pos.row, pos.col, (uint8_t)c, (uint8_t)color};
    pocket_send(DECK_HARDWARE, 0x70, buf, 4);

    Result result;
    if (!result_wait(&result, 100000)) {
        return -ERR_TIMEOUT;
    }

    if (result.error_code != OK) {
        return -(int)result.error_code;
    }

    return 0;
}

int vga_puts(const char* str) {
    if (!str) {
        return -ERR_INVALID_ARGS;
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

        uint8_t putstr_buf[192];
        memset(putstr_buf, 0, sizeof(putstr_buf));
        putstr_buf[0] = (uint8_t)chunk_size;
        putstr_buf[1] = (uint8_t)color;
        putstr_buf[2] = 0;
        putstr_buf[3] = 0;
        memcpy(putstr_buf + 4, str + offset, chunk_size);
        pocket_send(DECK_HARDWARE, 0x71, putstr_buf, (uint32_t)(chunk_size + 4));

        Result result;
        if (!result_wait(&result, 100000)) {
            return -ERR_TIMEOUT;
        }

        if (result.error_code != OK) {
            return -(int)result.error_code;
        }

        uint8_t* data = (uint8_t*)(uintptr_t)result.data_addr;
        uint16_t chars_written = ((uint16_t)data[0] << 8) | (uint16_t)data[1];
        total_written += chars_written;
        offset += chunk_size;
    }

    return total_written;
}

int vga_clear(uint8_t color) {
    pocket_send(DECK_HARDWARE, 0x72, &color, 1);

    Result result;
    if (!result_wait(&result, 100000)) {
        return -ERR_TIMEOUT;
    }

    if (result.error_code != OK) {
        return -(int)result.error_code;
    }

    return 0;
}

int vga_clear_line(uint8_t row, uint8_t color) {
    uint8_t buf[2] = {row, color};
    pocket_send(DECK_HARDWARE, 0x73, buf, 2);

    Result result;
    if (!result_wait(&result, 100000)) {
        return -ERR_TIMEOUT;
    }

    if (result.error_code != OK) {
        return -(int)result.error_code;
    }

    return 0;
}

int vga_clear_to_eol(uint8_t color) {
    pocket_send(DECK_HARDWARE, 0x74, &color, 1);

    Result result;
    if (!result_wait(&result, 100000)) {
        return -ERR_TIMEOUT;
    }

    if (result.error_code != OK) {
        return -(int)result.error_code;
    }

    return 0;
}

int vga_scroll_up(uint8_t lines, uint8_t fill_color) {
    uint8_t buf[2] = {lines, fill_color};
    pocket_send(DECK_HARDWARE, 0x79, buf, 2);

    Result result;
    if (!result_wait(&result, 100000)) {
        return -ERR_TIMEOUT;
    }

    if (result.error_code != OK) {
        return -(int)result.error_code;
    }

    return 0;
}

int vga_newline(void) {
    uint8_t buf[4] = {0};
    pocket_send(DECK_HARDWARE, 0x7A, buf, 4);

    Result result;
    if (!result_wait(&result, 100000)) {
        return -ERR_TIMEOUT;
    }

    if (result.error_code != OK) {
        return -(int)result.error_code;
    }

    return 0;
}

int vga_getcursor(vga_pos_t* pos) {
    if (!pos) {
        return -ERR_INVALID_ARGS;
    }

    uint8_t buf[4] = {0};
    pocket_send(DECK_HARDWARE, 0x75, buf, 4);

    Result result;
    if (!result_wait(&result, 100000)) {
        return -ERR_TIMEOUT;
    }

    if (result.error_code != OK) {
        return -(int)result.error_code;
    }

    uint8_t* data = (uint8_t*)(uintptr_t)result.data_addr;
    pos->row = data[1];
    pos->col = data[2];

    return 0;
}

int vga_setcursor(uint8_t row, uint8_t col) {
    uint8_t buf[2] = {row, col};
    pocket_send(DECK_HARDWARE, 0x76, buf, 2);

    Result result;
    if (!result_wait(&result, 100000)) {
        return -ERR_TIMEOUT;
    }

    if (result.error_code != OK) {
        return -(int)result.error_code;
    }

    return 0;
}

int vga_getcolor(void) {
    uint8_t buf[4] = {0};
    pocket_send(DECK_HARDWARE, 0x78, buf, 4);

    Result result;
    if (!result_wait(&result, 100000)) {
        return -ERR_TIMEOUT;
    }

    if (result.error_code != OK) {
        return -(int)result.error_code;
    }

    uint8_t* data = (uint8_t*)(uintptr_t)result.data_addr;
    return (int)data[1];
}

int vga_setcolor(uint8_t color) {
    pocket_send(DECK_HARDWARE, 0x77, &color, 1);

    Result result;
    if (!result_wait(&result, 100000)) {
        return -ERR_TIMEOUT;
    }

    if (result.error_code != OK) {
        return -(int)result.error_code;
    }

    return 0;
}

int vga_getdimensions(vga_dimensions_t* dims) {
    if (!dims) {
        return -ERR_INVALID_ARGS;
    }

    uint8_t buf[4] = {0};
    pocket_send(DECK_HARDWARE, 0x7B, buf, 4);

    Result result;
    if (!result_wait(&result, 100000)) {
        return -ERR_TIMEOUT;
    }

    if (result.error_code != OK) {
        return -(int)result.error_code;
    }

    uint8_t* data = (uint8_t*)(uintptr_t)result.data_addr;
    dims->rows = data[0];
    dims->cols = data[1];

    return 0;
}
