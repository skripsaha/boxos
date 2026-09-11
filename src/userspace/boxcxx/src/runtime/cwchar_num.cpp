
#include <cstddef>
#include <cstdlib>
#include <cwchar>
#include <cwctype>
#include <new>

namespace {

bool NumericByte(::std::wint_t c) noexcept
{
    return (c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'z') ||
           (c >= L'A' && c <= L'Z') || c == L'+' || c == L'-' || c == L'.' ||
           c == L'(' || c == L')' || c == L'_';
}

constexpr ::std::size_t kInline = 512;

struct Candidate {
    char             *text;
    ::std::size_t     len;
    const wchar_t    *begin;
    char              inln[kInline];
    bool              heap;

    ~Candidate() { if (heap) ::operator delete[](text); }
};

bool Narrow(const wchar_t *s, Candidate &c) noexcept
{
    while (::std::iswspace(static_cast<::std::wint_t>(*s))) ++s;
    c.begin = s;

    ::std::size_t n = 0;
    while (NumericByte(static_cast<::std::wint_t>(s[n]))) ++n;

    c.heap = n + 1 > kInline;
    if (c.heap) {
        c.text = static_cast<char *>(::operator new[](n + 1, ::std::nothrow));
        if (!c.text) { c.heap = false; return false; }
    } else {
        c.text = c.inln;
    }
    for (::std::size_t i = 0; i < n; ++i) c.text[i] = static_cast<char>(s[i]);
    c.text[n] = '\0';
    c.len = n;
    return true;
}

void SetEnd(wchar_t **endptr, const Candidate &c, const char *nend,
            const wchar_t *original) noexcept
{
    if (!endptr) return;
    const ::std::size_t used = static_cast<::std::size_t>(nend - c.text);
    *endptr = used == 0 ? const_cast<wchar_t *>(original)
                        : const_cast<wchar_t *>(c.begin) + used;
}

}

namespace std {

#define BOXCXX_WCSTO_FLOAT(NAME, NARROW, TYPE)                                 \
    TYPE NAME(const wchar_t *s, wchar_t **end) noexcept                        \
    {                                                                          \
        Candidate c;                                                           \
        if (!Narrow(s, c)) { if (end) *end = const_cast<wchar_t *>(s); return 0; } \
        char      *nend = nullptr;                                             \
        const TYPE v    = NARROW(c.text, &nend);                               \
        SetEnd(end, c, nend, s);                                               \
        return v;                                                              \
    }

BOXCXX_WCSTO_FLOAT(wcstod, strtod, double)
BOXCXX_WCSTO_FLOAT(wcstof, strtof, float)
BOXCXX_WCSTO_FLOAT(wcstold, strtold, long double)

#undef BOXCXX_WCSTO_FLOAT

#define BOXCXX_WCSTO_INT(NAME, NARROW, TYPE)                                   \
    TYPE NAME(const wchar_t *s, wchar_t **end, int base) noexcept              \
    {                                                                          \
        Candidate c;                                                           \
        if (!Narrow(s, c)) { if (end) *end = const_cast<wchar_t *>(s); return 0; } \
        char      *nend = nullptr;                                             \
        const TYPE v    = NARROW(c.text, &nend, base);                         \
        SetEnd(end, c, nend, s);                                               \
        return v;                                                              \
    }

BOXCXX_WCSTO_INT(wcstol, strtol, long)
BOXCXX_WCSTO_INT(wcstoll, strtoll, long long)
BOXCXX_WCSTO_INT(wcstoul, strtoul, unsigned long)
BOXCXX_WCSTO_INT(wcstoull, strtoull, unsigned long long)

#undef BOXCXX_WCSTO_INT

}