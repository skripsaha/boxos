/*
 * vocab_exceptions.cpp — key functions for the Ф8 vocabulary-type
 * exceptions (single vtable/typeinfo emission) plus their __throw_*
 * helpers. Same scheme as <stdexcept>: the headers keep the throw off the
 * hot inline path and let one TU own the exception's polymorphic image.
 *
 *   bad_optional_access      <optional>
 *   bad_variant_access       <variant>
 *   bad_expected_access<void> <expected>   (the templated derived classes
 *                                            inherit this non-template base,
 *                                            so its what() is the key fn)
 */

#include <expected>
#include <memory>
#include <optional>
#include <variant>

namespace std {

bad_optional_access::~bad_optional_access() = default;
const char *bad_optional_access::what() const noexcept
{
    return "bad optional access";
}
void __throw_bad_optional_access() { throw bad_optional_access{}; }

bad_variant_access::~bad_variant_access() = default;
const char *bad_variant_access::what() const noexcept
{
    return "bad variant access";
}
void __throw_bad_variant_access() { throw bad_variant_access{}; }

// bad_expected_access<E> is a template that throws inline (it carries E),
// but its non-template base owns the vtable/typeinfo and what() message.
bad_expected_access<void>::~bad_expected_access() = default;
const char *bad_expected_access<void>::what() const noexcept
{
    return "bad access to std::expected without expected value";
}

bad_weak_ptr::~bad_weak_ptr() = default;
const char *bad_weak_ptr::what() const noexcept { return "bad_weak_ptr"; }
void __throw_bad_weak_ptr() { throw bad_weak_ptr{}; }

} // namespace std
