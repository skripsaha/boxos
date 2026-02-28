#include "box/io.h"
#include "box/io/vga.h"
#include "box/io/keyboard.h"
#include "box/string.h"
#include "box/ipc.h"
#include "box/convert.h"

// ============================================================================
// OUTPUT
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
        while (len > 0) {
            uint16_t chunk = len > 200 ? 200 : (uint16_t)len;
            broadcast("shell", str, chunk);
            str += chunk;
            len -= chunk;
        }
        return;
    }
    print_raw(str);
}

void println(const char* str) {
    if (io_get_mode() == IO_MODE_IPC) {
        if (str) print(str);
        broadcast("shell", "\n", 1);
        return;
    }
    if (str) print_raw(str);
    vga_newline();
}

void clear(void) {
    vga_clear(VGA_COLOR(VGA_LIGHT_GRAY, VGA_BLACK));
}

void color(uint8_t c) {
    vga_setcolor(c);
}

// ============================================================================
// INPUT
// ============================================================================

int readline(char* buffer, size_t max_len) {
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
