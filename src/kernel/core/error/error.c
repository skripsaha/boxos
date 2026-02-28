#include "error.h"

const char* boxos_error_string(boxos_error_t err) {
    switch (err) {
        // Success
        case BOXOS_OK:
            return "Success";

        // General Errors (1-99)
        case BOXOS_ERR_UNKNOWN:
            return "Unknown error";
        case BOXOS_ERR_NOT_IMPLEMENTED:
            return "Feature not implemented";
        case BOXOS_ERR_INVALID_ARGUMENT:
            return "Invalid argument";
        case BOXOS_ERR_NULL_POINTER:
            return "Null pointer";
        case BOXOS_ERR_OUT_OF_RANGE:
            return "Value out of range";
        case BOXOS_ERR_BUFFER_TOO_SMALL:
            return "Buffer too small";
        case BOXOS_ERR_TIMEOUT:
            return "Operation timed out";
        case BOXOS_ERR_BUSY:
            return "Resource busy";
        case BOXOS_ERR_WOULD_BLOCK:
            return "Operation would block";
        case BOXOS_ERR_ALIGNMENT:
            return "Alignment error";
        case BOXOS_ERR_CORRUPTED:
            return "Data corrupted";
        case BOXOS_ERR_INTERNAL:
            return "Internal error";

        // Memory Errors (100-199)
        case BOXOS_ERR_NO_MEMORY:
            return "Out of memory";
        case BOXOS_ERR_INVALID_ADDRESS:
            return "Invalid memory address";
        case BOXOS_ERR_PAGE_FAULT:
            return "Page fault";
        case BOXOS_ERR_ALREADY_MAPPED:
            return "Memory already mapped";
        case BOXOS_ERR_NOT_MAPPED:
            return "Memory not mapped";
        case BOXOS_ERR_PERMISSION_DENIED_MEM:
            return "Memory access permission denied";
        case BOXOS_ERR_HEAP_EXHAUSTED:
            return "Heap exhausted";
        case BOXOS_ERR_STACK_OVERFLOW:
            return "Stack overflow";
        case BOXOS_ERR_BUFFER_OVERFLOW:
            return "Buffer overflow";
        case BOXOS_ERR_INVALID_BUFFER_ID:
            return "Invalid buffer ID";
        case BOXOS_ERR_BUFFER_IN_USE:
            return "Buffer in use";
        case BOXOS_ERR_BUFFER_LIMIT_EXCEEDED:
            return "Buffer limit exceeded";

        // I/O Errors (200-299)
        case BOXOS_ERR_IO:
            return "I/O error";
        case BOXOS_ERR_READ_FAILED:
            return "Read operation failed";
        case BOXOS_ERR_WRITE_FAILED:
            return "Write operation failed";
        case BOXOS_ERR_DEVICE_NOT_READY:
            return "Device not ready";
        case BOXOS_ERR_DEVICE_ERROR:
            return "Device error";
        case BOXOS_ERR_END_OF_FILE:
            return "End of file";
        case BOXOS_ERR_DISK_FULL:
            return "Disk full";
        case BOXOS_ERR_BAD_SECTOR:
            return "Bad disk sector";
        case BOXOS_ERR_IO_PENDING:
            return "I/O operation pending";
        case BOXOS_ERR_IO_QUEUE_FULL:
            return "I/O queue full";
        case BOXOS_ERR_IO_CANCELLED:
            return "I/O request cancelled";

        // Filesystem Errors (300-399)
        case BOXOS_ERR_FILE_NOT_FOUND:
            return "File not found";
        case BOXOS_ERR_OBJECT_NOT_FOUND:
            return "Object not found";
        case BOXOS_ERR_TAG_NOT_FOUND:
            return "Tag not found";
        case BOXOS_ERR_ALREADY_EXISTS:
            return "Object already exists";
        case BOXOS_ERR_INVALID_TAG:
            return "Invalid tag";
        case BOXOS_ERR_TAG_LIMIT_EXCEEDED:
            return "Tag limit exceeded";
        case BOXOS_ERR_OBJECT_CORRUPTED:
            return "Object corrupted";
        case BOXOS_ERR_JOURNAL_FULL:
            return "Journal full";
        case BOXOS_ERR_JOURNAL_CORRUPTED:
            return "Journal corrupted";
        case BOXOS_ERR_METADATA_CORRUPTED:
            return "Metadata corrupted";

        // Process Errors (400-499)
        case BOXOS_ERR_PROCESS_NOT_FOUND:
            return "Process not found";
        case BOXOS_ERR_INVALID_PID:
            return "Invalid PID";
        case BOXOS_ERR_PROCESS_LIMIT_EXCEEDED:
            return "Process limit exceeded";
        case BOXOS_ERR_PROCESS_TERMINATED:
            return "Process terminated";
        case BOXOS_ERR_PROCESS_BLOCKED:
            return "Process blocked";
        case BOXOS_ERR_INVALID_ELF:
            return "Invalid ELF binary";
        case BOXOS_ERR_BINARY_TOO_LARGE:
            return "Binary too large";
        case BOXOS_ERR_SPAWN_FAILED:
            return "Process spawn failed";

        // Security Errors (500-599)
        case BOXOS_ERR_ACCESS_DENIED:
            return "Access denied";
        case BOXOS_ERR_PERMISSION_DENIED:
            return "Permission denied";
        case BOXOS_ERR_SECURITY_VIOLATION:
            return "Security violation";
        case BOXOS_ERR_TAG_MISMATCH:
            return "Tag mismatch";
        case BOXOS_ERR_INVALID_OPERATION:
            return "Invalid operation";

        // Hardware Errors (600-699)
        case BOXOS_ERR_HARDWARE:
            return "Hardware error";
        case BOXOS_ERR_INVALID_DEVICE:
            return "Invalid device";
        case BOXOS_ERR_DEVICE_BUSY:
            return "Device busy";
        case BOXOS_ERR_KEYBOARD_BUFFER_FULL:
            return "Keyboard buffer full";
        case BOXOS_ERR_VGA_ERROR:
            return "VGA error";
        case BOXOS_ERR_ATA_ERROR:
            return "ATA error";
        case BOXOS_ERR_PCI_ERROR:
            return "PCI error";

        // ACPI Errors (800-899)
        case BOXOS_ERR_ACPI_NOT_FOUND:
            return "ACPI tables not found";
        case BOXOS_ERR_ACPI_INVALID_TABLE:
            return "Invalid ACPI table";
        case BOXOS_ERR_ACPI_CHECKSUM_FAILED:
            return "ACPI checksum failed";
        case BOXOS_ERR_ACPI_PARSE_ERROR:
            return "ACPI parse error";

        // Event System Errors (900-999)
        case BOXOS_ERR_EVENT_RING_FULL:
            return "Event ring buffer full";
        case BOXOS_ERR_RESULT_RING_FULL:
            return "Result ring buffer full";
        case BOXOS_ERR_INVALID_EVENT:
            return "Invalid event";
        case BOXOS_ERR_INVALID_DECK_ID:
            return "Invalid deck ID";
        case BOXOS_ERR_INVALID_OPCODE:
            return "Invalid opcode";
        case BOXOS_ERR_PREFIX_CHAIN_TOO_LONG:
            return "Prefix chain too long";
        case BOXOS_ERR_EVENT_PROCESSING_FAILED:
            return "Event processing failed";
        case BOXOS_ERR_PENDING_QUEUE_FULL:
            return "Pending queue full";

        // Routing/IPC Errors
        case BOXOS_ERR_ROUTE_TARGET_FULL:
            return "Route target queue full";
        case BOXOS_ERR_ROUTE_NO_SUBSCRIBERS:
            return "No subscribers for route";
        case BOXOS_ERR_ROUTE_SELF:
            return "Cannot route to self";
        case BOXOS_ERR_LISTEN_TABLE_FULL:
            return "Listen table full";
        case BOXOS_ERR_LISTEN_ALREADY:
            return "Already listening on route";

        default:
            return "Unknown error code";
    }
}
