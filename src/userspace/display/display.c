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

static Color display_fg = COLOR_LIGHT_GRAY;
static Color display_bg = COLOR_BLACK;

static void render(const uint8_t* data, uint16_t len) {
    /* One shell render() = one user-visible frame fragment. With the
     * boxlib VGA batching from the 2026-05-26 audit, wrapping the whole
     * fragment in vga_begin/vga_commit collapses every internal
     * vga_clear/setcolor/puts/newline into ONE multi-op Manifest. A
     * typical printf("%color foo %color bar\n") used to fire 5+
     * syscalls between daemon and kernel; it now fires one.
     *
     * Buffer 256 (was 186) is comfortably inside the batch payload
     * arena and reduces inner iterations by ~33% per run. */
    vga_begin();

    uint16_t i = 0;
    while (i < len) {
        uint8_t b = data[i];

        if (b == DISP_CMD_CLEAR) {
            vga_clear_rgb(COLOR_LIGHT_GRAY, COLOR_BLACK);
            display_fg = COLOR_LIGHT_GRAY;
            display_bg = COLOR_BLACK;
            i++;
            continue;
        }

        /* [cmd][u32 fg][u32 bg] — full #RRGGBB pair, little-endian. */
        if (b == DISP_CMD_COLOR && i + 9 <= len) {
            memcpy(&display_fg, &data[i + 1], 4);
            memcpy(&display_bg, &data[i + 5], 4);
            vga_setcolor_rgb(display_fg, display_bg);
            i += 9;
            continue;
        }

        if (b == '\n') {
            vga_newline();
            i++;
            continue;
        }

        if (b >= 0x20) {
            char buf[256];
            int pos = 0;
            while (i < len && data[i] >= 0x20 && pos < (int)sizeof(buf) - 1) {
                buf[pos++] = (char)data[i++];
            }
            buf[pos] = '\0';
            vga_puts(buf);
            continue;
        }

        i++;
    }

    vga_commit();
}

static void handle_readline(uint32_t requester, uint16_t max_len, uint8_t echo) {
    /* Daemon is single-threaded; keep the two big I/O buffers in .bss so the
     * call chain handle_readline → kb_readline → MfCall1 → ManifestSubmitFull
     * stays well under the user-stack guard. Previously these 2052 bytes on
     * the stack pushed the cumulative chain past the lowest mapped page and
     * a memset down the chain wrote into the guard page (CR2=0x7fff…d05000,
     * err=0x6). */
    static char    line_buf[1024];
    static uint8_t reply_buf[1028];

    uint16_t cap = max_len > 0 ? max_len : 128;
    if (cap > sizeof(line_buf)) cap = sizeof(line_buf);
    int len = kb_readline(line_buf, cap, echo != 0);

    if (len < 0) {
        uint8_t err_reply[4] = {0, 0, 0, 0};
        send(requester, err_reply, 4);
        return;
    }

    uint32_t ulen = (uint32_t)len;
    if (ulen > sizeof(reply_buf) - 4) ulen = sizeof(reply_buf) - 4;
    memcpy(reply_buf, &ulen, 4);
    memcpy(reply_buf + 4, line_buf, ulen);
    send(requester, reply_buf, (uint16_t)(4 + ulen));
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
