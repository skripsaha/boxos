#include "box/print.h"
#include "box/vga.h"
#include "box/keyboard.h"
#include "box/ipc.h"
#include "box/core/notify.h"
#include "box/core/cabin.h"
#include "box/core/result.h"
#include "box/string.h"
#include "box/system.h"
#include "box/display.h"

static uint8_t display_cached_color = VIDEO_COLOR(VIDEO_LIGHT_GRAY, VIDEO_BLACK);

static void render(const uint8_t* data, uint16_t len) {
    uint16_t i = 0;
    while (i < len) {
        uint8_t b = data[i];

        if (b == DISP_CMD_CLEAR) {
            vga_clear(VIDEO_COLOR(VIDEO_LIGHT_GRAY, VIDEO_BLACK));
            display_cached_color = VIDEO_COLOR(VIDEO_LIGHT_GRAY, VIDEO_BLACK);
            i++;
            continue;
        }

        if (b == DISP_CMD_COLOR && i + 1 < len) {
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
            char buf[186];
            int pos = 0;
            while (i < len && data[i] >= 0x20 && pos < 185) {
                buf[pos++] = (char)data[i++];
            }
            buf[pos] = '\0';
            vga_puts(buf);
            continue;
        }

        i++;
    }
}

static void handle_readline(uint32_t requester, uint16_t max_len, uint8_t echo) {
    char line_buf[1024];
    uint16_t cap = max_len > 0 ? max_len : 128;
    if (cap > sizeof(line_buf)) cap = sizeof(line_buf);
    int len = kb_readline(line_buf, cap, echo != 0);

    if (len < 0) {
        uint8_t reply[4] = {0, 0, 0, 0};
        send(requester, reply, 4);
        return;
    }

    uint8_t reply[1028];
    uint32_t ulen = (uint32_t)len;
    memcpy(reply, &ulen, 4);
    memcpy(reply + 4, line_buf, ulen);
    send(requester, reply, (uint16_t)(4 + ulen));
}

static void handle_getchar(uint32_t requester) {
    int ch = kb_getchar();
    uint8_t reply = (ch >= 0) ? (uint8_t)ch : 0;
    send(requester, &reply, 1);
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
        if (!receive_wait(&entry, 0)) continue;

        if (entry.data_addr == 0 || entry.data_length == 0) continue;

        const uint8_t* data = (const uint8_t*)(uintptr_t)entry.data_addr;
        uint16_t len = (uint16_t)entry.data_length;
        uint8_t cmd = data[0];

        if (cmd == DISP_CMD_READLINE && entry.sender_pid != 0 && len >= 4) {
            uint16_t rl_max = (uint16_t)data[1] | ((uint16_t)data[2] << 8);
            handle_readline(entry.sender_pid, rl_max, data[3]);
        } else if (cmd == DISP_CMD_GETCHAR && entry.sender_pid != 0) {
            handle_getchar(entry.sender_pid);
        } else if (cmd == DISP_CMD_PING && entry.sender_pid != 0) {
            CabinInfo* self = cabin_info();
            uint32_t my_pid = self->pid;
            send(entry.sender_pid, &my_pid, 4);
        } else {
            render(data, len);
        }
    }

    return 0;
}
