#include "box/io.h"
#include "box/io/vga.h"
#include "box/io/keyboard.h"
#include "box/string.h"
#include "box/ipc.h"
#include "box/convert.h"
#include "box/notify.h"

// ============================================================================
// IPC output buffer — batches print/println into fewer broadcast() calls.
// The display server's result ring has only 15 slots; without buffering,
// each print(" ") is a separate broadcast that overflows the ring.
// ============================================================================
#define IO_BUF_SIZE 200
static char io_buf[IO_BUF_SIZE];
static int  io_buf_pos = 0;

void io_flush(void) {
    if (io_get_mode() == IO_MODE_IPC && io_buf_pos > 0) {
        broadcast("display", io_buf, (uint16_t)io_buf_pos);
        io_buf_pos = 0;
    }
}

static void io_buf_append(const char* data, size_t len) {
    while (len > 0) {
        size_t space = (size_t)(IO_BUF_SIZE - io_buf_pos);
        size_t chunk = len < space ? len : space;
        memcpy(io_buf + io_buf_pos, data, chunk);
        io_buf_pos += (int)chunk;
        data += chunk;
        len -= chunk;
        if (io_buf_pos >= IO_BUF_SIZE) {
            io_flush();
        }
    }
}

static void io_buf_putc(char c) {
    io_buf[io_buf_pos++] = c;
    if (io_buf_pos >= IO_BUF_SIZE) {
        io_flush();
    }
}

// ============================================================================

static void print_raw(const char* str) {
    if (!str) return;

    const char* seg = str;
    while (*seg) {
        const char* nl = seg;
        while (*nl && *nl != '\n') nl++;

        if (nl > seg) {
            char buf[192];
            size_t len = (size_t)(nl - seg);
            if (len > 190) len = 190;
            memcpy(buf, seg, len);
            buf[len] = '\0';
            vga_puts(buf);
        }

        if (*nl == '\n') {
            vga_newline();
            nl++;
        }

        seg = nl;
    }
}

void print(const char* str) {
    if (io_get_mode() == IO_MODE_IPC) {
        if (!str) return;
        size_t len = strlen(str);
        io_buf_append(str, len);
        return;
    }
    print_raw(str);
}

void println(const char* str) {
    if (io_get_mode() == IO_MODE_IPC) {
        if (str) {
            size_t len = strlen(str);
            io_buf_append(str, len);
        }
        io_buf_putc('\n');
        return;
    }
    if (str) print_raw(str);
    vga_newline();
}

void clear(void) {
    if (io_get_mode() == IO_MODE_IPC) {
        io_flush();
        uint8_t cmd = 0x01;
        broadcast("display", &cmd, 1);
        return;
    }
    vga_clear(VGA_COLOR(VGA_LIGHT_GRAY, VGA_BLACK));
}

void color(uint8_t c) {
    if (io_get_mode() == IO_MODE_IPC) {
        io_flush();
        uint8_t cmd[2] = {0x02, c};
        broadcast("display", cmd, 2);
        return;
    }
    vga_setcolor(c);
}

int readline(char* buffer, size_t max_len) {
    io_flush();

    if (!buffer || max_len < 2) {
        return -1;
    }

    int result = kb_readline(buffer, max_len, true);
    if (result < 0) {
        return -1;
    }

    return result;
}

int getchar(void) {
    io_flush();
    return kb_getchar();
}

int input(const char* prompt, char* buffer, size_t max_len) {
    if (prompt) {
        print(prompt);
    }
    return readline(buffer, max_len);
}

void print_int(int num) {
    char buf[12];
    to_str(num, buf, sizeof(buf));
    print(buf);
}

void print_hex(uint32_t num) {
    const char hex[] = "0123456789abcdef";
    char buf[11] = "0x00000000";
    for (int i = 9; i >= 2; i--) {
        buf[i] = hex[num & 0xF];
        num >>= 4;
    }
    print(buf);
}
