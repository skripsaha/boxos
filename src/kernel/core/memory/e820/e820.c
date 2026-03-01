#include "e820.h"

static e820_entry_t* e820_entries = (e820_entry_t*)0x500;
static size_t e820_entry_count = 0;

void e820_set_entries(e820_entry_t* entries, size_t count) {
    if (count > 128) {
        count = 128;
    }
    e820_entries = entries;
    e820_entry_count = count;
}

e820_entry_t* memory_map_get_entries(void) {
    return e820_entries;
}

size_t memory_map_get_entry_count(void) {
    return e820_entry_count;
}
