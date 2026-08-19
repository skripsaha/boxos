/*
 * ios.cpp — key functions and out-of-line members for <ios>.
 *
 * Same scheme as vocab_exceptions.cpp / system_error.cpp: heavy or
 * genuinely-non-template pieces of ios_base live in exactly one
 * translation unit instead of every includer's vague-linkage copy.
 *
 *   ios_base::~ios_base()      key function (anchors vtable/typeinfo),
 *                               also the erase_event firing point
 *   ios_base::failure          ctors + dtor (system_error-derived)
 *   iostream_category()        error_category singleton
 *   ios_base::xalloc()         backed by a real atomic<int> global
 *
 *   __ios::IsSpace(wchar_t)    the wide whitespace test, out of line so that
 *                               <istream> need not carry Unicode tables
 *
 * ios_base::Init is NOT here. Since Ф36 it sequences something real — the
 * four stream objects — so it lives with them, in iostream.cpp. Putting it
 * here would make every user of <ios> drag the console streams in.
 */

#include <cwctype>
#include <ios>
#include <istream>

namespace std {

namespace __ios {

// Declared in <istream>. A wide stream's characters are code points, so its
// whitespace is Unicode White_Space and not the six of the "C" locale — the
// same decision Ф42-a made for <cwctype> and Ф42-b for wcstod, which has to
// skip an EM SPACE before a number. Defined here rather than in the header
// because iswspace stands on 1 312 lines of generated tables, and a header
// that every formatted insertion includes should not carry them.
bool IsSpace(wchar_t c)
{
    return iswspace(static_cast<wint_t>(c)) != 0;
}

} // namespace __ios

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

} // namespace

const error_category &iostream_category() noexcept
{
    return g_iostream_category;
}

int ios_base::xalloc() noexcept
{
    return g_ios_xalloc_counter.fetch_add(1, memory_order_relaxed);
}

} // namespace std
