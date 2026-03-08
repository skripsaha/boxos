#include "box/io.h"
#include "box/io/vga.h"
#include "box/ipc.h"
#include "box/notify.h"
#include "box/result.h"
#include "box/string.h"
#include "box/system.h"

#define DISPLAY_CMD_CLEAR 0x01
#define DISPLAY_CMD_COLOR 0x02

// Cached VGA color — avoids a vga_getcolor() syscall on every text render.
static uint8_t display_cached_color = VGA_COLOR(VGA_LIGHT_GRAY, VGA_BLACK);

static void render(const uint8_t* data, uint16_t len) {
    uint16_t i = 0;
    while (i < len) {
        uint8_t b = data[i];

        if (b == DISPLAY_CMD_CLEAR) {
            vga_clear(VGA_COLOR(VGA_LIGHT_GRAY, VGA_BLACK));
            display_cached_color = VGA_COLOR(VGA_LIGHT_GRAY, VGA_BLACK);
            i++;
            continue;
        }

        if (b == DISPLAY_CMD_COLOR && i + 1 < len) {
            display_cached_color = data[i + 1];
            vga_setcolor(display_cached_color);
            i += 2;
            continue;
        }

        if (b == '\n') {
            vga_newline();
            i++;
            continue;
        }

        if (b >= 0x20) {
            char buf[192];
            int pos = 0;
            while (i < len && data[i] >= 0x20 && pos < 185) {
                buf[pos++] = (char)data[i++];
            }
            buf[pos] = '\0';
            // Build putstring packet directly with cached color.
            // vga_puts() calls vga_getcolor() per chunk (extra syscall),
            // so bypassing it halves the syscall count for text rendering.
            uint8_t pkt[192];
            pkt[0] = (uint8_t)pos;
            pkt[1] = display_cached_color;
            pkt[2] = 0;
            pkt[3] = 0;
            memcpy(pkt + 4, buf, pos);
            pocket_send(DECK_HARDWARE, 0x71, pkt, (uint32_t)(pos + 4));
            Result result;
            result_wait(&result, 100000);
            continue;
        }

        i++;
    }
}

int main(void) {
    io_set_mode(IO_MODE_VGA);

    CabinInfo* ci = cabin_info();
    if (ci->spawner_pid != 0) {
        uint8_t ready = 0xFF;
        send(ci->spawner_pid, &ready, 1);
    }

    while (1) {
        Result entry;
        if (receive_wait(&entry, 0)) {
            if (entry.data_addr != 0 && entry.data_length > 0) {
                render((const uint8_t*)(uintptr_t)entry.data_addr, (uint16_t)entry.data_length);
            }
        }
    }

    return 0;
}
