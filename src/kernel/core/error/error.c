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

        // TagFS Core Errors
        case ERR_TAGFS_NOT_INITIALIZED:
            return "TagFS not initialized";
        case ERR_TAGFS_CORRUPTED:
            return "TagFS corrupted";
        case ERR_TAGFS_NO_SPACE:
            return "TagFS no space";
        case ERR_TAGFS_FILE_NOT_FOUND:
            return "TagFS file not found";
        case ERR_TAGFS_FILE_EXISTS:
            return "TagFS file exists";
        case ERR_TAGFS_INVALID_HANDLE:
            return "TagFS invalid handle";
        case ERR_TAGFS_READ_ONLY:
            return "TagFS read only";
        case ERR_TAGFS_QUOTA_EXCEEDED:
            return "TagFS quota exceeded";
        case ERR_TAGFS_METADATA_ERROR:
            return "TagFS metadata error";
        case ERR_TAGFS_BITMAP_FULL:
            return "TagFS bitmap full";
        case ERR_TAGFS_REGISTRY_FULL:
            return "TagFS registry full";
        case ERR_TAGFS_TAG_NOT_FOUND:
            return "TagFS tag not found";
        case ERR_TAGFS_TAG_EXISTS:
            return "TagFS tag exists";
        case ERR_TAGFS_INVALID_TAG:
            return "TagFS invalid tag";
        case ERR_TAGFS_EXTENT_ERROR:
            return "TagFS extent error";
        case ERR_TAGFS_RECOVERY_FAILED:
            return "TagFS recovery failed";

        // TagFS Module Errors
        case ERR_DISKBOOK_NOT_INITIALIZED:
            return "DiskBook not initialized";
        case ERR_DISKBOOK_FULL:
            return "DiskBook journal full";
        case ERR_DISKBOOK_CORRUPTED:
            return "DiskBook journal corrupted";
        case ERR_DISKBOOK_COMMIT_FAILED:
            return "DiskBook commit failed";
        case ERR_DISKBOOK_CHECKPOINT_FAILED:
            return "DiskBook checkpoint failed";
        case ERR_DISKBOOK_REPLAY_FAILED:
            return "DiskBook replay failed";
        case ERR_DISKBOOK_INVALID_TXN:
            return "DiskBook invalid transaction";
        case ERR_DISKBOOK_WRITE_FAILED:
            return "DiskBook write failed";
        case ERR_DISKBOOK_READ_FAILED:
            return "DiskBook read failed";
        
        case ERR_COW_NOT_INITIALIZED:
            return "CoW snapshots not initialized";
        case ERR_COW_SNAPSHOT_EXISTS:
            return "Snapshot already exists";
        case ERR_COW_SNAPSHOT_NOT_FOUND:
            return "Snapshot not found";
        case ERR_COW_SNAPSHOT_LIMIT:
            return "Snapshot limit reached";
        case ERR_COW_ALLOCATION_FAILED:
            return "CoW allocation failed";
        case ERR_COW_RESTORE_FAILED:
            return "Snapshot restore failed";
        
        case ERR_DEDUP_NOT_INITIALIZED:
            return "Dedup not initialized";
        case ERR_DEDUP_HASH_COLLISION:
            return "Dedup hash collision";
        case ERR_DEDUP_POOL_EXHAUSTED:
            return "Dedup entry pool exhausted";
        case ERR_DEDUP_GC_FAILED:
            return "Dedup GC failed";
        case ERR_DEDUP_REGISTER_FAILED:
            return "Dedup register failed";
        
        case ERR_SELF_HEAL_NOT_INITIALIZED:
            return "Self-heal not initialized";
        case ERR_SELF_HEAL_CORRUPTION_DETECTED:
            return "Self-heal corruption detected";
        case ERR_SELF_HEAL_RECOVERY_FAILED:
            return "Self-heal recovery failed";
        case ERR_SELF_HEAL_MIRROR_FAILED:
            return "Self-heal mirror failed";
        case ERR_SELF_HEAL_SCRUB_FAILED:
            return "Self-heal scrub failed";
        
        case ERR_BOXHASH_INVALID_CONTEXT:
            return "BoxHash invalid context";
        case ERR_BOXHASH_VERIFICATION_FAILED:
            return "BoxHash verification failed";
        case ERR_BOXHASH_KEY_NOT_SET:
            return "BoxHash key not set";
        
        case ERR_BRAID_NOT_INITIALIZED:
            return "Braid not initialized";
        case ERR_BRAID_DISK_OFFLINE:
            return "Braid disk offline";
        case ERR_BRAID_DISK_FULL:
            return "Braid disk full";
        case ERR_BRAID_READ_FAILED:
            return "Braid read failed";
        case ERR_BRAID_WRITE_FAILED:
            return "Braid write failed";
        case ERR_BRAID_CHECKSUM_MISMATCH:
            return "Braid checksum mismatch";
        case ERR_BRAID_HEAL_FAILED:
            return "Braid heal failed";
        case ERR_BRAID_INSUFFICIENT_DISKS:
            return "Braid insufficient disks";
        case ERR_BRAID_MODE_INVALID:
            return "Braid invalid mode";
        case ERR_BRAID_REBUILD_FAILED:
            return "Braid rebuild failed";

        default:
            return "Unknown error code";
    }
}
