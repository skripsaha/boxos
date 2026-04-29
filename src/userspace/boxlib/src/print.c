#include "box/print.h"
#include "box/vga.h"
#include "box/keyboard.h"
#include "box/string.h"
#include "box/ipc.h"
#include "box/convert.h"
#include "box/core/notify.h"
#include "box/display.h"

/* ===========================================================================
 * Cabin I/O state — IO mode + display daemon PID
 * Was io_mode.c (merged Stage 2 — same conceptual domain).
 * =========================================================================== */
static uint8_t  g_io_mode    = IO_MODE_IPC;
static uint32_t g_display_pid = 0;

void     io_set_mode(uint8_t mode)        { g_io_mode = mode; }
uint8_t  io_get_mode(void)                { return g_io_mode; }
void     io_set_display_pid(uint32_t pid) { g_display_pid = pid; }
uint32_t io_get_display_pid(void)         { return g_display_pid; }

/* ===========================================================================
 * IPC output buffer — batches print/println into fewer broadcast() calls.
 * The display server's result ring has only 15 slots; without buffering,
 * each print(" ") is a separate broadcast that overflows the ring.
 * =========================================================================== */
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
        // Don't flush on every println — let the buffer batch multiple lines.
        // The buffer auto-flushes when full (IO_BUF_SIZE), and callers flush
        // explicitly before readline, prompt, or exit.
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
    vga_clear(VIDEO_COLOR(VIDEO_LIGHT_GRAY, VIDEO_BLACK));
}

void color(uint8_t c) {
    if (io_get_mode() == IO_MODE_IPC) {
        // Embed color command inline in IO buffer (0x02 = DISPLAY_CMD_COLOR).
        // This avoids a separate flush + broadcast per color change, which
        // previously caused the display's IPC stash to overflow and silently
        // drop queued messages.
        char cmd[2] = {0x02, (char)c};
        io_buf_append(cmd, 2);
        return;
    }
    vga_setcolor(c);
}

int readline(char* buffer, size_t max_len) {
    io_flush();

    if (!buffer || max_len < 2) {
        return -1;
    }

    /* If in IPC mode but display daemon not yet known, try to discover it */
    if (io_get_mode() == IO_MODE_IPC && g_display_pid == 0) {
        uint8_t ping = DISP_CMD_PING;
        broadcast("display", &ping, 1);
        Result ping_result;
        if (receive_wait(&ping_result, 2000) && ping_result.sender_pid != 0) {
            g_display_pid = ping_result.sender_pid;
        }
    }

    if (io_get_mode() == IO_MODE_IPC && g_display_pid != 0) {
        uint16_t capped = (uint16_t)(max_len > 1024 ? 1024 : max_len);
        uint8_t req[4] = {DISP_CMD_READLINE, (uint8_t)(capped & 0xFF), (uint8_t)(capped >> 8), 1};
        send(g_display_pid, req, 4);

        Result result;
        if (!receive_wait(&result, 60000)) return -1;
        if (result.error_code != OK) return -1;
        if (result.data_addr == 0 || result.data_length < 4) return -1;

        const uint8_t* resp = (const uint8_t*)(uintptr_t)result.data_addr;
        uint32_t len;
        memcpy(&len, resp, 4);
        if (len > max_len - 1) len = (uint32_t)(max_len - 1);
        memcpy(buffer, resp + 4, len);
        buffer[len] = '\0';
        return (int)len;
    }

    int len = kb_readline(buffer, max_len, true);
    if (len < 0) return -1;
    return len;
}

int getchar(void) {
    io_flush();

    /* If in IPC mode but display daemon not yet known, try to discover it */
    if (io_get_mode() == IO_MODE_IPC && g_display_pid == 0) {
        uint8_t ping = DISP_CMD_PING;
        broadcast("display", &ping, 1);
        Result ping_result;
        if (receive_wait(&ping_result, 2000) && ping_result.sender_pid != 0) {
            g_display_pid = ping_result.sender_pid;
        }
    }

    if (io_get_mode() == IO_MODE_IPC && g_display_pid != 0) {
        uint8_t req = DISP_CMD_GETCHAR;
        send(g_display_pid, &req, 1);

        Result result;
        if (!receive_wait(&result, 60000)) return -1;
        if (result.error_code != OK) return -1;
        if (result.data_addr == 0 || result.data_length < 1) return -1;

        return *(const uint8_t*)(uintptr_t)result.data_addr;
    }

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

/* ===========================================================================
 * printf — was format.c (merged Stage 2).
 * Builds a 512B formatted string then calls print().
 * =========================================================================== */
static void fmt_htoa64(unsigned long value, char *buf)
{
    const char *digits = "0123456789abcdef";
    if (value == 0) { buf[0] = '0'; buf[1] = '\0'; return; }
    char tmp[17];
    int i = 0;
    while (value > 0) {
        tmp[i++] = digits[value & 0xF];
        value >>= 4;
    }
    int j = 0;
    while (i > 0) buf[j++] = tmp[--i];
    buf[j] = '\0';
}

int printf(const char *fmt, ...)
{
    if (!fmt) return -1;

    va_list args;
    va_start(args, fmt);

    char out[512];
    int  pos = 0;
    char numbuf[20];

    while (*fmt && pos < 510) {
        if (*fmt != '%') { out[pos++] = *fmt++; continue; }
        fmt++;
        switch (*fmt) {
            case 's': {
                const char *s = va_arg(args, const char *);
                if (!s) s = "(null)";
                while (*s && pos < 510) out[pos++] = *s++;
                break;
            }
            case 'd': {
                int v = va_arg(args, int);
                to_str(v, numbuf, sizeof(numbuf));
                for (int i = 0; numbuf[i] && pos < 510; i++) out[pos++] = numbuf[i];
                break;
            }
            case 'u': {
                unsigned int v = va_arg(args, unsigned int);
                uint_to_str(v, numbuf, sizeof(numbuf));
                for (int i = 0; numbuf[i] && pos < 510; i++) out[pos++] = numbuf[i];
                break;
            }
            case 'x': case 'X': {
                unsigned int v = va_arg(args, unsigned int);
                to_hex(v, numbuf, sizeof(numbuf));
                for (int i = 0; numbuf[i] && pos < 510; i++) out[pos++] = numbuf[i];
                break;
            }
            case 'c': {
                char c = (char)va_arg(args, int);
                out[pos++] = c;
                break;
            }
            case 'p': {
                unsigned long v = va_arg(args, unsigned long);
                if (pos < 508) { out[pos++] = '0'; out[pos++] = 'x'; }
                fmt_htoa64(v, numbuf);
                for (int i = 0; numbuf[i] && pos < 510; i++) out[pos++] = numbuf[i];
                break;
            }
            case '%': out[pos++] = '%'; break;
            case '\0': goto done;
            default:
                out[pos++] = '%';
                if (pos < 510) out[pos++] = *fmt;
                break;
        }
        fmt++;
    }
done:
    out[pos] = '\0';
    va_end(args);

    print(out);
    return pos;
}
