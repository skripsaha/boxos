#ifndef DEBUG_H
#define DEBUG_H

#include "klib.h"

// ============================================================================
// BOXOS DEBUG MACROS
// ============================================================================
// Standardized debug output format: [SUBSYSTEM] message
// Subsystem tags should be 3-8 characters for alignment consistency

// Debug levels
#define DEBUG_LEVEL_INFO  0
#define DEBUG_LEVEL_WARN  1
#define DEBUG_LEVEL_ERROR 2

// Standard debug output (INFO level)
#define debug_info(tag, fmt, ...) \
    debug_printf("[" tag "] " fmt "\n", ##__VA_ARGS__)

// Warning output
#define debug_warn(tag, fmt, ...) \
    debug_printf("[" tag ":WARN] " fmt "\n", ##__VA_ARGS__)

// Error output
#define debug_error(tag, fmt, ...) \
    debug_printf("[" tag ":ERROR] " fmt "\n", ##__VA_ARGS__)

// Subsystem tags (for consistency)
#define DEBUG_TAG_PMM      "PMM"
#define DEBUG_TAG_VMM      "VMM"
#define DEBUG_TAG_PROCESS  "PROCESS"
#define DEBUG_TAG_SCHEDULER "SCHED"
#define DEBUG_TAG_GUIDE    "GUIDE"
#define DEBUG_TAG_TAGFS    "TAGFS"
#define DEBUG_TAG_JOURNAL  "JOURNAL"
#define DEBUG_TAG_ATA      "ATA"
#define DEBUG_TAG_AHCI     "AHCI"
#define DEBUG_TAG_XHCI     "xHCI"
#define DEBUG_TAG_USB      "USB"
#define DEBUG_TAG_ACPI     "ACPI"
#define DEBUG_TAG_VGA      "VGA"
#define DEBUG_TAG_KEYBOARD "KEYBOARD"
#define DEBUG_TAG_SERIAL   "SERIAL"
#define DEBUG_TAG_PIT      "PIT"
#define DEBUG_TAG_IDT      "IDT"
#define DEBUG_TAG_GDT      "GDT"
#define DEBUG_TAG_PIC      "PIC"

#endif // DEBUG_H
