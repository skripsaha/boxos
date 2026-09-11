
#include <any>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <variant>

namespace std {

bad_any_cast::~bad_any_cast() = default;
const char *bad_any_cast::what() const noexcept { return "bad any cast"; }
void __throw_bad_any_cast() { throw bad_any_cast{}; }

bad_optional_access::~bad_optional_access() = default;
const char *bad_optional_access::what() const noexcept
{
    return "bad optional access";
}
void __throw_bad_optional_access() { throw bad_optional_access{}; }

bad_function_call::~bad_function_call() = default;
const char *bad_function_call::what() const noexcept
{
    return "bad function call";
}
void __throw_bad_function_call() { throw bad_function_call{}; }

bad_variant_access::~bad_variant_access() = default;
const char *bad_variant_access::what() const noexcept
{
    return "bad variant access";
}
void __throw_bad_variant_access() { throw bad_variant_access{}; }

bad_expected_access<void>::~bad_expected_access() = default;
const char *bad_expected_access<void>::what() const noexcept
{
    return "bad access to std::expected without expected value";
}

bad_weak_ptr::~bad_weak_ptr() = default;
const char *bad_weak_ptr::what() const noexcept { return "bad_weak_ptr"; }
void __throw_bad_weak_ptr() { throw bad_weak_ptr{}; }

}