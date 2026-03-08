#include "box/chain.h"
#include "box/notify.h"
#include "box/string.h"

// Scratch buffer in BSS for data that needs to be in the cabin heap
// (PocketRing data_addr must point to mapped memory)
static uint8_t g_data_buf[256] __attribute__((aligned(16)));

void route(uint32_t target_pid) {
    Pocket p;
    pocket_prepare(&p);
    p.target_pid = target_pid;
    p.route_tag[0] = '\0';
    pocket_add_prefix(&p, DECK_SYSTEM, 0x40);
    pocket_submit(&p);
}

void route_tag(const char* tag) {
    if (!tag) return;
    Pocket p;
    pocket_prepare(&p);
    p.target_pid = 0;
    size_t len = strlen(tag);
    if (len > 31) len = 31;
    memcpy(p.route_tag, tag, len);
    p.route_tag[len] = '\0';
    pocket_add_prefix(&p, DECK_SYSTEM, 0x41);
    pocket_submit(&p);
}

void hw_listen(uint8_t source_type, uint8_t flags) {
    memset(g_data_buf, 0, 4);
    g_data_buf[0] = source_type;
    g_data_buf[1] = flags;

    Pocket p;
    pocket_prepare(&p);
    pocket_set_data(&p, g_data_buf, 4);
    pocket_add_prefix(&p, DECK_SYSTEM, 0x42);
    pocket_submit(&p);
}

void proc_kill(uint32_t pid) {
    memset(g_data_buf, 0, 8);
    memcpy(g_data_buf, &pid, sizeof(pid));

    Pocket p;
    pocket_prepare(&p);
    pocket_set_data(&p, g_data_buf, 8);
    pocket_add_prefix(&p, DECK_SYSTEM, 0x02);
    pocket_submit(&p);
}

void proc_query(uint32_t pid) {
    memset(g_data_buf, 0, 8);
    memcpy(g_data_buf, &pid, sizeof(pid));

    Pocket p;
    pocket_prepare(&p);
    pocket_set_data(&p, g_data_buf, 8);
    pocket_add_prefix(&p, DECK_SYSTEM, 0x03);
    pocket_submit(&p);
}

void proc_spawn(const char* filename) {
    if (!filename) return;
    memset(g_data_buf, 0, 192);
    size_t len = strlen(filename);
    if (len > 31) len = 31;
    memcpy(g_data_buf, filename, len);

    Pocket p;
    pocket_prepare(&p);
    pocket_set_data(&p, g_data_buf, 192);
    pocket_add_prefix(&p, DECK_SYSTEM, 0x06);
    pocket_submit(&p);
}

void buf_alloc(uint8_t size_class) {
    g_data_buf[0] = size_class;

    Pocket p;
    pocket_prepare(&p);
    pocket_set_data(&p, g_data_buf, sizeof(size_class));
    pocket_add_prefix(&p, DECK_SYSTEM, 0x10);
    pocket_submit(&p);
}

void buf_release(uint16_t buffer_id) {
    memcpy(g_data_buf, &buffer_id, sizeof(buffer_id));

    Pocket p;
    pocket_prepare(&p);
    pocket_set_data(&p, g_data_buf, sizeof(buffer_id));
    pocket_add_prefix(&p, DECK_SYSTEM, 0x11);
    pocket_submit(&p);
}

static void ptag_common(uint16_t pid, const char* tag, uint8_t opcode) {
    struct PACKED {
        uint16_t pid;
        char tag[32];
    } request;
    request.pid = pid;
    memset(request.tag, 0, sizeof(request.tag));
    size_t len = strlen(tag);
    if (len > 31) len = 31;
    memcpy(request.tag, tag, len);

    memcpy(g_data_buf, &request, sizeof(request));

    Pocket p;
    pocket_prepare(&p);
    pocket_set_data(&p, g_data_buf, sizeof(request));
    pocket_add_prefix(&p, DECK_SYSTEM, opcode);
    pocket_submit(&p);
}

void ptag_add(uint16_t pid, const char* tag) {
    ptag_common(pid, tag, 0x20);
}

void ptag_remove(uint16_t pid, const char* tag) {
    ptag_common(pid, tag, 0x21);
}

void ptag_check(uint16_t pid, const char* tag) {
    ptag_common(pid, tag, 0x22);
}

void fs_defrag(uint32_t file_id, uint32_t target_block) {
    memset(g_data_buf, 0, 192);
    memcpy(g_data_buf, &file_id, 4);
    memcpy(g_data_buf + 4, &target_block, 4);

    Pocket p;
    pocket_prepare(&p);
    pocket_set_data(&p, g_data_buf, 192);
    pocket_add_prefix(&p, DECK_SYSTEM, 0x18);
    pocket_submit(&p);
}

