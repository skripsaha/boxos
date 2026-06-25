/*
 * box_error.cpp — the box:: error model's out-of-line tables.
 *
 *   * box::_detail::error_message / error_category_name — a static,
 *     allocation-free lookup over the box/error.h codes. Both switch tables
 *     expand from the one BOX_ERROR_LIST(X) in box/error.h, so they carry
 *     EVERY code and can never drift from it: a code's message and category
 *     live on its row in that list. Kept out of the header so the string table
 *     is emitted once and box::error stays a trivially-copyable 4-byte value.
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
    case OK: return "ok";
#define BOX_ERROR_MSG(SUFFIX, name, value, msg, cat) case ERR_##SUFFIX: return msg;
    BOX_ERROR_LIST(BOX_ERROR_MSG)
#undef BOX_ERROR_MSG
    default: return "unknown error code";
    }
}

std::string_view error_category_name(::error_t code) noexcept
{
    switch (code) {
    case OK: return "ok";
#define BOX_ERROR_CAT(SUFFIX, name, value, msg, cat) case ERR_##SUFFIX: return cat;
    BOX_ERROR_LIST(BOX_ERROR_CAT)
#undef BOX_ERROR_CAT
    default: return "box";
    }
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
