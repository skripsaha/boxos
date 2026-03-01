#ifndef BOX_CHAIN_H
#define BOX_CHAIN_H

#include "types.h"

// Chain builders: each function adds one prefix + data; call notify() last to send.
// Example: obj_read(fid, 0, 256); route(target_pid); notify();

void route(uint32_t target_pid);
void route_tag(const char* tag);
void hw_listen(uint8_t source_type, uint8_t flags);

void proc_kill(uint16_t pid);
void proc_query(uint16_t pid);
void proc_spawn(const char* filename);

void buf_alloc(uint8_t size_class);
void buf_release(uint16_t buffer_id);

void ptag_add(uint16_t pid, const char* tag);
void ptag_remove(uint16_t pid, const char* tag);
void ptag_check(uint16_t pid, const char* tag);

void fs_defrag(uint32_t file_id, uint32_t target_block);
void fs_fraginfo(void);

void obj_create(const char* filename, const char* tags);
void obj_read(uint32_t file_id, uint64_t offset, uint32_t length);
void obj_write(uint32_t file_id, uint64_t offset, const void* buf, uint32_t length);
void obj_delete(uint32_t file_id);
void obj_rename(uint32_t file_id, const char* new_name);
void obj_info(uint32_t file_id);
void obj_query(const char* tags);
void obj_tag_set(uint32_t file_id, const char* tag);
void obj_tag_unset(uint32_t file_id, const char* key);
void ctx_set(const char* tag);
void ctx_clear(void);

void hw_vga_putchar(uint8_t row, uint8_t col, char c, uint8_t color);
void hw_vga_putstring(const char* str, uint8_t len, uint8_t color);
void hw_vga_clear(uint8_t color);
void hw_vga_clear_line(uint8_t row, uint8_t color);
void hw_vga_clear_to_eol(uint8_t color);
void hw_vga_scroll(uint8_t lines, uint8_t fill_color);
void hw_vga_newline(void);
void hw_vga_getcursor(void);
void hw_vga_setcursor(uint8_t row, uint8_t col);
void hw_vga_getcolor(void);
void hw_vga_setcolor(uint8_t color);
void hw_vga_getdimensions(void);

void hw_kb_getchar(void);
void hw_kb_readline(uint8_t max_size, bool echo);
void hw_kb_status(void);

void hw_timer_ms(void);
void hw_rtc_time(void);
void hw_rtc_unix64(void);
void hw_rtc_uptime(void);

void hw_reboot(void);
void hw_shutdown(void);

#endif // BOX_CHAIN_H