void fs_fraginfo(void) {
    memset(g_data_buf, 0, 192);

    Pocket p;
    pocket_prepare(&p);
    pocket_set_data(&p, g_data_buf, 192);
    pocket_add_prefix(&p, DECK_SYSTEM, 0x19);
    pocket_submit(&p);
}

void obj_create(const char* filename, const char* tags) {
    if (!filename) return;
    struct PACKED {
        char filename[32];
        char tags[160];
    } request;
    memset(&request, 0, sizeof(request));
    size_t fn_len = strlen(filename);
    if (fn_len > 31) fn_len = 31;
    memcpy(request.filename, filename, fn_len);
    if (tags && tags[0] != '\0') {
        size_t tag_len = strlen(tags);
        if (tag_len < 160) {
            memcpy(request.tags, tags, tag_len);
        }
    }
    memcpy(g_data_buf, &request, sizeof(request));

    Pocket p;
    pocket_prepare(&p);
    pocket_set_data(&p, g_data_buf, sizeof(request));
    pocket_add_prefix(&p, DECK_STORAGE, 0x07);
    pocket_submit(&p);
}

void obj_read(uint32_t file_id, uint64_t offset, uint32_t length) {
    struct PACKED {
        uint32_t file_id;
        uint64_t offset;
        uint32_t length;
        uint8_t reserved[176];
    } request;
    memset(&request, 0, sizeof(request));
    request.file_id = file_id;
    request.offset = offset;
    request.length = length;
    memcpy(g_data_buf, &request, sizeof(request));

    Pocket p;
    pocket_prepare(&p);
    pocket_set_data(&p, g_data_buf, sizeof(request));
    pocket_add_prefix(&p, DECK_STORAGE, 0x05);
    pocket_submit(&p);
}

void obj_write(uint32_t file_id, uint64_t offset, const void* buf, uint32_t length) {
    struct PACKED {
        uint32_t file_id;
        uint64_t offset;
        uint32_t length;
        uint32_t flags;
        uint8_t data[168];
        uint8_t reserved[4];
    } request;
    memset(&request, 0, sizeof(request));
    request.file_id = file_id;
    request.offset = offset;
    request.length = length;
    request.flags = 0;
    if (buf && length > 0) {
        uint32_t copy_len = length > 168 ? 168 : length;
        memcpy(request.data, buf, copy_len);
    }
    memcpy(g_data_buf, &request, sizeof(request));

    Pocket p;
    pocket_prepare(&p);
    pocket_set_data(&p, g_data_buf, sizeof(request));
    pocket_add_prefix(&p, DECK_STORAGE, 0x06);
    pocket_submit(&p);
}

void obj_delete(uint32_t file_id) {
    struct PACKED {
        uint32_t file_id;
        uint8_t reserved[188];
    } request;
    memset(&request, 0, sizeof(request));
    request.file_id = file_id;
    memcpy(g_data_buf, &request, sizeof(request));

    Pocket p;
    pocket_prepare(&p);
    pocket_set_data(&p, g_data_buf, sizeof(request));
    pocket_add_prefix(&p, DECK_STORAGE, 0x08);
    pocket_submit(&p);
}

void obj_rename(uint32_t file_id, const char* new_name) {
    if (!new_name) return;
    struct PACKED {
        uint32_t file_id;
        char new_filename[32];
        uint8_t reserved[156];
    } request;
    memset(&request, 0, sizeof(request));
    request.file_id = file_id;
    size_t len = strlen(new_name);
    if (len > 31) len = 31;
    memcpy(request.new_filename, new_name, len);
    memcpy(g_data_buf, &request, sizeof(request));

    Pocket p;
    pocket_prepare(&p);
    pocket_set_data(&p, g_data_buf, sizeof(request));
    pocket_add_prefix(&p, DECK_STORAGE, 0x09);
    pocket_submit(&p);
}

void obj_info(uint32_t file_id) {
    struct PACKED {
        uint32_t file_id;
        uint8_t reserved[188];
    } request;
    memset(&request, 0, sizeof(request));
    request.file_id = file_id;
    memcpy(g_data_buf, &request, sizeof(request));

    Pocket p;
    pocket_prepare(&p);
    pocket_set_data(&p, g_data_buf, sizeof(request));
    pocket_add_prefix(&p, DECK_STORAGE, 0x0A);
    pocket_submit(&p);
}

void obj_query(const char* tags) {
    memset(g_data_buf, 0, 192);
    if (tags && tags[0] != '\0') {
        size_t len = strlen(tags);
        if (len < 192) {
            memcpy(g_data_buf, tags, len);
        }
    }

    Pocket p;
    pocket_prepare(&p);
    pocket_set_data(&p, g_data_buf, 192);
    pocket_add_prefix(&p, DECK_STORAGE, 0x01);
    pocket_submit(&p);
}

