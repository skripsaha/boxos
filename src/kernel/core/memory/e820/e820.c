#include "e820.h"
#include "vmm.h"
#include "klib.h"

static e820_entry_t* e820_entries = (e820_entry_t*)0x500;
static uintptr_t e820_entries_phys = 0x500;
static size_t e820_entry_count = 0;

void e820_set_entries(e820_entry_t* entries, size_t count) {
    if (count > E820_MAX_ENTRIES) {
        kprintf("[E820] WARNING: BIOS reported %zu entries, truncating to %d\n",
                count, E820_MAX_ENTRIES);
        count = E820_MAX_ENTRIES;
    }
    e820_entries = entries;
    e820_entries_phys = (uintptr_t)entries;
    e820_entry_count = count;
}

void e820_activate_pull_map(void) {
    e820_entries = (e820_entry_t*)vmm_phys_to_virt(e820_entries_phys);
    debug_printf("[E820] Entries rebased to Pull Map: %p (phys: 0x%lx)\n",
                 e820_entries, e820_entries_phys);
}

e820_entry_t* memory_map_get_entries(void) {
    return e820_entries;
}

size_t memory_map_get_entry_count(void) {
    return e820_entry_count;
}
