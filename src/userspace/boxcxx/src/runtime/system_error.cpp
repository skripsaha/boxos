
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
    default:                                   return nullptr;
    }
}

}

extern "C" const char *__boxcxx_generic_text(int code) noexcept
{
    return GenericText(code);
}

namespace {

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
        return error_condition(code, system_category());
    }
};

constinit GenericCategory g_generic_category;
constinit SystemCategory g_system_category;

}

const error_category &generic_category() noexcept
{
    return g_generic_category;
}

const error_category &system_category() noexcept
{
    return g_system_category;
}

}