void obj_tag_set(uint32_t file_id, const char* tag) {
    if (!tag) return;
    struct PACKED {
        uint32_t file_id;
        char tag[32];
        uint8_t reserved[156];
    } request;
    memset(&request, 0, sizeof(request));
    request.file_id = file_id;
    size_t len = strlen(tag);
    if (len > 31) len = 31;
    memcpy(request.tag, tag, len);
    memcpy(g_data_buf, &request, sizeof(request));

    Pocket p;
    pocket_prepare(&p);
    pocket_set_data(&p, g_data_buf, sizeof(request));
    pocket_add_prefix(&p, DECK_STORAGE, 0x02);
    pocket_submit(&p);
}

void obj_tag_unset(uint32_t file_id, const char* key) {
    if (!key) return;
    struct PACKED {
        uint32_t file_id;
        char tag[32];
        uint8_t reserved[156];
    } request;
    memset(&request, 0, sizeof(request));
    request.file_id = file_id;
    size_t len = strlen(key);
    if (len > 31) len = 31;
    memcpy(request.tag, key, len);
    memcpy(g_data_buf, &request, sizeof(request));

    Pocket p;
    pocket_prepare(&p);
    pocket_set_data(&p, g_data_buf, sizeof(request));
    pocket_add_prefix(&p, DECK_STORAGE, 0x03);
    pocket_submit(&p);
}

void ctx_set(const char* tag) {
    if (!tag) return;
    memset(g_data_buf, 0, 192);
    size_t len = strlen(tag);
    if (len < 192) {
        memcpy(g_data_buf, tag, len);
    }

    Pocket p;
    pocket_prepare(&p);
    pocket_set_data(&p, g_data_buf, 192);
    pocket_add_prefix(&p, DECK_STORAGE, 0x10);
    pocket_submit(&p);
}

void ctx_clear(void) {
    memset(g_data_buf, 0, 192);

    Pocket p;
    pocket_prepare(&p);
    pocket_set_data(&p, g_data_buf, 192);
    pocket_add_prefix(&p, DECK_STORAGE, 0x11);
    pocket_submit(&p);
}

void hw_vga_putchar(uint8_t row, uint8_t col, char c, uint8_t color) {
    g_data_buf[0] = row;
    g_data_buf[1] = col;
    g_data_buf[2] = (uint8_t)c;
    g_data_buf[3] = color;

    Pocket p;
    pocket_prepare(&p);
    pocket_set_data(&p, g_data_buf, 4);
    pocket_add_prefix(&p, DECK_HARDWARE, 0x70);
    pocket_submit(&p);
}

void hw_vga_putstring(const char* str, uint8_t len, uint8_t color) {
    if (len > 188) len = 188;
    memset(g_data_buf, 0, 192);
    g_data_buf[0] = len;
    g_data_buf[1] = color;
    g_data_buf[2] = 0;
    g_data_buf[3] = 0;
    memcpy(g_data_buf + 4, str, len);

    Pocket p;
    pocket_prepare(&p);
    pocket_set_data(&p, g_data_buf, len + 4);
    pocket_add_prefix(&p, DECK_HARDWARE, 0x71);
    pocket_submit(&p);
}

void hw_vga_clear(uint8_t color) {
    g_data_buf[0] = color;

    Pocket p;
    pocket_prepare(&p);
    pocket_set_data(&p, g_data_buf, 1);
    pocket_add_prefix(&p, DECK_HARDWARE, 0x72);
    pocket_submit(&p);
}

void hw_vga_clear_line(uint8_t row, uint8_t color) {
    g_data_buf[0] = row;
    g_data_buf[1] = color;

    Pocket p;
    pocket_prepare(&p);
    pocket_set_data(&p, g_data_buf, 2);
    pocket_add_prefix(&p, DECK_HARDWARE, 0x73);
    pocket_submit(&p);
}

void hw_vga_clear_to_eol(uint8_t color) {
    g_data_buf[0] = color;

    Pocket p;
    pocket_prepare(&p);
    pocket_set_data(&p, g_data_buf, 1);
    pocket_add_prefix(&p, DECK_HARDWARE, 0x74);
    pocket_submit(&p);
}

void hw_vga_scroll(uint8_t lines, uint8_t fill_color) {
    g_data_buf[0] = lines;
    g_data_buf[1] = fill_color;

    Pocket p;
    pocket_prepare(&p);
    pocket_set_data(&p, g_data_buf, 2);
    pocket_add_prefix(&p, DECK_HARDWARE, 0x79);
    pocket_submit(&p);
}

