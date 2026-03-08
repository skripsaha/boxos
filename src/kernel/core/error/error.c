#include "error.h"

const char* error_string(error_t err) {
    switch (err) {
        case OK:
            return "Success";

        case ERR_UNKNOWN:
            return "Unknown error";
        case ERR_NOT_IMPLEMENTED:
            return "Feature not implemented";
        case ERR_INVALID_ARGUMENT:
            return "Invalid argument";
        case ERR_NULL_POINTER:
            return "Null pointer";
        case ERR_OUT_OF_RANGE:
            return "Value out of range";
        case ERR_BUFFER_TOO_SMALL:
            return "Buffer too small";
        case ERR_TIMEOUT:
            return "Operation timed out";
        case ERR_BUSY:
            return "Resource busy";
        case ERR_WOULD_BLOCK:
            return "Operation would block";
        case ERR_ALIGNMENT:
            return "Alignment error";
        case ERR_CORRUPTED:
            return "Data corrupted";
        case ERR_INTERNAL:
            return "Internal error";

        case ERR_NO_MEMORY:
            return "Out of memory";
        case ERR_INVALID_ADDRESS:
            return "Invalid memory address";
        case ERR_PAGE_FAULT:
            return "Page fault";
        case ERR_ALREADY_MAPPED:
            return "Memory already mapped";
        case ERR_NOT_MAPPED:
            return "Memory not mapped";
        case ERR_PERMISSION_DENIED_MEM:
            return "Memory access permission denied";
        case ERR_HEAP_EXHAUSTED:
            return "Heap exhausted";
        case ERR_STACK_OVERFLOW:
            return "Stack overflow";
        case ERR_BUFFER_OVERFLOW:
            return "Buffer overflow";
        case ERR_INVALID_BUFFER_ID:
            return "Invalid buffer ID";
        case ERR_BUFFER_IN_USE:
            return "Buffer in use";
        case ERR_BUFFER_LIMIT_EXCEEDED:
            return "Buffer limit exceeded";

        case ERR_IO:
            return "I/O error";
        case ERR_READ_FAILED:
            return "Read operation failed";
        case ERR_WRITE_FAILED:
            return "Write operation failed";
        case ERR_DEVICE_NOT_READY:
            return "Device not ready";
        case ERR_DEVICE_ERROR:
            return "Device error";
        case ERR_END_OF_FILE:
            return "End of file";
        case ERR_DISK_FULL:
            return "Disk full";
        case ERR_BAD_SECTOR:
            return "Bad disk sector";
        case ERR_IO_PENDING:
            return "I/O operation pending";
        case ERR_IO_QUEUE_FULL:
            return "I/O queue full";
        case ERR_IO_CANCELLED:
            return "I/O request cancelled";

        case ERR_FILE_NOT_FOUND:
            return "File not found";
        case ERR_OBJECT_NOT_FOUND:
            return "Object not found";
        case ERR_TAG_NOT_FOUND:
            return "Tag not found";
        case ERR_ALREADY_EXISTS:
            return "Object already exists";
        case ERR_INVALID_TAG:
            return "Invalid tag";
        case ERR_TAG_LIMIT_EXCEEDED:
            return "Tag limit exceeded";
        case ERR_OBJECT_CORRUPTED:
            return "Object corrupted";
        case ERR_JOURNAL_FULL:
            return "Journal full";
        case ERR_JOURNAL_CORRUPTED:
            return "Journal corrupted";
        case ERR_METADATA_CORRUPTED:
            return "Metadata corrupted";

        case ERR_PROCESS_NOT_FOUND:
            return "Process not found";
        case ERR_INVALID_PID:
            return "Invalid PID";
        case ERR_PROCESS_LIMIT_EXCEEDED:
            return "Process limit exceeded";
        case ERR_PROCESS_TERMINATED:
            return "Process terminated";
        case ERR_PROCESS_BLOCKED:
            return "Process blocked";
        case ERR_INVALID_ELF:
            return "Invalid ELF binary";
        case ERR_BINARY_TOO_LARGE:
            return "Binary too large";
        case ERR_SPAWN_FAILED:
            return "Process spawn failed";

        case ERR_ACCESS_DENIED:
            return "Access denied";
        case ERR_PERMISSION_DENIED:
            return "Permission denied";
        case ERR_SECURITY_VIOLATION:
            return "Security violation";
        case ERR_TAG_MISMATCH:
            return "Tag mismatch";
        case ERR_INVALID_OPERATION:
            return "Invalid operation";

        case ERR_HARDWARE:
            return "Hardware error";
        case ERR_INVALID_DEVICE:
            return "Invalid device";
        case ERR_DEVICE_BUSY:
            return "Device busy";
        case ERR_KEYBOARD_BUFFER_FULL:
            return "Keyboard buffer full";
        case ERR_VGA_ERROR:
            return "VGA error";
        case ERR_ATA_ERROR:
            return "ATA error";
        case ERR_PCI_ERROR:
            return "PCI error";

        case ERR_ACPI_NOT_FOUND:
            return "ACPI tables not found";
        case ERR_ACPI_INVALID_TABLE:
            return "Invalid ACPI table";
        case ERR_ACPI_CHECKSUM_FAILED:
            return "ACPI checksum failed";
        case ERR_ACPI_PARSE_ERROR:
            return "ACPI parse error";

        case ERR_POCKET_RING_FULL:
            return "Pocket ring buffer full";
        case ERR_RESULT_RING_FULL:
            return "Result ring buffer full";
        case ERR_INVALID_POCKET:
            return "Invalid pocket";
        case ERR_INVALID_DECK_ID:
            return "Invalid deck ID";
        case ERR_INVALID_OPCODE:
            return "Invalid opcode";
        case ERR_PREFIX_CHAIN_TOO_LONG:
            return "Prefix chain too long";
        case ERR_POCKET_PROCESSING_FAILED:
            return "Pocket processing failed";
        case ERR_PENDING_QUEUE_FULL:
            return "Pending queue full";
        case ERR_POCKET_FAILED:
            return "Pocket failed";

        case ERR_ROUTE_TARGET_FULL:
            return "Route target queue full";
        case ERR_ROUTE_NO_SUBSCRIBERS:
            return "No subscribers for route";
        case ERR_ROUTE_SELF:
            return "Cannot route to self";
        case ERR_LISTEN_TABLE_FULL:
            return "Listen table full";
        case ERR_LISTEN_ALREADY:
            return "Already listening on route";

        default:
            return "Unknown error code";
    }
}
