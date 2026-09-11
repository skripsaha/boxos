
#include <cerrno>
#include <cstddef>
#include <cwchar>
#include <__bits/c_file>

namespace {

using ::std::__stdio::ClaimByte;
using ::std::__stdio::ClaimWide;
using ::std::__stdio::GetLocked;
using ::std::__stdio::Guard;
using ::std::__stdio::kEof;
using ::std::__stdio::kErr;
using ::std::__stdio::PutLocked;


::std::wint_t GetWideLocked(::std::FILE *f) noexcept
{
    if (f->__unget != EOF) {
        const ::std::wint_t w = static_cast<::std::wint_t>(f->__unget);
        f->__unget = EOF;
        return w;
    }

    ::std::mbstate_t st{};
    for (int taken = 0; taken < 4; ++taken) {
        const int b = GetLocked(f);
        if (b == EOF) {
            if (taken > 0) { f->__flags |= kErr; errno = EILSEQ; }
            return WEOF;
        }
        const char        byte = static_cast<char>(b);
        wchar_t           wc   = 0;
        const ::std::size_t r  = ::std::mbrtowc(&wc, &byte, 1, &st);
        if (r == static_cast<::std::size_t>(-2)) continue;
        if (r == static_cast<::std::size_t>(-1)) {
            f->__flags |= kErr;
            errno = EILSEQ;
            return WEOF;
        }
        return static_cast<::std::wint_t>(wc);
    }
    f->__flags |= kErr;
    errno = EILSEQ;
    return WEOF;
}

::std::wint_t PutWideLocked(::std::FILE *f, wchar_t c) noexcept
{
    char              buf[8];
    ::std::mbstate_t  st{};
    const ::std::size_t n = ::std::wcrtomb(buf, c, &st);
    if (n == static_cast<::std::size_t>(-1)) {
        f->__flags |= kErr;
        errno = EILSEQ;
        return WEOF;
    }
    for (::std::size_t i = 0; i < n; ++i)
        if (PutLocked(f, static_cast<unsigned char>(buf[i])) == EOF) return WEOF;
    return static_cast<::std::wint_t>(c);
}

}

namespace std {

int fwide(FILE *stream, int mode) noexcept
{
    if (!stream) return 0;
    Guard g(stream);
    if (mode > 0) ClaimWide(stream);
    else if (mode < 0) ClaimByte(stream);
    if (stream->__flags & __stdio::kWideOriented) return 1;
    if (stream->__flags & __stdio::kByteOriented) return -1;
    return 0;
}

wint_t fgetwc(FILE *stream) noexcept
{
    if (!stream) return WEOF;
    Guard g(stream);
    if (!ClaimWide(stream)) { stream->__flags |= kErr; return WEOF; }
    return GetWideLocked(stream);
}

wint_t fputwc(wchar_t c, FILE *stream) noexcept
{
    if (!stream) return WEOF;
    Guard g(stream);
    if (!ClaimWide(stream)) { stream->__flags |= kErr; return WEOF; }
    return PutWideLocked(stream, c);
}

wint_t getwc(FILE *stream) noexcept { return fgetwc(stream); }
wint_t putwc(wchar_t c, FILE *stream) noexcept { return fputwc(c, stream); }
wint_t getwchar() noexcept { return fgetwc(stdin); }
wint_t putwchar(wchar_t c) noexcept { return fputwc(c, stdout); }

wint_t ungetwc(wint_t c, FILE *stream) noexcept
{
    if (!stream || c == WEOF) return WEOF;
    Guard g(stream);
    if (!ClaimWide(stream)) return WEOF;
    if (stream->__unget != EOF) return WEOF;
    stream->__unget = static_cast<int>(c);
    stream->__flags &= ~kEof;
    return c;
}

wchar_t *fgetws(wchar_t *s, int n, FILE *stream) noexcept
{
    if (!s || n <= 0 || !stream) return nullptr;
    Guard g(stream);
    if (!ClaimWide(stream)) { stream->__flags |= kErr; return nullptr; }

    int i = 0;
    while (i < n - 1) {
        const wint_t w = GetWideLocked(stream);
        if (w == WEOF) { if (i == 0) return nullptr; break; }
        s[i++] = static_cast<wchar_t>(w);
        if (w == static_cast<wint_t>(L'\n')) break;
    }
    s[i] = L'\0';
    return s;
}

int fputws(const wchar_t *s, FILE *stream) noexcept
{
    if (!s || !stream) return -1;
    Guard g(stream);
    if (!ClaimWide(stream)) { stream->__flags |= kErr; return -1; }
    for (; *s != L'\0'; ++s)
        if (PutWideLocked(stream, *s) == WEOF) return -1;
    return 0;
}

}