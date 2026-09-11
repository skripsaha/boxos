
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cwchar>
#include <new>

namespace {

using ::std::size_t;
using ::std::wint_t;

struct WSink {
    ::std::FILE *file;
    wchar_t     *buf;
    size_t       cap;
    size_t       used;
    long long    count;
    bool         overflow;
    bool         failed;
};

void Put(WSink &s, wchar_t c)
{
    if (s.failed) return;
    if (s.file) {
        if (::std::fputwc(c, s.file) == WEOF) { s.failed = true; return; }
    } else if (s.used + 1 < s.cap) {
        s.buf[s.used++] = c;
    } else {
        s.overflow = true;
    }
    s.count++;
}

void PutN(WSink &s, const wchar_t *p, size_t n)
{
    for (size_t i = 0; i < n && !s.failed; ++i) Put(s, p[i]);
}

void PutRepeat(WSink &s, wchar_t c, long long n)
{
    for (long long i = 0; i < n && !s.failed; ++i) Put(s, c);
}

struct Spec {
    bool left;
    long long width;
    long long prec;
    char      length;
    wchar_t   conv;
};

long long ReadNum(const wchar_t *&p)
{
    long long v = 0;
    while (*p >= L'0' && *p <= L'9') {
        v = v * 10 + (*p - L'0');
        if (v > 1000000) v = 1000000;
        ++p;
    }
    return v;
}

void PadOut(WSink &s, const Spec &sp, const wchar_t *body, long long n)
{
    const long long pad = sp.width > n ? sp.width - n : 0;
    if (!sp.left) PutRepeat(s, L' ', pad);
    PutN(s, body, static_cast<size_t>(n));
    if (sp.left) PutRepeat(s, L' ', pad);
}

int Narrow(char *out, size_t cap, const Spec &sp, const wchar_t *flags,
           size_t nflags)
{
    size_t k = 0;
    if (k < cap) out[k++] = '%';
    for (size_t i = 0; i < nflags && k + 1 < cap; ++i)
        out[k++] = static_cast<char>(flags[i]);
    if (sp.width >= 0) {
        char tmp[24];
        int  t = 0;
        long long w = sp.width;
        if (w == 0) tmp[t++] = '0';
        while (w > 0) { tmp[t++] = static_cast<char>('0' + w % 10); w /= 10; }
        while (t > 0 && k + 1 < cap) out[k++] = tmp[--t];
    }
    if (sp.prec >= 0) {
        if (k + 1 < cap) out[k++] = '.';
        char tmp[24];
        int  t = 0;
        long long w = sp.prec;
        if (w == 0) tmp[t++] = '0';
        while (w > 0) { tmp[t++] = static_cast<char>('0' + w % 10); w /= 10; }
        while (t > 0 && k + 1 < cap) out[k++] = tmp[--t];
    }
    switch (sp.length) {
    case 'H': if (k + 2 < cap) { out[k++] = 'h'; out[k++] = 'h'; } break;
    case 'q': if (k + 2 < cap) { out[k++] = 'l'; out[k++] = 'l'; } break;
    case 0:   break;
    default:  if (k + 1 < cap) out[k++] = sp.length; break;
    }
    if (k + 1 < cap) out[k++] = static_cast<char>(sp.conv);
    out[k] = '\0';
    return static_cast<int>(k);
}

}

