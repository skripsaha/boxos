#include "box/io.h"
#include "box/io/vga.h"
#include "box/io/keyboard.h"
#include "box/string.h"

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
    print_raw(str);
}

void println(const char* str) {
    if (str) print_raw(str);
    vga_newline();
}

void clear(void) {
    vga_clear(VGA_COLOR(COLOR_LIGHT_GRAY, COLOR_BLACK));
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
    if (num == 0) { print("0"); return; }
    char tmp[12];
    int i = 0;
    int neg = 0;
    unsigned int uval;
    if (num < 0) { neg = 1; uval = (unsigned int)(-(num + 1)) + 1; }
    else { uval = (unsigned int)num; }
    while (uval > 0) { tmp[i++] = '0' + (uval % 10); uval /= 10; }
    int j = 0;
    if (neg) buf[j++] = '-';
    while (i > 0) buf[j++] = tmp[--i];
    buf[j] = '\0';
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
