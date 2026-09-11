
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

}

namespace {

class BoxCategory final : public std::error_category {
public:
    constexpr BoxCategory() noexcept = default;
    const char *name() const noexcept override { return "box"; }
    std::string message(int code) const override
    {
        return std::string(_detail::error_message(static_cast<::error_t>(code)));
    }
    std::error_condition default_error_condition(int code) const noexcept override
    {
        return std::error_condition(code, *this);
    }
};

constinit BoxCategory g_box_category;

}

const std::error_category &error_category() noexcept
{
    return g_box_category;
}

}