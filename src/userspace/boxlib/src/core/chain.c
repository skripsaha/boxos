#include "box/chain.h"
#include "box/notify.h"
#include "box/string.h"

static void ensure_ready(void) {
    notify_page_t* np = notify_page();
    if (np->magic != NOTIFY_MAGIC) {
        notify_prepare();
    }
}

void route(uint32_t target_pid) {
    ensure_ready();
    notify_page_t* np = notify_page();
    np->route_target = target_pid;
    np->route_tag[0] = '\0';
    notify_add_prefix(DECK_SYSTEM, 0x40);
}

void route_tag(const char* tag) {
    if (!tag) return;
    ensure_ready();
    notify_page_t* np = notify_page();
    np->route_target = 0;
    size_t len = strlen(tag);
    if (len > 31) len = 31;
    memcpy(np->route_tag, tag, len);
    np->route_tag[len] = '\0';
    notify_add_prefix(DECK_SYSTEM, 0x41);
}

void hw_listen(uint8_t source_type, uint8_t flags) {
    ensure_ready();
    notify_page_t* np = notify_page();
    np->data[0] = source_type;
    np->data[1] = flags;
    np->data[2] = 0;
    np->data[3] = 0;
    notify_add_prefix(DECK_SYSTEM, 0x42);
}

void proc_kill(uint16_t pid) {
    ensure_ready();
    notify_write_data(&pid, sizeof(pid));
    notify_add_prefix(DECK_SYSTEM, 0x02);
}

void proc_query(uint16_t pid) {
    ensure_ready();
    notify_write_data(&pid, sizeof(pid));
    notify_add_prefix(DECK_SYSTEM, 0x03);
}

void proc_spawn(const char* filename) {
    if (!filename) return;
    ensure_ready();
    uint8_t data[192];
    memset(data, 0, 192);
    size_t len = strlen(filename);
    if (len > 31) len = 31;
    memcpy(data, filename, len);
    notify_write_data(data, 192);
    notify_add_prefix(DECK_SYSTEM, 0x06);
}

void buf_alloc(uint8_t size_class) {
    ensure_ready();
    notify_write_data(&size_class, sizeof(size_class));
    notify_add_prefix(DECK_SYSTEM, 0x10);
}

void buf_release(uint16_t buffer_id) {
    ensure_ready();
    notify_write_data(&buffer_id, sizeof(buffer_id));
    notify_add_prefix(DECK_SYSTEM, 0x11);
}

static void ptag_common(uint16_t pid, const char* tag, uint8_t opcode) {
    ensure_ready();
    struct PACKED {
        uint16_t pid;
        char tag[32];
    } request;
    request.pid = pid;
    memset(request.tag, 0, sizeof(request.tag));
    size_t len = strlen(tag);
    if (len > 31) len = 31;
    memcpy(request.tag, tag, len);
    notify_write_data(&request, sizeof(request));
    notify_add_prefix(DECK_SYSTEM, opcode);
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
    ensure_ready();
    uint8_t data[192];
    memset(data, 0, 192);
    memcpy(data, &file_id, 4);
    memcpy(data + 4, &target_block, 4);
    notify_write_data(data, 192);
    notify_add_prefix(DECK_SYSTEM, 0x18);
}

void fs_fraginfo(void) {
    ensure_ready();
    uint8_t data[192];
    memset(data, 0, 192);
    notify_write_data(data, 192);
    notify_add_prefix(DECK_SYSTEM, 0x19);
}

void obj_create(const char* filename, const char* tags) {
    if (!filename) return;
    ensure_ready();
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
    notify_write_data(&request, sizeof(request));
    notify_add_prefix(DECK_STORAGE, 0x07);
}

void obj_read(uint32_t file_id, uint64_t offset, uint32_t length) {
    ensure_ready();
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
    notify_write_data(&request, sizeof(request));
    notify_add_prefix(DECK_STORAGE, 0x05);
}

void obj_write(uint32_t file_id, uint64_t offset, const void* buf, uint32_t length) {
    ensure_ready();
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
    notify_write_data(&request, sizeof(request));
    notify_add_prefix(DECK_STORAGE, 0x06);
}

void obj_delete(uint32_t file_id) {
    ensure_ready();
    struct PACKED {
        uint32_t file_id;
        uint8_t reserved[188];
    } request;
    memset(&request, 0, sizeof(request));
    request.file_id = file_id;
    notify_write_data(&request, sizeof(request));
    notify_add_prefix(DECK_STORAGE, 0x08);
}

void obj_rename(uint32_t file_id, const char* new_name) {
    if (!new_name) return;
    ensure_ready();
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
    notify_write_data(&request, sizeof(request));
    notify_add_prefix(DECK_STORAGE, 0x09);
}

void obj_info(uint32_t file_id) {
    ensure_ready();
    struct PACKED {
        uint32_t file_id;
        uint8_t reserved[188];
    } request;
    memset(&request, 0, sizeof(request));
    request.file_id = file_id;
    notify_write_data(&request, sizeof(request));
    notify_add_prefix(DECK_STORAGE, 0x0A);
}

void obj_query(const char* tags) {
    ensure_ready();
    char tag_buffer[192];
    memset(tag_buffer, 0, 192);
    if (tags && tags[0] != '\0') {
        size_t len = strlen(tags);
        if (len < 192) {
            memcpy(tag_buffer, tags, len);
        }
    }
    notify_write_data(tag_buffer, 192);
    notify_add_prefix(DECK_STORAGE, 0x01);
}

