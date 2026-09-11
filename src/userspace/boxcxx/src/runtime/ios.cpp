
#include <ios>
#include <istream>

namespace std {

ios_base::~ios_base()
{
    FireEvent(erase_event);
}

ios_base::failure::failure(const string &msg, const error_code &ec)
    : system_error(ec, msg)
{
}

ios_base::failure::failure(const char *msg, const error_code &ec)
    : system_error(ec, msg)
{
}

ios_base::failure::~failure() = default;

namespace {

class IostreamCategory final : public error_category {
public:
    constexpr IostreamCategory() noexcept = default;
    const char *name() const noexcept override { return "iostream"; }
    string message(int code) const override
    {
        if (code == static_cast<int>(io_errc::stream)) return string("iostream error");
        return string("iostream error ") + to_string(code);
    }
};

constinit IostreamCategory g_iostream_category;

atomic<int> g_ios_xalloc_counter{0};

}

const error_category &iostream_category() noexcept
{
    return g_iostream_category;
}

int ios_base::xalloc() noexcept
{
    return g_ios_xalloc_counter.fetch_add(1, memory_order_relaxed);
}

}