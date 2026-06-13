/*
 * system_error.cpp — category singletons + key functions.
 *
 * The categories are constinit objects (constexpr base ctor, vtable
 * filled at static-init by the compiler-emitted descriptor — no dynamic
 * initializer runs), so error_code construction is safe at any point of
 * static initialization.
 */

#include <system_error>

namespace std {

error_category::~error_category() = default;

error_condition
error_category::default_error_condition(int code) const noexcept
{
    return error_condition(code, *this);
}

bool error_category::equivalent(int code,
                                const error_condition &cond) const noexcept
{
    return default_error_condition(code) == cond;
}

bool error_category::equivalent(const error_code &ec, int cond) const noexcept
{
    return *this == ec.category() && ec.value() == cond;
}

system_error::~system_error() = default;

namespace {

const char *GenericText(int code)
{
    switch (static_cast<errc>(code)) {
    case errc::operation_not_permitted:        return "operation not permitted";
    case errc::no_such_file_or_directory:      return "no such file or directory";
    case errc::io_error:                       return "input/output error";
    case errc::resource_unavailable_try_again: return "resource unavailable, try again";
    case errc::not_enough_memory:              return "cannot allocate memory";
    case errc::permission_denied:              return "permission denied";
    case errc::device_or_resource_busy:        return "device or resource busy";
    case errc::file_exists:                    return "file exists";
    case errc::invalid_argument:               return "invalid argument";
    case errc::no_space_on_device:             return "no space left on device";
    case errc::result_out_of_range:            return "numerical result out of range";
    case errc::resource_deadlock_would_occur:  return "resource deadlock would occur";
    case errc::function_not_supported:         return "function not supported";
    case errc::not_supported:                  return "operation not supported";
    case errc::timed_out:                      return "connection timed out";
    case errc::operation_canceled:             return "operation canceled";
    case errc::owner_dead:                     return "owner dead";
    case errc::state_not_recoverable:          return "state not recoverable";
    case errc::address_family_not_supported:   return "address family not supported by protocol";
    case errc::address_in_use:                 return "address already in use";
    case errc::address_not_available:          return "cannot assign requested address";
    case errc::already_connected:              return "transport endpoint is already connected";
    case errc::argument_list_too_long:         return "argument list too long";
    case errc::argument_out_of_domain:         return "numerical argument out of domain";
    case errc::bad_address:                    return "bad address";
    case errc::bad_file_descriptor:            return "bad file descriptor";
    case errc::bad_message:                    return "bad message";
    case errc::broken_pipe:                    return "broken pipe";
    case errc::connection_aborted:             return "software caused connection abort";
    case errc::connection_already_in_progress: return "operation already in progress";
    case errc::connection_refused:             return "connection refused";
    case errc::connection_reset:               return "connection reset by peer";
    case errc::cross_device_link:              return "invalid cross-device link";
    case errc::destination_address_required:   return "destination address required";
    case errc::directory_not_empty:            return "directory not empty";
    case errc::executable_format_error:        return "exec format error";
    case errc::file_too_large:                 return "file too large";
    case errc::filename_too_long:              return "file name too long";
    case errc::host_unreachable:               return "no route to host";
    case errc::identifier_removed:             return "identifier removed";
    case errc::illegal_byte_sequence:          return "invalid or incomplete multibyte or wide character";
    case errc::inappropriate_io_control_operation: return "inappropriate ioctl for device";
    case errc::interrupted:                    return "interrupted system call";
    case errc::invalid_seek:                   return "illegal seek";
    case errc::is_a_directory:                 return "is a directory";
    case errc::message_size:                   return "message too long";
    case errc::network_down:                   return "network is down";
    case errc::network_reset:                  return "network dropped connection on reset";
    case errc::network_unreachable:            return "network is unreachable";
    case errc::no_buffer_space:                return "no buffer space available";
    case errc::no_child_process:               return "no child processes";
    case errc::no_link:                        return "link has been severed";
    case errc::no_lock_available:              return "no locks available";
    case errc::no_message:                     return "no message of desired type";
    case errc::no_message_available:           return "no message available on the stream head read queue";
    case errc::no_protocol_option:             return "protocol not available";
    case errc::no_stream_resources:            return "out of streams resources";
    case errc::no_such_device:                 return "no such device";
    case errc::no_such_device_or_address:      return "no such device or address";
    case errc::no_such_process:                return "no such process";
    case errc::not_a_directory:                return "not a directory";
    case errc::not_a_socket:                   return "socket operation on non-socket";
    case errc::not_a_stream:                   return "device not a stream";
    case errc::not_connected:                  return "transport endpoint is not connected";
    case errc::operation_in_progress:          return "operation now in progress";
    case errc::protocol_error:                 return "protocol error";
    case errc::protocol_not_supported:         return "protocol not supported";
    case errc::read_only_file_system:          return "read-only file system";
    case errc::stream_timeout:                 return "timer expired";
    case errc::text_file_busy:                 return "text file busy";
    case errc::too_many_files_open:            return "too many open files";
    case errc::too_many_files_open_in_system:  return "too many open files in system";
    case errc::too_many_links:                 return "too many links";
    case errc::too_many_symbolic_link_levels:  return "too many levels of symbolic links";
    case errc::value_too_large:                return "value too large for defined data type";
    case errc::wrong_protocol_type:            return "protocol wrong type for socket";
    default:                                   return nullptr;
    }
}

class GenericCategory final : public error_category {
public:
    constexpr GenericCategory() noexcept = default;
    const char *name() const noexcept override { return "generic"; }
    string message(int code) const override
    {
        if (const char *text = GenericText(code)) return string(text);
        return string("generic error ") + to_string(code);
    }
};

class SystemCategory final : public error_category {
public:
    constexpr SystemCategory() noexcept = default;
    const char *name() const noexcept override { return "system"; }
    string message(int code) const override
    {
        if (const char *text = GenericText(code)) return string(text);
        return string("system error ") + to_string(code);
    }
    error_condition default_error_condition(int code) const noexcept override
    {
        // BoxOS "system" errors share the errno numbering, so they map
        // straight onto the generic conditions.
        return error_condition(code, generic_category());
    }
};

constinit GenericCategory g_generic_category;
constinit SystemCategory g_system_category;

} // namespace

const error_category &generic_category() noexcept
{
    return g_generic_category;
}

const error_category &system_category() noexcept
{
    return g_system_category;
}

} // namespace std
