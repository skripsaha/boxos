#include "vga.h"
#include "io.h"
#include "klib.h"
#include "serial.h"

unsigned char *vga = (unsigned char*)VGA;

uint8_t vga_attr_backup[VGA_WIDTH * VGA_HEIGHT];

// Current text color - used by all print functions
uint8_t vga_current_color = TEXT_ATTR_DEFAULT;

unsigned int current_loc = 0;
unsigned int last_loc = 0;

unsigned int current_line = 0;
unsigned int line_size = VGA_WIDTH * BYTES_FOR_EACH_ELEMENT;

// Color management functions
void vga_set_color(uint8_t color) {
    vga_current_color = color;
}

uint8_t vga_get_color(void) {
    return vga_current_color;
}

void vga_reset_color(void) {
    vga_current_color = TEXT_ATTR_DEFAULT;
}

void vga_init(void){
    vga_clear_screen();
    vga_set_cursor_position(0, 0);
}

void vga_print_char(char ch, const unsigned char attr) {

    // Fixed: Check for buffer overflow before writing (need space for char + attr)
    if(current_loc >= VGA_SIZE - 2) vga_scroll_up();

    vga[current_loc] = ch;
    vga[current_loc + 1] = attr;

    vga_attr_backup[current_loc / 2] = attr;

    current_loc += 2;
    vga_update_cursor();
}



void vga_print(const char *str){
    while(*str){
        if(*str == '\n'){
            vga_print_newline();
            str++;
            continue;
        }
        vga_print_char(*str, vga_current_color);
        str++;
    }
    vga_update_cursor();
}

void vga_print_newline(void){
    int y = vga_get_cursor_position_y();

    if (y + 1 >= VGA_HEIGHT) {
        vga_scroll_up();
        current_loc = (VGA_HEIGHT - 1) * line_size;
    } else {
        current_loc = (y + 1) * line_size;
    }
}


void vga_clear_screen(void){
    for(int i= 0; i < VGA_SIZE; i += 2){
        vga[i] = ' ';
        vga[i+1] = TEXT_ATTR_DEFAULT;
    }
    current_loc = 0;
}

void vga_clear_line(int line) {
    if(line < 0 || line >= VGA_HEIGHT) return;
    unsigned int line_start = line * line_size;
    for(unsigned int i = line_start; i < line_start + line_size; i += 2) {
        vga[i] = ' ';
        vga[i+1] = TEXT_ATTR_DEFAULT;
    }
}

void vga_clear_to_eol(void) {
    int x = vga_get_cursor_position_x();
    int y = vga_get_cursor_position_y();

    if (x < 0 || x >= VGA_WIDTH || y < 0 || y >= VGA_HEIGHT) {
        vga_print_error("vga_clear_to_eol: Invalid cursor pos");
        return;
    }

    int idx = (y * VGA_WIDTH + x) * 2;
    int end = (y * VGA_WIDTH + VGA_WIDTH) * 2;

    for (; idx < end; idx += 2) {
        vga[idx] = ' ';
        vga[idx + 1] = TEXT_ATTR_DEFAULT;
    }
}





void vga_print_error(const char *str){
    while(*str){
        if(*str == '\n'){
            vga_print_newline();
            str++;
            continue;
        }
        vga_print_char(*str, TEXT_ATTR_ERROR);
        str++;
    }
    vga_update_cursor(); 
}
void vga_print_success(const char *str){
    while(*str){
        if(*str == '\n'){
            vga_print_newline();
            str++;
            continue;
        }
        vga_print_char(*str, TEXT_ATTR_SUCCESS);
        str++;
    }
    vga_update_cursor(); 
}

void vga_print_hint(const char *str){
    while(*str){
        if(*str == '\n'){
            vga_print_newline();
            str++;
            continue;
        }
        vga_print_char(*str, TEXT_ATTR_HINT);
        str++;
    }
    vga_update_cursor();
}

void vga_scroll_up(void){
    for (unsigned int i = 0; i < VGA_SIZE - line_size; i += 2) {
        vga[i] = vga[i + line_size];
        vga[i + 1] = vga[i + line_size + 1];

        uint16_t to = i / 2;
        uint16_t from = (i + line_size) / 2;
        vga_attr_backup[to] = vga_attr_backup[from];
    }
    for (unsigned int i = VGA_SIZE - line_size; i < VGA_SIZE; i += 2) {
        vga[i] = ' ';
        vga[i + 1] = TEXT_ATTR_DEFAULT;

        vga_attr_backup[i / 2] = TEXT_ATTR_DEFAULT;
    }
    if (current_loc >= line_size) {
        current_loc -= line_size;
    } else {
        current_loc = 0;
    }
}

void vga_change_background(unsigned char new_bg_color) {
    new_bg_color &= 0xF0;
    
    for(unsigned int i = 1; i < VGA_SIZE; i += 2) {
        // Get current attribute
        unsigned char current_attr = vga[i];
        
        vga[i] = (current_attr & 0x0F) | new_bg_color;

        vga_attr_backup[i / 2] = vga[i];
    }
}

void vga_update_cursor(void){
    if (last_loc < VGA_SIZE) {
        uint16_t last_pos = last_loc / 2;
        vga[last_loc + 1] = vga_attr_backup[last_pos];
    }

    last_loc = current_loc;

    uint16_t pos = current_loc / 2;

    outb(0x3D4, 0x0F);
    outb(0x3D5, (uint8_t)(pos & 0xFF));
    outb(0x3D4, 0x0E);
    outb(0x3D5, (uint8_t)((pos >> 8) & 0xFF));
}


void vga_set_cursor_position(int x, int y){
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x >= VGA_WIDTH) x = VGA_WIDTH - 1;
    if (y >= VGA_HEIGHT) y = VGA_HEIGHT - 1;

    current_loc = y * line_size + x * BYTES_FOR_EACH_ELEMENT;
    vga_update_cursor();
}

int vga_get_cursor_position_x() {
    return (current_loc / 2) % VGA_WIDTH;
}

int vga_get_cursor_position_y() {
    return (current_loc / 2) / VGA_WIDTH;
}
