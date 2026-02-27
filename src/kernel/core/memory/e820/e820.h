#ifndef E820_H
#define E820_H

#include "ktypes.h"

/* E820 Memory Types */
#define E820_USABLE      1
#define E820_RESERVED    2
#define E820_ACPI_RECL   3
#define E820_ACPI_NVS    4
#define E820_BAD         5

/* Структура для E820 memory map */
typedef struct {
    uint64_t base;
    uint64_t length;
    uint32_t type;
    uint32_t acpi;
} __attribute__((packed)) e820_entry_t;

void e820_set_entries(e820_entry_t* entries, size_t count);
e820_entry_t* memory_map_get_entries(void);
size_t memory_map_get_entry_count(void);


#endif // E820_H