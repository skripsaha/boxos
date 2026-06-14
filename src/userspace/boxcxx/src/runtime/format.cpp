// boxcxx — <format> runtime anchor.
//
// The formatting engine is header-resident (it is templated on the argument
// pack and the output sink). The only out-of-line definition is the
// format_error key function, which pins the vtable and typeinfo to a single
// translation unit so catch-by-base works across the program — the same
// key-function scheme used by the other boxcxx exception types.
#include <format>

namespace std {

format_error::~format_error() = default;

} // namespace std