namespace std {
namespace {

int Run(WSink &s, const wchar_t *fmt, va_list ap)
{
    while (*fmt != L'\0' && !s.failed) {
        if (*fmt != L'%') { Put(s, *fmt++); continue; }
        ++fmt;
        if (*fmt == L'%') { Put(s, L'%'); ++fmt; continue; }

        Spec    sp{};
        sp.width = -1;
        sp.prec  = -1;
        wchar_t flags[8];
        size_t  nflags = 0;

        for (;;) {
            if (*fmt == L'-') sp.left = true;
            else if (*fmt != L'+' && *fmt != L' ' && *fmt != L'#' && *fmt != L'0') break;
            if (nflags < 8) flags[nflags++] = *fmt;
            ++fmt;
        }
        if (*fmt == L'*') {
            sp.width = va_arg(ap, int);
            if (sp.width < 0) {
                sp.left  = true;
                sp.width = -sp.width;
                if (nflags < 8) flags[nflags++] = L'-';
            }
            ++fmt;
        } else if (*fmt >= L'0' && *fmt <= L'9') {
            sp.width = ReadNum(fmt);
        }
        if (*fmt == L'.') {
            ++fmt;
            if (*fmt == L'*') { sp.prec = va_arg(ap, int); ++fmt; if (sp.prec < 0) sp.prec = -1; }
            else sp.prec = ReadNum(fmt);
        }
        if (*fmt == L'h') { ++fmt; sp.length = 'h'; if (*fmt == L'h') { ++fmt; sp.length = 'H'; } }
        else if (*fmt == L'l') { ++fmt; sp.length = 'l'; if (*fmt == L'l') { ++fmt; sp.length = 'q'; } }
        else if (*fmt == L'j' || *fmt == L'z' || *fmt == L't' || *fmt == L'L') {
            sp.length = static_cast<char>(*fmt);
            ++fmt;
        }
        sp.conv = *fmt;
        if (sp.conv == L'\0') break;
        ++fmt;

        if (sp.conv == L'c') {
            wchar_t c;
            if (sp.length == 'l') {
                c = static_cast<wchar_t>(va_arg(ap, wint_t));
            } else {
                const wint_t w = btowc(va_arg(ap, int));
                if (w == WEOF) { s.failed = true; break; }
                c = static_cast<wchar_t>(w);
            }
            PadOut(s, sp, &c, 1);
            continue;
        }
        if (sp.conv == L's') {
            if (sp.length == 'l') {
                const wchar_t *w = va_arg(ap, const wchar_t *);
                if (!w) w = L"(null)";
                long long n = 0;
                while (w[n] != L'\0' && (sp.prec < 0 || n < sp.prec)) ++n;
                PadOut(s, sp, w, n);
            } else {
                const char *m = va_arg(ap, const char *);
                if (!m) m = "(null)";

                long long n = 0;
                {
                    mbstate_t   st{};
                    const char *q = m;
                    while (sp.prec < 0 || n < sp.prec) {
                        wchar_t      wc = 0;
                        const size_t r  = mbrtowc(&wc, q, 4, &st);
                        if (r == 0 || r == (size_t)-1 || r == (size_t)-2) break;
                        ++n;
                        q += r;
                    }
                }
                const long long pad = sp.width > n ? sp.width - n : 0;
                if (!sp.left) PutRepeat(s, L' ', pad);
                {
                    mbstate_t   st{};
                    const char *q = m;
                    for (long long k = 0; k < n && !s.failed; ++k) {
                        wchar_t      wc = 0;
                        const size_t r  = mbrtowc(&wc, q, 4, &st);
                        if (r == 0 || r == (size_t)-1 || r == (size_t)-2) break;
                        Put(s, wc);
                        q += r;
                    }
                }
                if (sp.left) PutRepeat(s, L' ', pad);
            }
            continue;
        }
        if (sp.conv == L'n') {
            void *p = va_arg(ap, void *);
            switch (sp.length) {
            case 'H': *static_cast<signed char *>(p) = static_cast<signed char>(s.count); break;
            case 'h': *static_cast<short *>(p)       = static_cast<short>(s.count); break;
            case 'l': *static_cast<long *>(p)        = static_cast<long>(s.count); break;
            case 'q': *static_cast<long long *>(p)   = s.count; break;
            case 'j': *static_cast<intmax_t *>(p)    = static_cast<intmax_t>(s.count); break;
            case 'z': *static_cast<size_t *>(p)      = static_cast<size_t>(s.count); break;
            case 't': *static_cast<ptrdiff_t *>(p)   = static_cast<ptrdiff_t>(s.count); break;
            default:  *static_cast<int *>(p)         = static_cast<int>(s.count); break;
            }
            continue;
        }

        char ndir[48];
        Narrow(ndir, sizeof ndir, sp, flags, nflags);

        auto emit = [&](auto value) {
            char      small[512];
            const int wrote = snprintf(small, sizeof small, ndir, value);
            if (wrote < 0) { s.failed = true; return; }
            if (static_cast<::std::size_t>(wrote) < sizeof small) {
                for (int i = 0; i < wrote; ++i)
                    Put(s, static_cast<wchar_t>(static_cast<unsigned char>(small[i])));
                return;
            }
            char *big = static_cast<char *>(
                ::operator new[](static_cast<::std::size_t>(wrote) + 1, ::std::nothrow));
            if (!big) { s.failed = true; return; }
            const int again = snprintf(big, static_cast<::std::size_t>(wrote) + 1, ndir, value);
            for (int i = 0; i < again; ++i)
                Put(s, static_cast<wchar_t>(static_cast<unsigned char>(big[i])));
            ::operator delete[](big);
        };

        switch (sp.conv) {
        case L'd': case L'i':
            switch (sp.length) {
            case 'q': emit(va_arg(ap, long long)); break;
            case 'l': emit(va_arg(ap, long)); break;
            case 'j': emit(va_arg(ap, intmax_t)); break;
            case 'z': case 't': emit(va_arg(ap, ptrdiff_t)); break;
            default:  emit(va_arg(ap, int)); break;
            }
            break;
        case L'o': case L'u': case L'x': case L'X':
            switch (sp.length) {
            case 'q': emit(va_arg(ap, unsigned long long)); break;
            case 'l': emit(va_arg(ap, unsigned long)); break;
            case 'j': emit(va_arg(ap, uintmax_t)); break;
            case 'z': case 't': emit(va_arg(ap, size_t)); break;
            default:  emit(va_arg(ap, unsigned int)); break;
            }
            break;
        case L'f': case L'F': case L'e': case L'E':
        case L'g': case L'G': case L'a': case L'A':
            if (sp.length == 'L') emit(va_arg(ap, long double));
            else                  emit(va_arg(ap, double));
            break;
        case L'p':
            emit(va_arg(ap, void *));
            break;
        default:
            Put(s, L'%');
            Put(s, sp.conv);
            continue;
        }
    }

    if (s.failed) return -1;
    return static_cast<int>(s.count);
}

}

int vfwprintf(FILE *stream, const wchar_t *format, va_list arg) noexcept
{
    if (!stream || !format) return -1;
    WSink s{};
    s.file = stream;
    return Run(s, format, arg);
}

int vwprintf(const wchar_t *format, va_list arg) noexcept
{
    return vfwprintf(stdout, format, arg);
}

int vswprintf(wchar_t *s, size_t n, const wchar_t *format, va_list arg) noexcept
{
    if (!s || n == 0 || !format) return -1;
    WSink sink{};
    sink.buf = s;
    sink.cap = n;
    const int rc = Run(sink, format, arg);
    s[sink.used] = L'\0';
    if (rc < 0 || sink.overflow) return -1;
    return rc;
}

int fwprintf(FILE *stream, const wchar_t *format, ...) noexcept
{
    va_list ap;
    va_start(ap, format);
    const int rc = vfwprintf(stream, format, ap);
    va_end(ap);
    return rc;
}

int wprintf(const wchar_t *format, ...) noexcept
{
    va_list ap;
    va_start(ap, format);
    const int rc = vfwprintf(stdout, format, ap);
    va_end(ap);
    return rc;
}

int swprintf(wchar_t *s, size_t n, const wchar_t *format, ...) noexcept
{
    va_list ap;
    va_start(ap, format);
    const int rc = vswprintf(s, n, format, ap);
    va_end(ap);
    return rc;
}

}