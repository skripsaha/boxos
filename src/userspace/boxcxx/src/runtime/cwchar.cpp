
#include <cerrno>
#include <cstddef>
#include <cuchar>
#include <cwchar>

namespace {

thread_local ::std::mbstate_t g_mbrlen;
thread_local ::std::mbstate_t g_mbrtowc;
thread_local ::std::mbstate_t g_wcrtomb;
thread_local ::std::mbstate_t g_mbsrtowcs;
thread_local ::std::mbstate_t g_wcsrtombs;

constexpr ::std::size_t kIncomplete = static_cast<::std::size_t>(-2);
constexpr ::std::size_t kInvalid    = static_cast<::std::size_t>(-1);

constexpr bool SingleByte(unsigned v) noexcept { return v <= 0x7Fu; }

constexpr int kEof = -1;

}

namespace std {


wchar_t *wcscpy(wchar_t *dst, const wchar_t *src) noexcept
{
    wchar_t *out = dst;
    while ((*out++ = *src++) != L'\0') {}
    return dst;
}

wchar_t *wcsncpy(wchar_t *dst, const wchar_t *src, size_t n) noexcept
{
    size_t i = 0;
    for (; i < n && src[i] != L'\0'; ++i) dst[i] = src[i];
    for (; i < n; ++i) dst[i] = L'\0';
    return dst;
}

wchar_t *wmemcpy(wchar_t *dst, const wchar_t *src, size_t n) noexcept
{
    for (size_t i = 0; i < n; ++i) dst[i] = src[i];
    return dst;
}

wchar_t *wmemmove(wchar_t *dst, const wchar_t *src, size_t n) noexcept
{
    if (dst < src) {
        for (size_t i = 0; i < n; ++i) dst[i] = src[i];
    } else if (dst > src) {
        for (size_t i = n; i-- > 0;) dst[i] = src[i];
    }
    return dst;
}


wchar_t *wcscat(wchar_t *dst, const wchar_t *src) noexcept
{
    wchar_t *out = dst;
    while (*out != L'\0') ++out;
    while ((*out++ = *src++) != L'\0') {}
    return dst;
}

wchar_t *wcsncat(wchar_t *dst, const wchar_t *src, size_t n) noexcept
{
    wchar_t *out = dst;
    while (*out != L'\0') ++out;
    size_t i = 0;
    for (; i < n && src[i] != L'\0'; ++i) out[i] = src[i];
    out[i] = L'\0';
    return dst;
}


int wcscmp(const wchar_t *a, const wchar_t *b) noexcept
{
    while (*a != L'\0' && *a == *b) { ++a; ++b; }
    return (*a < *b) ? -1 : (*a > *b) ? 1 : 0;
}

int wcsncmp(const wchar_t *a, const wchar_t *b, size_t n) noexcept
{
    for (size_t i = 0; i < n; ++i) {
        if (a[i] != b[i]) return (a[i] < b[i]) ? -1 : 1;
        if (a[i] == L'\0') break;
    }
    return 0;
}

int wmemcmp(const wchar_t *a, const wchar_t *b, size_t n) noexcept
{
    for (size_t i = 0; i < n; ++i)
        if (a[i] != b[i]) return (a[i] < b[i]) ? -1 : 1;
    return 0;
}

int wcscoll(const wchar_t *a, const wchar_t *b) noexcept { return wcscmp(a, b); }

size_t wcsxfrm(wchar_t *dst, const wchar_t *src, size_t n) noexcept
{
    const size_t len = wcslen(src);
    if (n != 0) {
        size_t i = 0;
        for (; i + 1 < n && src[i] != L'\0'; ++i) dst[i] = src[i];
        dst[i] = L'\0';
    }
    return len;
}


const wchar_t *wcschr(const wchar_t *s, wchar_t c) noexcept
{
    for (;; ++s) {
        if (*s == c) return s;
        if (*s == L'\0') return nullptr;
    }
}

wchar_t *wcschr(wchar_t *s, wchar_t c) noexcept
{
    return const_cast<wchar_t *>(wcschr(static_cast<const wchar_t *>(s), c));
}

const wchar_t *wcsrchr(const wchar_t *s, wchar_t c) noexcept
{
    const wchar_t *found = nullptr;
    for (;; ++s) {
        if (*s == c) found = s;
        if (*s == L'\0') return found;
    }
}

wchar_t *wcsrchr(wchar_t *s, wchar_t c) noexcept
{
    return const_cast<wchar_t *>(wcsrchr(static_cast<const wchar_t *>(s), c));
}

const wchar_t *wcspbrk(const wchar_t *s, const wchar_t *set) noexcept
{
    for (; *s != L'\0'; ++s)
        for (const wchar_t *p = set; *p != L'\0'; ++p)
            if (*s == *p) return s;
    return nullptr;
}

wchar_t *wcspbrk(wchar_t *s, const wchar_t *set) noexcept
{
    return const_cast<wchar_t *>(wcspbrk(static_cast<const wchar_t *>(s), set));
}

const wchar_t *wcsstr(const wchar_t *hay, const wchar_t *needle) noexcept
{
    if (*needle == L'\0') return hay;
    for (; *hay != L'\0'; ++hay) {
        size_t i = 0;
        while (needle[i] != L'\0' && hay[i] == needle[i]) ++i;
        if (needle[i] == L'\0') return hay;
    }
    return nullptr;
}

wchar_t *wcsstr(wchar_t *hay, const wchar_t *needle) noexcept
{
    return const_cast<wchar_t *>(wcsstr(static_cast<const wchar_t *>(hay), needle));
}

const wchar_t *wmemchr(const wchar_t *s, wchar_t c, size_t n) noexcept
{
    for (size_t i = 0; i < n; ++i)
        if (s[i] == c) return s + i;
    return nullptr;
}

wchar_t *wmemchr(wchar_t *s, wchar_t c, size_t n) noexcept
{
    return const_cast<wchar_t *>(wmemchr(static_cast<const wchar_t *>(s), c, n));
}

size_t wcscspn(const wchar_t *s, const wchar_t *set) noexcept
{
    size_t i = 0;
    for (; s[i] != L'\0'; ++i)
        for (const wchar_t *p = set; *p != L'\0'; ++p)
            if (s[i] == *p) return i;
    return i;
}

size_t wcsspn(const wchar_t *s, const wchar_t *set) noexcept
{
    size_t i = 0;
    for (; s[i] != L'\0'; ++i) {
        bool in = false;
        for (const wchar_t *p = set; *p != L'\0' && !in; ++p)
            if (s[i] == *p) in = true;
        if (!in) return i;
    }
    return i;
}

wchar_t *wcstok(wchar_t *s, const wchar_t *delim, wchar_t **ptr) noexcept
{
    if (s == nullptr) s = *ptr;
    if (s == nullptr) return nullptr;

    s += wcsspn(s, delim);
    if (*s == L'\0') { *ptr = nullptr; return nullptr; }

    wchar_t *end = s + wcscspn(s, delim);
    if (*end == L'\0') {
        *ptr = nullptr;
    } else {
        *end = L'\0';
        *ptr = end + 1;
    }
    return s;
}


size_t wcslen(const wchar_t *s) noexcept
{
    size_t n = 0;
    while (s[n] != L'\0') ++n;
    return n;
}

wchar_t *wmemset(wchar_t *s, wchar_t c, size_t n) noexcept
{
    for (size_t i = 0; i < n; ++i) s[i] = c;
    return s;
}


wint_t btowc(int c) noexcept
{
    if (c == kEof || !SingleByte(static_cast<unsigned char>(c)))
        return WEOF;
    return static_cast<wint_t>(static_cast<unsigned char>(c));
}

int wctob(wint_t c) noexcept
{
    if (c == WEOF || !SingleByte(c)) return kEof;
    return static_cast<int>(c);
}

int mbsinit(const mbstate_t *ps) noexcept
{
    return (ps == nullptr) || (ps->nin == 0 && ps->npend == 0);
}

size_t mbrtowc(wchar_t *pwc, const char *s, size_t n, mbstate_t *ps) noexcept
{
    mbstate_t &st = ps ? *ps : g_mbrtowc;

    if (s == nullptr) { st = mbstate_t{}; return 0; }

    char32_t  c32 = 0;
    const size_t r = mbrtoc32(&c32, s, n, &st);
    if (r != kInvalid && r != kIncomplete && pwc != nullptr)
        *pwc = static_cast<wchar_t>(c32);
    return r;
}

size_t mbrlen(const char *s, size_t n, mbstate_t *ps) noexcept
{
    return mbrtowc(nullptr, s, n, ps ? ps : &g_mbrlen);
}

size_t wcrtomb(char *s, wchar_t wc, mbstate_t *ps) noexcept
{
    mbstate_t &st = ps ? *ps : g_wcrtomb;

    char scratch[8];
    if (s == nullptr) { s = scratch; wc = L'\0'; }

    return c32rtomb(s, static_cast<char32_t>(wc), &st);
}

size_t mbsrtowcs(wchar_t *dst, const char **src, size_t len, mbstate_t *ps) noexcept
{
    mbstate_t &st = ps ? *ps : g_mbsrtowcs;
    const char *p = *src;
    size_t written = 0;

    for (;;) {
        if (dst != nullptr && written == len) break;

        wchar_t wc = 0;
        const size_t r = mbrtowc(&wc, p, 4, &st);
        if (r == kInvalid || r == kIncomplete) { errno = EILSEQ; return kInvalid; }

        if (dst != nullptr) dst[written] = wc;
        ++written;

        if (r == 0) {
            if (dst != nullptr) *src = nullptr;
            return written - 1;
        }
        p += r;
    }

    *src = p;
    return written;
}

size_t wcsrtombs(char *dst, const wchar_t **src, size_t len, mbstate_t *ps) noexcept
{
    mbstate_t &st = ps ? *ps : g_wcsrtombs;
    const wchar_t *p = *src;
    size_t written = 0;

    for (;;) {
        char         buf[8];
        const size_t r = wcrtomb(buf, *p, &st);
        if (r == kInvalid) { errno = EILSEQ; return kInvalid; }

        if (dst != nullptr) {
            if (written + r > len) break;
            for (size_t i = 0; i < r; ++i) dst[written + i] = buf[i];
        }

        if (*p == L'\0') {
            if (dst != nullptr) *src = nullptr;
            return written;
        }
        written += r;
        ++p;
    }

    *src = p;
    return written;
}

}