void obj_tag_set(uint32_t file_id, const char* tag) {
    if (!tag) return;
    ensure_ready();
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
    notify_write_data(&request, sizeof(request));
    notify_add_prefix(DECK_STORAGE, 0x02);
}

void obj_tag_unset(uint32_t file_id, const char* key) {
    if (!key) return;
    ensure_ready();
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
    notify_write_data(&request, sizeof(request));
    notify_add_prefix(DECK_STORAGE, 0x03);
}

void ctx_set(const char* tag) {
    if (!tag) return;
    ensure_ready();
    char tag_buffer[192];
    memset(tag_buffer, 0, 192);
    size_t len = strlen(tag);
    if (len < 192) {
        memcpy(tag_buffer, tag, len);
    }
    notify_write_data(tag_buffer, 192);
    notify_add_prefix(DECK_STORAGE, 0x10);
}

void ctx_clear(void) {
    ensure_ready();
    uint8_t dummy[192];
    memset(dummy, 0, 192);
    notify_write_data(dummy, 192);
    notify_add_prefix(DECK_STORAGE, 0x11);
}

void hw_vga_putchar(uint8_t row, uint8_t col, char c, uint8_t color) {
    ensure_ready();
    uint8_t data[4];
    data[0] = row;
    data[1] = col;
    data[2] = (uint8_t)c;
    data[3] = color;
    notify_write_data(data, 4);
    notify_add_prefix(DECK_HARDWARE, 0x70);
}

void hw_vga_putstring(const char* str, uint8_t len, uint8_t color) {
    if (len > 188) len = 188;
    ensure_ready();
    uint8_t data[192];
    memset(data, 0, 192);
    data[0] = len;
    data[1] = color;
    data[2] = 0;
    data[3] = 0;
    memcpy(data + 4, str, len);
    notify_write_data(data, len + 4);
    notify_add_prefix(DECK_HARDWARE, 0x71);
}

void hw_vga_clear(uint8_t color) {
    ensure_ready();
    notify_write_data(&color, 1);
    notify_add_prefix(DECK_HARDWARE, 0x72);
}

void hw_vga_clear_line(uint8_t row, uint8_t color) {
    ensure_ready();
    uint8_t data[2];
    data[0] = row;
    data[1] = color;
    notify_write_data(data, 2);
    notify_add_prefix(DECK_HARDWARE, 0x73);
}

void hw_vga_clear_to_eol(uint8_t color) {
    ensure_ready();
    notify_write_data(&color, 1);
    notify_add_prefix(DECK_HARDWARE, 0x74);
}

void hw_vga_scroll(uint8_t lines, uint8_t fill_color) {
    ensure_ready();
    uint8_t data[2];
    data[0] = lines;
    data[1] = fill_color;
    notify_write_data(data, 2);
    notify_add_prefix(DECK_HARDWARE, 0x79);
}

void hw_vga_newline(void) {
    ensure_ready();
    notify_add_prefix(DECK_HARDWARE, 0x7A);
}

void hw_vga_getcursor(void) {
    ensure_ready();
    notify_add_prefix(DECK_HARDWARE, 0x75);
}

void hw_vga_setcursor(uint8_t row, uint8_t col) {
    ensure_ready();
    uint8_t data[2];
    data[0] = row;
    data[1] = col;
    notify_write_data(data, 2);
    notify_add_prefix(DECK_HARDWARE, 0x76);
}

void hw_vga_getcolor(void) {
    ensure_ready();
    notify_add_prefix(DECK_HARDWARE, 0x78);
}

void hw_vga_setcolor(uint8_t color) {
    ensure_ready();
    notify_write_data(&color, 1);
    notify_add_prefix(DECK_HARDWARE, 0x77);
}

void hw_vga_getdimensions(void) {
    ensure_ready();
    notify_add_prefix(DECK_HARDWARE, 0x7B);
}

void hw_kb_getchar(void) {
    ensure_ready();
    notify_add_prefix(DECK_HARDWARE, 0x60);
}

void hw_kb_readline(uint8_t max_size, bool echo) {
    ensure_ready();
    uint8_t data[4];
    data[0] = max_size;
    data[1] = echo ? 1 : 0;
    data[2] = 0;
    data[3] = 0;
    notify_write_data(data, 4);
    notify_add_prefix(DECK_HARDWARE, 0x61);
}

void hw_kb_status(void) {
    ensure_ready();
    notify_add_prefix(DECK_HARDWARE, 0x62);
}

void hw_timer_ms(void) {
    ensure_ready();
    notify_add_prefix(DECK_HARDWARE, 0x11);
}

void hw_rtc_time(void) {
    ensure_ready();
    notify_add_prefix(DECK_HARDWARE, 0x15);
}

void hw_rtc_unix64(void) {
    ensure_ready();
    notify_add_prefix(DECK_HARDWARE, 0x16);
}

void hw_rtc_uptime(void) {
    ensure_ready();
    notify_add_prefix(DECK_HARDWARE, 0x17);
}

void hw_reboot(void) {
    ensure_ready();
    uint8_t data[192];
    memset(data, 0, 192);
    notify_write_data(data, 192);
    notify_add_prefix(DECK_HARDWARE, 0x80);
}

void hw_shutdown(void) {
    ensure_ready();
    uint8_t data[192];
    memset(data, 0, 192);
    notify_write_data(data, 192);
    notify_add_prefix(DECK_HARDWARE, 0x81);
}
