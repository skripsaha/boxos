// boxcxx — <cwchar> runtime, numeric conversion
//
// The seven wcsto* functions. None of them parses a number: every character a
// numeric literal can contain is ASCII, so the candidate run is narrowed one
// character to one character and handed to the strto* that <cstdlib> already
// has. That keeps exactly one implementation of the grammar, one of the
// correctly-rounded decimal-to-binary path, and one set of overflow rules —
// the same reasoning that gave the system exactly one printf in Ф41.
//
// Two things are genuinely different in the wide form, and both come from C
// rather than from us:
//
//   * The leading white space is skipped by `iswspace`, which has been
//     Unicode-wide since Ф42-a. `wcstod(L" " L"42", ...)` therefore
//     consumes the EM SPACE and reads 42, where the narrow `strtod` would stop
//     at the first byte of it. This is not an oversight in either direction:
//     C defines the skip in terms of the classification of the character type
//     it was given.
//   * `endptr` must point into the CALLER's wide string. Because the narrowing
//     is one character to one character, the count strto* consumed maps back by
//     addition, with no second scan and nothing to keep in step.

#include <cstddef>
#include <cstdlib>
#include <cwchar>
#include <cwctype>
#include <new>

namespace {

// A superset of every character any numeric literal can hold: digits and
// letters (hex digits, the x/e/p markers, and the letters of `inf`, `infinity`
// and `nan`), the sign, the radix point, and the parentheses and underscores
// of nan's n-char-sequence. Bounding the copy by this rather than by "ASCII"
// keeps a megabyte of following prose out of the buffer.
bool NumericByte(::std::wint_t c) noexcept
{
    return (c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'z') ||
           (c >= L'A' && c <= L'Z') || c == L'+' || c == L'-' || c == L'.' ||
           c == L'(' || c == L')' || c == L'_';
}

// Long enough that no literal anyone writes reaches the heap: 1074 digits is
// the most that can affect a double, and a hex float needs far fewer.
constexpr ::std::size_t kInline = 512;

// The narrowed candidate, plus where it started in the wide string.
struct Candidate {
    char             *text;      // NUL-terminated, ASCII
    ::std::size_t     len;
    const wchar_t    *begin;     // first character after the white space
    char              inln[kInline];
    bool              heap;

    ~Candidate() { if (heap) ::operator delete[](text); }
};

// Narrows the numeric candidate at `s`. Returns false only when the run is too
// long for the inline buffer AND the heap refuses it, which is a genuine
// out-of-memory rather than a parse outcome; the caller then reports "no
// conversion", which is the one answer C gives that cannot be wrong about the
// value.
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

// Maps the narrow end pointer back into the caller's wide string. strto* leaves
// `nend` at the narrowed text when it converted nothing, and C then requires
// endptr to be the ORIGINAL argument — before the white space, not after it.
void SetEnd(wchar_t **endptr, const Candidate &c, const char *nend,
            const wchar_t *original) noexcept
{
    if (!endptr) return;
    const ::std::size_t used = static_cast<::std::size_t>(nend - c.text);
    *endptr = used == 0 ? const_cast<wchar_t *>(original)
                        : const_cast<wchar_t *>(c.begin) + used;
}

} // namespace

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

} // namespace std
