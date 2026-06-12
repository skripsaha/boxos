/*
 * stdexcept.cpp — key functions for the <stdexcept> hierarchy (single
 * vtable/typeinfo emission) + the boxcxx::Throw* helpers containers use
 * instead of including <stdexcept> (it includes <string>; the include
 * cycle breaks here, same scheme as libstdc++'s __throw_* functions).
 */

#include <stdexcept>

#include "__bits/throw_helpers"

namespace std {

logic_error::~logic_error() = default;
const char *logic_error::what() const noexcept { return __msg.c_str(); }

domain_error::~domain_error()     = default;
invalid_argument::~invalid_argument() = default;
length_error::~length_error()     = default;
out_of_range::~out_of_range()     = default;

runtime_error::~runtime_error() = default;
const char *runtime_error::what() const noexcept { return __msg.c_str(); }

range_error::~range_error()         = default;
overflow_error::~overflow_error()   = default;
underflow_error::~underflow_error() = default;

} // namespace std

namespace boxcxx {

void ThrowInvalidArgument(const char *what)
{
    throw std::invalid_argument(what);
}
void ThrowDomainError(const char *what) { throw std::domain_error(what); }
void ThrowLengthError(const char *what) { throw std::length_error(what); }
void ThrowOutOfRange(const char *what) { throw std::out_of_range(what); }
void ThrowRangeError(const char *what) { throw std::range_error(what); }
void ThrowOverflowError(const char *what)
{
    throw std::overflow_error(what);
}
void ThrowUnderflowError(const char *what)
{
    throw std::underflow_error(what);
}
void ThrowLogicError(const char *what) { throw std::logic_error(what); }
void ThrowRuntimeError(const char *what) { throw std::runtime_error(what); }

} // namespace boxcxx
