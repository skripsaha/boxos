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
        case ERR_UNSUPPORTED:
            return "Unsupported operation";
        case ERR_CHECKSUM:
            return "Checksum failed";

        case ERR_NO_MEMORY:
            return "Out of memory";
        case ERR_INVALID_ADDRESS:
            return "Invalid memory address";
        case ERR_ALREADY_MAPPED:
            return "Memory already mapped";
        case ERR_NOT_MAPPED:
            return "Memory not mapped";
        case ERR_PERMISSION_DENIED_MEM:
            return "Memory access permission denied";
        case ERR_HEAP_EXHAUSTED:
            return "Heap exhausted";
        case ERR_INVALID_BUFFER_ID:
            return "Invalid buffer ID";
        case ERR_MAPPING_FAILED:
            return "Memory mapping failed";
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
        case ERR_STREAM_CLOSED:
            return "Stream closed";
        case ERR_BAD_SECTOR:
            return "Bad disk sector";
        case ERR_IO_CANCELLED:
            return "I/O request cancelled";
        case ERR_UNALIGNED:
            return "Not whole blocks of the medium";

        case ERR_FILE_NOT_FOUND:
            return "File not found";
        case ERR_OBJECT_NOT_FOUND:
            return "Object not found";
        case ERR_TAG_NOT_FOUND:
            return "Tag not found";
        case ERR_ALREADY_EXISTS:
            return "Object already exists";
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
        case ERR_PROCESS_DESTROYING:
            return "Process is being destroyed";
        case ERR_TAG_OVERFLOW_FAILED:
            return "Tag overflow allocation failed";

        case ERR_ACCESS_DENIED:
            return "Access denied";
        case ERR_PERMISSION_DENIED:
            return "Permission denied";
        case ERR_INVALID_OPERATION:
            return "Invalid operation";

        case ERR_CPU_ERROR:
            return "CPU error";

        case ERR_POCKET_RING_FULL:
            return "Pocket ring buffer full";
        case ERR_INVALID_POCKET:
            return "Invalid pocket";
        case ERR_INVALID_OPCODE:
            return "Invalid opcode";
        case ERR_KCORE_QUEUE_FULL:
            return "K-Core queue full";
        case ERR_KCORE_SUBMIT_FAILED:
            return "K-Core submit failed";

        case ERR_ROUTE_NO_SUBSCRIBERS:
            return "No subscribers for route";
        case ERR_ROUTE_SELF:
            return "Cannot route to self";
        case ERR_ROUTE_AMBIGUOUS:
            return "More than one carries the route tag";

        case ERR_RUNQUEUE_FULL:
            return "RunQueue full";
        case ERR_HOME_CORE_INVALID:
            return "Invalid home core";

        case ERR_E820_FAILED:
            return "E820 memory detection failed";

        // TagFS Core Errors
        case ERR_TAGFS_NOT_INITIALIZED:
            return "TagFS not initialized";
        case ERR_TAGFS_CORRUPTED:
            return "TagFS corrupted";
        case ERR_TAGFS_NO_SPACE:
            return "TagFS no space";
        case ERR_TAGFS_INVALID_HANDLE:
            return "TagFS invalid handle";
        case ERR_TAGFS_READ_ONLY:
            return "TagFS read only";
        case ERR_TAGFS_METADATA_ERROR:
            return "TagFS metadata error";
        case ERR_TAGFS_REGISTRY_FULL:
            return "TagFS registry full";
        case ERR_TAGFS_TAG_NOT_FOUND:
            return "TagFS tag not found";
        case ERR_TAGFS_INVALID_TAG:
            return "TagFS invalid tag";
        case ERR_TAGFS_EXTENT_ERROR:
            return "TagFS extent error";
        case ERR_TAGFS_TAG_TOO_LONG:
            return "TagFS tag too long";
        case ERR_TAGFS_BEYOND_SIZE:
            return "TagFS write begins past the file's size";
        case ERR_TAGFS_RECORD_FULL:
            return "TagFS file record full";
        case ERR_TAGFS_FID_EXHAUSTED:
            return "TagFS file ids exhausted";
        case ERR_TAGFS_NO_OWN_TAGS:
            return "TagFS: this program keeps no tags of its own";

        // TagFS Module Errors
        case ERR_TAGFS_NOT_LANDED:
            return "TagFS write did not land";
        
        case ERR_BRAID_DISK_OFFLINE:
            return "Braid disk offline";
        case ERR_BRAID_HEAL_FAILED:
            return "Braid heal failed";
        case ERR_BRAID_INSUFFICIENT_DISKS:
            return "Braid insufficient disks";
        case ERR_BRAID_MODE_INVALID:
            return "Braid invalid mode";

        case ERR_ADDR_VALUE_MISMATCH:
            return "Address value mismatch (park skipped)";

        default:
            return "Unknown error code";
    }
}
