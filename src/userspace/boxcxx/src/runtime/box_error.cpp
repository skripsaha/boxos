/*
 * box_error.cpp — the box:: error model's out-of-line tables.
 *
 *   * box::_detail::error_message / error_category_name — a static,
 *     allocation-free lookup over the box/error.h codes. Kept out of the
 *     header so the string table is emitted once and box::error stays a
 *     trivially-copyable 4-byte value.
 *   * box::error_category() — a std::error_category whose numbering and
 *     messages are BoxOS's own. Like the std categories it is a constinit
 *     object (constexpr base ctor, vtable filled by the compiler-emitted
 *     descriptor — no dynamic initializer), so make_error_code is safe at
 *     any point of static initialization. default_error_condition is the
 *     IDENTITY: a BoxOS code maps only to itself, never onto the Linux
 *     generic_category, because the two numberings share no meaning.
 */

#include <box/cxx/error.h>

#include <string>

namespace box {
namespace _detail {

std::string_view error_message(::error_t code) noexcept
{
    switch (code) {
    case OK:                          return "ok";

    case ERR_UNKNOWN:                 return "unknown error";
    case ERR_NOT_IMPLEMENTED:         return "not implemented";
    case ERR_INVALID_ARGUMENT:        return "invalid argument";
    case ERR_NULL_POINTER:            return "null pointer";
    case ERR_OUT_OF_RANGE:            return "out of range";
    case ERR_BUFFER_TOO_SMALL:        return "buffer too small";
    case ERR_TIMEOUT:                 return "timed out";
    case ERR_BUSY:                    return "busy";
    case ERR_WOULD_BLOCK:             return "operation would block";
    case ERR_ALIGNMENT:              return "bad alignment";
    case ERR_CORRUPTED:               return "corrupted";
    case ERR_INTERNAL:                return "internal error";

    case ERR_NO_MEMORY:               return "out of memory";
    case ERR_INVALID_ADDRESS:         return "invalid address";
    case ERR_PAGE_FAULT:              return "page fault";
    case ERR_ALREADY_MAPPED:          return "already mapped";
    case ERR_NOT_MAPPED:              return "not mapped";
    case ERR_PERMISSION_DENIED_MEM:   return "memory permission denied";
    case ERR_HEAP_EXHAUSTED:          return "heap exhausted";
    case ERR_STACK_OVERFLOW:          return "stack overflow";
    case ERR_BUFFER_OVERFLOW:         return "buffer overflow";
    case ERR_INVALID_BUFFER_ID:       return "invalid buffer id";
    case ERR_BUFFER_IN_USE:           return "buffer in use";
    case ERR_BUFFER_LIMIT_EXCEEDED:   return "buffer limit exceeded";

    case ERR_IO:                      return "i/o error";
    case ERR_READ_FAILED:             return "read failed";
    case ERR_WRITE_FAILED:            return "write failed";
    case ERR_DEVICE_NOT_READY:        return "device not ready";
    case ERR_DEVICE_ERROR:            return "device error";
    case ERR_STREAM_CLOSED:           return "stream closed";
    case ERR_DISK_FULL:               return "disk full";
    case ERR_BAD_SECTOR:              return "bad sector";

    case ERR_FILE_NOT_FOUND:          return "file not found";
    case ERR_OBJECT_NOT_FOUND:        return "object not found";
    case ERR_TAG_NOT_FOUND:           return "tag not found";
    case ERR_ALREADY_EXISTS:          return "already exists";
    case ERR_INVALID_TAG:             return "invalid tag";
    case ERR_TAG_LIMIT_EXCEEDED:      return "tag limit exceeded";
    case ERR_OBJECT_CORRUPTED:        return "object corrupted";
    case ERR_JOURNAL_FULL:            return "journal full";
    case ERR_JOURNAL_CORRUPTED:       return "journal corrupted";
    case ERR_METADATA_CORRUPTED:      return "metadata corrupted";

    case ERR_PROCESS_NOT_FOUND:       return "process not found";
    case ERR_INVALID_PID:             return "invalid pid";
    case ERR_PROCESS_LIMIT_EXCEEDED:  return "process limit exceeded";
    case ERR_PROCESS_TERMINATED:      return "process terminated";
    case ERR_PROCESS_BLOCKED:         return "process blocked";
    case ERR_INVALID_ELF:             return "invalid elf";
    case ERR_BINARY_TOO_LARGE:        return "binary too large";
    case ERR_SPAWN_FAILED:            return "spawn failed";

    case ERR_ACCESS_DENIED:           return "access denied";
    case ERR_PERMISSION_DENIED:       return "permission denied";
    case ERR_SECURITY_VIOLATION:      return "security violation";
    case ERR_TAG_MISMATCH:            return "tag mismatch";
    case ERR_INVALID_OPERATION:       return "invalid operation";

    case ERR_HARDWARE:                return "hardware error";
    case ERR_INVALID_DEVICE:          return "invalid device";
    case ERR_DEVICE_BUSY:             return "device busy";
    case ERR_KEYBOARD_BUFFER_FULL:    return "keyboard buffer full";
    case ERR_VGA_ERROR:               return "vga error";
    case ERR_ATA_ERROR:               return "ata error";
    case ERR_PCI_ERROR:               return "pci error";

    case ERR_POCKET_RING_FULL:        return "pocket ring full";
    case ERR_RESULT_RING_FULL:        return "result ring full";
    case ERR_INVALID_POCKET:          return "invalid pocket";
    case ERR_INVALID_DECK_ID:         return "invalid deck id";
    case ERR_INVALID_OPCODE:          return "invalid opcode";
    case ERR_PREFIX_CHAIN_TOO_LONG:   return "prefix chain too long";
    case ERR_POCKET_PROCESSING_FAILED:return "pocket processing failed";
    case ERR_PENDING_QUEUE_FULL:      return "pending queue full";

    case ERR_ADDR_VALUE_MISMATCH:     return "park value mismatch";

    default:                          return "error";
    }
}

std::string_view error_category_name(::error_t code) noexcept
{
    if (code == OK)                       return "ok";
    if (code <= ERR_INTERNAL)             return "core";       // 1..12
    if (code >= 100 && code <= 111)       return "memory";
    if (code >= 200 && code <= 207)       return "io";
    if (code >= 300 && code <= 309)       return "storage";
    if (code >= 400 && code <= 407)       return "process";
    if (code >= 500 && code <= 504)       return "security";
    if (code >= 600 && code <= 606)       return "hardware";
    if (code >= 900 && code <= 907)       return "ipc";
    if (code == ERR_ADDR_VALUE_MISMATCH)  return "strand";
    return "box";
}

}  // namespace _detail

namespace {

class BoxCategory final : public std::error_category {
public:
    constexpr BoxCategory() noexcept = default;
    const char *name() const noexcept override { return "box"; }
    std::string message(int code) const override
    {
        return std::string(_detail::error_message(static_cast<::error_t>(code)));
    }
    // Identity, never generic: a BoxOS code has no faithful Linux-errno
    // equivalent, so it is its own condition. This is the deliberate opposite
    // of std::system_category's old (incorrect) generic remap.
    std::error_condition default_error_condition(int code) const noexcept override
    {
        return std::error_condition(code, *this);
    }
};

constinit BoxCategory g_box_category;

}  // namespace

const std::error_category &error_category() noexcept
{
    return g_box_category;
}

}  // namespace box
