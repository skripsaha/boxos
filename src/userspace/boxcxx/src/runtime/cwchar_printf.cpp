// boxcxx — <cwchar> runtime, formatted wide output
//
// ── There is no second formatter ───────────────────────────────────────────
// Every conversion except the character and string ones produces ASCII and
// nothing else: digits, a sign, `0x`, an exponent, `inf`, `nan`. So a numeric
// directive is copied out of the wide format string one character to one
// character, handed to the narrow engine <cstdio> already has, and the ASCII it
// returns is widened. The correctly-rounded floating-point path of Ф27 comes
// along without being ported, and there is exactly one place where `%g` decides
// how many digits to keep.
//
// That is the same answer Ф41 reached about `printf` itself: the system has one
// formatting engine, and a second would be a second thing to keep correct.
//
// ── What the wide layer must do itself, and why ────────────────────────────
// Width and precision are counted in WIDE CHARACTERS here and in BYTES there.
// For a numeric conversion the two agree, because the output is ASCII and one
// byte is one character. For `%c`, `%lc`, `%s` and `%ls` they do not:
// `fwprintf(L"%10ls", L"")` must pad to ten characters, and the narrow
// engine would pad to ten bytes of UTF-8, which is a different picture on the
// screen. So those four are padded here, over wide characters, and only those
// four.
//
// `%n` counts wide characters for the same reason.

#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cwchar>
#include <new>

namespace {

using ::std::size_t;
using ::std::wint_t;

// Where formatted output goes. A FILE* for fwprintf, a wide array for
// swprintf; `count` is what the function returns, and it keeps rising after
// the array is full so an overflow can be reported.
struct WSink {
    ::std::FILE *file;
    wchar_t     *buf;
    size_t       cap;      // wide characters the array can hold, INCLUDING the null
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

// ── the parsed directive ────────────────────────────────────────────────────
struct Spec {
    bool left;          // '-'
    long long width;    // -1 when absent
    long long prec;     // -1 when absent
    char      length;   // 0, 'h', 'H' (hh), 'l', 'q' (ll), 'j', 'z', 't', 'L'
    wchar_t   conv;
};

// Saturating, and that is an OVERFLOW GUARD rather than a policy. A field
// width is written by the caller and nothing bounds its digits, so `v * 10`
// on a plain int is signed overflow — undefined, not merely large. This is not
// <format>'s deliberate 65535 cap (§3 of CONFORMANCE): the number here is high
// enough that no width a program really writes is changed by it.
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

// Pads a run of wide characters to the field width, counting characters.
void PadOut(WSink &s, const Spec &sp, const wchar_t *body, long long n)
{
    const long long pad = sp.width > n ? sp.width - n : 0;
    if (!sp.left) PutRepeat(s, L' ', pad);
    PutN(s, body, static_cast<size_t>(n));
    if (sp.left) PutRepeat(s, L' ', pad);
}

// ── the numeric directives, formatted by the one engine ─────────────────────
//
// The directive is rebuilt narrow with its width and precision already
// resolved, so a `*` never reaches the narrow engine and the argument order
// cannot drift between the two parsers.
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

} // namespace

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
                // A negative * width IS the '-' flag with a positive width. It
                // has to join `flags`, not just `sp`: the narrow directive is
                // rebuilt from `flags`, so a left-justification that lived only
                // in the parsed struct would be lost on every delegated
                // conversion. Found by the differential, not by reading.
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

        // ── the four the wide layer owns, because their width is in characters
        if (sp.conv == L'c') {
            wchar_t c;
            if (sp.length == 'l') {
                c = static_cast<wchar_t>(va_arg(ap, wint_t));
            } else {
                // C: the int argument is converted as if by btowc, so anything
                // that is not a single-byte character has no wide form.
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
                // A multibyte string, decoded one character at a time so the
                // precision counts characters rather than bytes — and decoded
                // TWICE rather than buffered, because a buffer has a size and
                // a string does not. The first version held 512 characters and
                // silently produced 511 of a 2000-character argument.
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

        // ── everything else: ASCII out, so the one engine formats it ─────
        char ndir[48];
        Narrow(ndir, sizeof ndir, sp, flags, nflags);

        // The scratch is a guess, and snprintf says when the guess was wrong:
        // it returns the length it WOULD have written. Measured, before this
        // existed: %.500f of 1e300 needs 812 characters and produced 511 of
        // them, with a return value that said 512. A precision is a number the
        // caller chooses, so no fixed buffer is large enough by argument.
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
            // An unknown conversion is echoed, which is what the narrow engine
            // does and what makes a typo visible rather than silently dropped.
            Put(s, L'%');
            Put(s, sp.conv);
            continue;
        }
    }

    if (s.failed) return -1;
    return static_cast<int>(s.count);
}

} // namespace

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

// Unlike snprintf, swprintf does NOT report what it would have written: C makes
// a full buffer an error and asks for a negative return. Callers that size a
// buffer by asking first have to do it another way, and that is C's design
// rather than a limitation here.
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

} // namespace std
