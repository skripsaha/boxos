#include "error.h"

const char* ErrorString(error_t err) {
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
        case ERR_NOT_INITIALIZED:
            return "Not initialized";
        case ERR_ALREADY_INITIALIZED:
            return "Already initialized";
        case ERR_INVALID_STATE:
            return "Invalid state";
        case ERR_RETRY:
            return "Retry operation";
        case ERR_UNSUPPORTED:
            return "Unsupported operation";
        case ERR_VERSION_MISMATCH:
            return "Version mismatch";
        case ERR_CHECKSUM:
            return "Checksum failed";

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
        case ERR_MAPPING_FAILED:
            return "Memory mapping failed";
        case ERR_UNMAP_FAILED:
            return "Memory unmapping failed";
        case ERR_TLB_FLUSH_FAILED:
            return "TLB flush failed";
        case ERR_PHYS_ADDR_EXCEEDED:
            return "Physical address exceeded MAXPHYADDR";
        case ERR_PCID_EXHAUSTED:
            return "PCID pool exhausted";

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
        case ERR_DMA_FAILED:
            return "DMA operation failed";
        case ERR_DMA_TIMEOUT:
            return "DMA timeout";
        case ERR_SECTOR_READ_FAILED:
            return "Sector read failed";
        case ERR_SECTOR_WRITE_FAILED:
            return "Sector write failed";

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
        case ERR_TAG_REGISTRY_FULL:
            return "Tag registry full";
        case ERR_FILE_TABLE_CORRUPT:
            return "File table corrupt";
        case ERR_METADATA_POOL_FULL:
            return "Metadata pool full";
        case ERR_BLOCK_ALLOC_FAILED:
            return "Block allocation failed";
        case ERR_BLOCK_FREE_FAILED:
            return "Block free failed";
        case ERR_FILE_HANDLE_INVALID:
            return "Invalid file handle";
        case ERR_FILE_HANDLE_IN_USE:
            return "File handle in use";
        case ERR_EXTENT_INVALID:
            return "Invalid extent";
        case ERR_BITMAP_FULL:
            return "Bitmap full";
        case ERR_SUPERBLOCK_INVALID:
            return "Invalid superblock";
        case ERR_BOOT_HINTS_MISSING:
            return "Boot hints missing";
        case ERR_SNAPSHOT_LIMIT:
            return "Snapshot limit reached";
        case ERR_SNAPSHOT_NOT_FOUND:
            return "Snapshot not found";
        case ERR_DEDUP_FAILED:
            return "Deduplication failed";
        case ERR_SELF_HEAL_FAILED:
            return "Self-healing failed";

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
        case ERR_PROCESS_RUNNING:
            return "Process is running";
        case ERR_PROCESS_DESTROYING:
            return "Process is being destroyed";
        case ERR_CABIN_CREATE_FAILED:
            return "Cabin creation failed";
        case ERR_PID_EXHAUSTED:
            return "PID pool exhausted";
        case ERR_TAG_OVERFLOW_FAILED:
            return "Tag overflow allocation failed";
        case ERR_FPU_INIT_FAILED:
            return "FPU initialization failed";
        case ERR_STACK_ALLOC_FAILED:
            return "Stack allocation failed";

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
        case ERR_PRIVILEGE_REQUIRED:
            return "Privilege required";
        case ERR_SANDBOX_VIOLATION:
            return "Sandbox violation";

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
        case ERR_USB_ERROR:
            return "USB error";
        case ERR_XHCI_ERROR:
            return "xHCI error";
        case ERR_AHCI_ERROR:
            return "AHCI error";
        case ERR_TIMER_ERROR:
            return "Timer error";
        case ERR_INTERRUPT_ERROR:
            return "Interrupt error";
        case ERR_CPU_ERROR:
            return "CPU error";

        case ERR_ACPI_NOT_FOUND:
            return "ACPI tables not found";
        case ERR_ACPI_INVALID_TABLE:
            return "Invalid ACPI table";
        case ERR_ACPI_CHECKSUM_FAILED:
            return "ACPI checksum failed";
        case ERR_ACPI_PARSE_ERROR:
            return "ACPI parse error";
        case ERR_ACPI_MADT_NOT_FOUND:
            return "ACPI MADT not found";
        case ERR_ACPI_FADT_NOT_FOUND:
            return "ACPI FADT not found";

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
        case ERR_KCORE_QUEUE_FULL:
            return "K-Core queue full";
        case ERR_KCORE_SUBMIT_FAILED:
            return "K-Core submit failed";
        case ERR_RESULT_NOT_READY:
            return "Result not ready";
        case ERR_RESULT_STASH_FULL:
            return "Result stash full";

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
        case ERR_ROUTE_INVALID_TAG:
            return "Invalid route tag";

        case ERR_POCKET_FAILED:
            return "Pocket failed";

        case ERR_SCHEDULER_LOCKED:
            return "Scheduler locked";
        case ERR_RUNQUEUE_FULL:
            return "RunQueue full";
        case ERR_RUNQUEUE_EMPTY:
            return "RunQueue empty";
        case ERR_HOME_CORE_INVALID:
            return "Invalid home core";
        case ERR_WORK_STEAL_FAILED:
            return "Work stealing failed";

        case ERR_BOOT_INFO_INVALID:
            return "Invalid boot info";
        case ERR_E820_FAILED:
            return "E820 memory detection failed";
        case ERR_A20_FAILED:
            return "A20 gate failed";
        case ERR_LONG_MODE_FAILED:
            return "Long mode failed";
        case ERR_KERNEL_LOAD_FAILED:
            return "Kernel load failed";

        default:
            return "Unknown error code";
    }
}