void hw_vga_newline(void) {
    memset(g_data_buf, 0, 4);

    Pocket p;
    pocket_prepare(&p);
    pocket_set_data(&p, g_data_buf, 4);
    pocket_add_prefix(&p, DECK_HARDWARE, 0x7A);
    pocket_submit(&p);
}

void hw_vga_getcursor(void) {
    memset(g_data_buf, 0, 4);

    Pocket p;
    pocket_prepare(&p);
    pocket_set_data(&p, g_data_buf, 4);
    pocket_add_prefix(&p, DECK_HARDWARE, 0x75);
    pocket_submit(&p);
}

void hw_vga_setcursor(uint8_t row, uint8_t col) {
    g_data_buf[0] = row;
    g_data_buf[1] = col;

    Pocket p;
    pocket_prepare(&p);
    pocket_set_data(&p, g_data_buf, 2);
    pocket_add_prefix(&p, DECK_HARDWARE, 0x76);
    pocket_submit(&p);
}

void hw_vga_getcolor(void) {
    memset(g_data_buf, 0, 4);

    Pocket p;
    pocket_prepare(&p);
    pocket_set_data(&p, g_data_buf, 4);
    pocket_add_prefix(&p, DECK_HARDWARE, 0x78);
    pocket_submit(&p);
}

void hw_vga_setcolor(uint8_t color) {
    g_data_buf[0] = color;

    Pocket p;
    pocket_prepare(&p);
    pocket_set_data(&p, g_data_buf, 1);
    pocket_add_prefix(&p, DECK_HARDWARE, 0x77);
    pocket_submit(&p);
}

void hw_vga_getdimensions(void) {
    memset(g_data_buf, 0, 4);

    Pocket p;
    pocket_prepare(&p);
    pocket_set_data(&p, g_data_buf, 4);
    pocket_add_prefix(&p, DECK_HARDWARE, 0x7B);
    pocket_submit(&p);
}

void hw_kb_getchar(void) {
    memset(g_data_buf, 0, 4);

    Pocket p;
    pocket_prepare(&p);
    pocket_set_data(&p, g_data_buf, 4);
    pocket_add_prefix(&p, DECK_HARDWARE, 0x60);
    pocket_submit(&p);
}

void hw_kb_readline(uint8_t max_size, bool echo) {
    memset(g_data_buf, 0, 192);
    g_data_buf[0] = max_size;
    g_data_buf[1] = echo ? 1 : 0;

    Pocket p;
    pocket_prepare(&p);
    pocket_set_data(&p, g_data_buf, 192);
    pocket_add_prefix(&p, DECK_HARDWARE, 0x61);
    pocket_submit(&p);
}

void hw_kb_status(void) {
    memset(g_data_buf, 0, 16);

    Pocket p;
    pocket_prepare(&p);
    pocket_set_data(&p, g_data_buf, 16);
    pocket_add_prefix(&p, DECK_HARDWARE, 0x62);
    pocket_submit(&p);
}

void hw_timer_ms(void) {
    memset(g_data_buf, 0, 8);

    Pocket p;
    pocket_prepare(&p);
    pocket_set_data(&p, g_data_buf, 8);
    pocket_add_prefix(&p, DECK_HARDWARE, 0x11);
    pocket_submit(&p);
}

void hw_rtc_time(void) {
    memset(g_data_buf, 0, 16);

    Pocket p;
    pocket_prepare(&p);
    pocket_set_data(&p, g_data_buf, 16);
    pocket_add_prefix(&p, DECK_HARDWARE, 0x15);
    pocket_submit(&p);
}

void hw_rtc_unix64(void) {
    memset(g_data_buf, 0, 16);

    Pocket p;
    pocket_prepare(&p);
    pocket_set_data(&p, g_data_buf, 16);
    pocket_add_prefix(&p, DECK_HARDWARE, 0x16);
    pocket_submit(&p);
}

void hw_rtc_uptime(void) {
    memset(g_data_buf, 0, 16);

    Pocket p;
    pocket_prepare(&p);
    pocket_set_data(&p, g_data_buf, 16);
    pocket_add_prefix(&p, DECK_HARDWARE, 0x17);
    pocket_submit(&p);
}

void hw_reboot(void) {
    memset(g_data_buf, 0, 192);

    Pocket p;
    pocket_prepare(&p);
    pocket_set_data(&p, g_data_buf, 192);
    pocket_add_prefix(&p, DECK_HARDWARE, 0x80);
    pocket_submit(&p);
}

void hw_shutdown(void) {
    memset(g_data_buf, 0, 192);

    Pocket p;
    pocket_prepare(&p);
    pocket_set_data(&p, g_data_buf, 192);
    pocket_add_prefix(&p, DECK_HARDWARE, 0x81);
    pocket_submit(&p);
}
