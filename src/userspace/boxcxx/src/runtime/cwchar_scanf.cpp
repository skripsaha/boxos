// boxcxx — <cwchar> runtime, formatted wide input
//
// ── What is shared with the narrow engine, and what cannot be ──────────────
// The VALUE conversion is shared: a gathered token is handed to `wcstoll`,
// `wcstoull` or `wcstod`, which narrow it one character to one character and
// call the `strto*` that <cstdlib> already has. So there is one decimal-to-
// binary path, one set of overflow rules, and one answer to "is this hex".
//
// The TOKENIZER cannot be shared, and the reason is not laziness. Two things
// the narrow engine does are wrong at this width:
//   * white space is `iswspace`, which has been Unicode-wide since Ф42-a, so a
//     field may be preceded by U+2003 and the narrow `isspace` would stop dead
//     on the first byte of it;
//   * a field width counts CHARACTERS, and the narrow engine counts bytes.
// A tokenizer that got either wrong would be silently reading the wrong field.
//
// ── How much look-ahead, and why exactly that much ────────────────────────
// The gatherers below over-read by a bounded amount — `0x` with no hex digit
// after it, a `-` that begins nothing, the prefixes of `inf`, `infinity` and
// `nan`. `wcstod` and `wcstoll` then report through `endptr` how much of the
// token was really theirs, and everything past it is pushed back. The stack is
// eight deep because `infinity` is eight characters: Ф41 measured that when a
// single-cell push-back swallowed seven of them.

#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cwchar>
#include <cwctype>

namespace {

using ::std::size_t;
using ::std::wint_t;

constexpr wint_t kEnd    = WEOF;
constexpr int    kPbCap  = 8;
constexpr int    kTokCap = 512;

// Either a stream or a wide array. `taken` is what %n reports: characters
// pulled from the source and not pushed back.
struct WSource {
    ::std::FILE   *file;
    const wchar_t *str;
    size_t         pos;
    wint_t         pb[kPbCap];
    int            npb;
    bool           hit_end;
    long long      taken;
};

wint_t Get(WSource &s)
{
    if (s.npb > 0) { ++s.taken; return s.pb[--s.npb]; }
    wint_t c;
    if (s.file) {
        c = ::std::fgetwc(s.file);
    } else {
        c = s.str[s.pos] == L'\0' ? kEnd : static_cast<wint_t>(s.str[s.pos]);
        if (c != kEnd) ++s.pos;
    }
    if (c == kEnd) { s.hit_end = true; return kEnd; }
    ++s.taken;
    return c;
}

void Unget(WSource &s, wint_t c)
{
    if (c == kEnd) return;
    if (s.npb < kPbCap) { s.pb[s.npb++] = c; --s.taken; }
}

bool AtEnd(const WSource &s) { return s.npb == 0 && s.hit_end; }

// Unicode white space, not ASCII white space. This is the line the narrow
// engine cannot be asked to walk.
void SkipSpace(WSource &s)
{
    wint_t c;
    while ((c = Get(s)) != kEnd && ::std::iswspace(c)) {}
    Unget(s, c);
}

// ── the parsed directive ────────────────────────────────────────────────────
struct Spec {
    bool suppress;
    int  width;      // 0 when absent
    char length;     // 0, 'h', 'H' (hh), 'l', 'q' (ll), 'j', 'z', 't', 'L'
};

void StoreSigned(void *dst, char length, long long v)
{
    switch (length) {
    case 'H': *static_cast<signed char *>(dst) = static_cast<signed char>(v); break;
    case 'h': *static_cast<short *>(dst)       = static_cast<short>(v); break;
    case 'l': *static_cast<long *>(dst)        = static_cast<long>(v); break;
    case 'q': *static_cast<long long *>(dst)   = v; break;
    case 'j': *static_cast<::std::intmax_t *>(dst)  = static_cast<::std::intmax_t>(v); break;
    case 'z': *static_cast<::std::ptrdiff_t *>(dst) = static_cast<::std::ptrdiff_t>(v); break;
    case 't': *static_cast<::std::ptrdiff_t *>(dst) = static_cast<::std::ptrdiff_t>(v); break;
    default:  *static_cast<int *>(dst)         = static_cast<int>(v); break;
    }
}

void StoreUnsigned(void *dst, char length, unsigned long long v)
{
    switch (length) {
    case 'H': *static_cast<unsigned char *>(dst)  = static_cast<unsigned char>(v); break;
    case 'h': *static_cast<unsigned short *>(dst) = static_cast<unsigned short>(v); break;
    case 'l': *static_cast<unsigned long *>(dst)  = static_cast<unsigned long>(v); break;
    case 'q': *static_cast<unsigned long long *>(dst) = v; break;
    case 'j': *static_cast<::std::uintmax_t *>(dst) = static_cast<::std::uintmax_t>(v); break;
    case 'z': case 't': *static_cast<size_t *>(dst) = static_cast<size_t>(v); break;
    default:  *static_cast<unsigned int *>(dst)   = static_cast<unsigned int>(v); break;
    }
}

bool HexDigit(wint_t c)
{
    return (c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f') || (c >= L'A' && c <= L'F');
}

// Collects an integer field. The base is resolved BEFORE any digit is taken,
// and only digits valid in that base are gathered, so what wcstoll declines is
// at most the two characters of a `0x` that led nowhere.
//
// Gathering generously and letting the converter sort it out was the first
// version, and it was wrong in a way no reading would have shown: `%i` on
// "0888888888888" resolves to octal, so eighteen characters became tail, the
// eight-deep push-back silently dropped ten of them, and the NEXT field read
// the wrong number. Measured against the host, which returned 888888888888
// where this returned 8888888.
//
// The push-back depth stays eight because that is what `infinity` needs; the
// invariant that keeps it sufficient is this function, not its size.
int GatherInteger(WSource &s, wchar_t *buf, int width, unsigned base)
{
    int    n = 0;
    wint_t c = Get(s);
    if (c == L'+' || c == L'-') {
        if (width && n >= width) { Unget(s, c); return n; }
        buf[n++] = static_cast<wchar_t>(c);
        c = Get(s);
    }

    // Resolve the base from the prefix, consuming only what the prefix really is.
    if (c == L'0' && (!width || n < width)) {
        buf[n++] = L'0';
        c = Get(s);
        if ((base == 0 || base == 16) && (c == L'x' || c == L'X')) {
            const wint_t d = Get(s);
            if (HexDigit(d) && (!width || n + 1 < width)) {
                buf[n++] = static_cast<wchar_t>(c);
                base = 16;
                c    = d;
            } else {
                // "0x" that leads nowhere: the field is the 0 alone, and both
                // characters go back in the order they were read.
                Unget(s, d);
                Unget(s, c);
                buf[n] = L'\0';
                return n;
            }
        } else if (base == 0) {
            base = 8;
        }
    } else if (base == 0) {
        base = 10;
    }

    while (c != kEnd && n < kTokCap - 1 && (!width || n < width)) {
        const bool ok = base == 16 ? HexDigit(c)
                      : base == 8  ? (c >= L'0' && c <= L'7')
                      : base == 10 ? (c >= L'0' && c <= L'9')
                                   : (c >= L'0' && c <= L'9') ||
                                     (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z');
        if (!ok) break;
        buf[n++] = static_cast<wchar_t>(c);
        c = Get(s);
    }
    Unget(s, c);
    buf[n] = L'\0';
    return n;
}

// The floating field, including the words. `inf`, `infinity` and `nan` are
// gathered whole so wcstod can recognise them; the eight-deep push-back is what
// makes `infinity` safe to try and abandon.
int GatherFloat(WSource &s, wchar_t *buf, int width)
{
    int    n = 0;
    wint_t c = Get(s);
    if (c == L'+' || c == L'-') {
        buf[n++] = static_cast<wchar_t>(c);
        c = Get(s);
    }
    bool hex = false;
    bool seenDot = false;
    if (c == L'0' && (!width || n + 1 < width)) {
        buf[n++] = L'0';
        c = Get(s);
        if (c == L'x' || c == L'X') {
            buf[n++] = static_cast<wchar_t>(c);
            hex = true;
            c = Get(s);
        }
    }
    while (c != kEnd && n < kTokCap - 1 && (!width || n < width)) {
        const bool digit = hex ? HexDigit(c) : (c >= L'0' && c <= L'9');
        const bool expo  = hex ? (c == L'p' || c == L'P') : (c == L'e' || c == L'E');
        if (c == L'.') {
            // One radix point only. Gathering "1.2.3.4.5.6" whole would leave
            // wcstold declining everything after "1.2", and a tail that long
            // does not fit the push-back — the same defect the integer
            // gatherer had.
            if (seenDot) break;
            seenDot = true;
            buf[n++] = static_cast<wchar_t>(c);
            c = Get(s);
            continue;
        }
        if (digit) {
            buf[n++] = static_cast<wchar_t>(c);
            c = Get(s);
            continue;
        }
        if (expo) {
            const wint_t sign = Get(s);
            const wint_t dig  = (sign == L'+' || sign == L'-') ? Get(s) : sign;
            if (dig >= L'0' && dig <= L'9') {
                buf[n++] = static_cast<wchar_t>(c);
                if (sign == L'+' || sign == L'-') buf[n++] = static_cast<wchar_t>(sign);
                buf[n++] = static_cast<wchar_t>(dig);
                c = Get(s);
                continue;
            }
            if (sign == L'+' || sign == L'-') Unget(s, dig);
            Unget(s, sign);
            break;
        }
        // inf / infinity / nan, tried and abandoned whole.
        if (n == 0 || (n == 1 && (buf[0] == L'+' || buf[0] == L'-'))) {
            const wchar_t *word = nullptr;
            if (c == L'i' || c == L'I') word = L"infinity";
            else if (c == L'n' || c == L'N') word = L"nan";
            if (word) {
                wint_t seen[9];
                int    k = 0;
                int    matched = 0;
                wint_t cur = c;
                while (word[k] != L'\0' && cur != kEnd && k < 8) {
                    if (::std::towlower(cur) != static_cast<wint_t>(word[k])) break;
                    seen[k] = cur;
                    ++k;
                    if (k == 3 || word[k] == L'\0') matched = k;
                    cur = Get(s);
                }
                Unget(s, cur);
                while (k > matched) Unget(s, seen[--k]);
                for (int i = 0; i < matched && n < kTokCap - 1; ++i)
                    buf[n++] = static_cast<wchar_t>(seen[i]);
                c = kEnd;
                break;
            }
        }
        break;
    }
    Unget(s, c);
    buf[n] = L'\0';
    return n;
}

// Pushes back everything wcsto* declined, so the next directive sees it.
void PushBackTail(WSource &s, const wchar_t *buf, int n, const wchar_t *endp)
{
    int used = endp ? static_cast<int>(endp - buf) : 0;
    for (int i = n; i > used; --i) Unget(s, static_cast<wint_t>(buf[i - 1]));
}

int ReadInt(const wchar_t *&p)
{
    int v = 0;
    while (*p >= L'0' && *p <= L'9') { v = v * 10 + (*p - L'0'); ++p; }
    return v;
}

int Run(WSource &s, const wchar_t *fmt, va_list ap)
{
    int assigned = 0;

    while (*fmt != L'\0') {
        if (::std::iswspace(static_cast<wint_t>(*fmt))) {
            SkipSpace(s);
            while (::std::iswspace(static_cast<wint_t>(*fmt))) ++fmt;
            continue;
        }
        if (*fmt != L'%') {
            const wint_t c = Get(s);
            if (c != static_cast<wint_t>(*fmt)) { Unget(s, c); return assigned; }
            ++fmt;
            continue;
        }
        ++fmt;
        if (*fmt == L'%') {
            SkipSpace(s);
            const wint_t c = Get(s);
            if (c != L'%') { Unget(s, c); return assigned; }
            ++fmt;
            continue;
        }

        Spec sp{};
        if (*fmt == L'*') { sp.suppress = true; ++fmt; }
        if (*fmt >= L'0' && *fmt <= L'9') sp.width = ReadInt(fmt);
        if (*fmt == L'h') { ++fmt; sp.length = 'h'; if (*fmt == L'h') { ++fmt; sp.length = 'H'; } }
        else if (*fmt == L'l') { ++fmt; sp.length = 'l'; if (*fmt == L'l') { ++fmt; sp.length = 'q'; } }
        else if (*fmt == L'j' || *fmt == L'z' || *fmt == L't' || *fmt == L'L') {
            sp.length = static_cast<char>(*fmt);
            ++fmt;
        }

        const wchar_t conv = *fmt;
        if (conv == L'\0') break;
        ++fmt;

        wchar_t tok[kTokCap];

        switch (conv) {
        case L'n': {
            if (sp.suppress) break;
            void *p = va_arg(ap, void *);
            StoreSigned(p, sp.length, s.taken);
            break;      // %n is not a conversion and is not counted
        }

        case L'd': case L'i': case L'o': case L'u': case L'x': case L'X': case L'p': {
            SkipSpace(s);
            if (AtEnd(s)) return assigned ? assigned : -1;
            const unsigned base = conv == L'd' || conv == L'u' ? 10u
                                : conv == L'o'                 ? 8u
                                : conv == L'i'                 ? 0u
                                                               : 16u;
            const int n = GatherInteger(s, tok, sp.width, base);
            if (n == 0) return assigned;
            wchar_t *endp = nullptr;
            const bool uns = conv == L'o' || conv == L'u' || conv == L'x' ||
                             conv == L'X' || conv == L'p';
            unsigned long long uv = 0;
            long long          sv = 0;
            if (uns) uv = ::std::wcstoull(tok, &endp, static_cast<int>(base));
            else     sv = ::std::wcstoll(tok, &endp, static_cast<int>(base));
            if (!endp || endp == tok) { PushBackTail(s, tok, n, tok); return assigned; }
            PushBackTail(s, tok, n, endp);
            if (!sp.suppress) {
                void *p = va_arg(ap, void *);
                if (conv == L'p') *static_cast<void **>(p) = reinterpret_cast<void *>(uv);
                else if (uns) StoreUnsigned(p, sp.length, uv);
                else StoreSigned(p, sp.length, sv);
                ++assigned;
            }
            break;
        }

        case L'f': case L'F': case L'e': case L'E':
        case L'g': case L'G': case L'a': case L'A': {
            SkipSpace(s);
            if (AtEnd(s)) return assigned ? assigned : -1;
            const int n = GatherFloat(s, tok, sp.width);
            if (n == 0) return assigned;
            wchar_t *endp = nullptr;
            const long double v = ::std::wcstold(tok, &endp);
            if (!endp || endp == tok) { PushBackTail(s, tok, n, tok); return assigned; }
            PushBackTail(s, tok, n, endp);
            if (!sp.suppress) {
                void *p = va_arg(ap, void *);
                switch (sp.length) {
                case 'l': *static_cast<double *>(p)      = static_cast<double>(v); break;
                case 'L': *static_cast<long double *>(p) = v; break;
                default:  *static_cast<float *>(p)       = static_cast<float>(v); break;
                }
                ++assigned;
            }
            break;
        }

        case L'c': {
            const int want = sp.width ? sp.width : 1;
            if (AtEnd(s)) return assigned ? assigned : -1;
            void *p = sp.suppress ? nullptr : va_arg(ap, void *);
            int   got = 0;
            for (; got < want; ++got) {
                const wint_t c = Get(s);
                if (c == kEnd) break;
                if (!p) continue;
                if (sp.length == 'l') {
                    static_cast<wchar_t *>(p)[got] = static_cast<wchar_t>(c);
                } else {
                    // No `l`: C converts to multibyte and stores bytes, so the
                    // caller's char array receives UTF-8 rather than truncated
                    // code points.
                    char             mb[8];
                    ::std::mbstate_t st{};
                    const size_t     r = ::std::wcrtomb(mb, static_cast<wchar_t>(c), &st);
                    if (r == static_cast<size_t>(-1)) return assigned ? assigned : -1;
                    for (size_t i = 0; i < r; ++i) static_cast<char *>(p)[i] = mb[i];
                    p = static_cast<char *>(p) + r;
                    continue;
                }
            }
            if (got == 0) return assigned;
            if (p && !sp.suppress) ++assigned;
            break;
        }

        case L's': {
            SkipSpace(s);
            if (AtEnd(s)) return assigned ? assigned : -1;
            void *p   = sp.suppress ? nullptr : va_arg(ap, void *);
            int   got = 0;
            char *mbp = static_cast<char *>(p);
            for (;;) {
                if (sp.width && got >= sp.width) break;
                const wint_t c = Get(s);
                if (c == kEnd) break;
                if (::std::iswspace(c)) { Unget(s, c); break; }
                if (p) {
                    if (sp.length == 'l') {
                        static_cast<wchar_t *>(p)[got] = static_cast<wchar_t>(c);
                    } else {
                        char             mb[8];
                        ::std::mbstate_t st{};
                        const size_t     r = ::std::wcrtomb(mb, static_cast<wchar_t>(c), &st);
                        if (r == static_cast<size_t>(-1)) return assigned ? assigned : -1;
                        for (size_t i = 0; i < r; ++i) *mbp++ = mb[i];
                    }
                }
                ++got;
            }
            if (got == 0) return assigned;
            if (p) {
                if (sp.length == 'l') static_cast<wchar_t *>(p)[got] = L'\0';
                else *mbp = '\0';
                ++assigned;
            }
            break;
        }

        case L'[': {
            bool invert = false;
            if (*fmt == L'^') { invert = true; ++fmt; }
            const wchar_t *setBegin = fmt;
            if (*fmt == L']') ++fmt;                 // a leading ] is a member
            while (*fmt != L']' && *fmt != L'\0') ++fmt;
            const wchar_t *setEnd = fmt;
            if (*fmt == L']') ++fmt;

            if (AtEnd(s)) return assigned ? assigned : -1;
            void *p   = sp.suppress ? nullptr : va_arg(ap, void *);
            int   got = 0;
            char *mbp = static_cast<char *>(p);
            for (;;) {
                if (sp.width && got >= sp.width) break;
                const wint_t c = Get(s);
                if (c == kEnd) break;
                bool in = false;
                for (const wchar_t *q = setBegin; q < setEnd && !in; ++q)
                    if (static_cast<wint_t>(*q) == c) in = true;
                if (in == invert) { Unget(s, c); break; }
                if (p) {
                    if (sp.length == 'l') {
                        static_cast<wchar_t *>(p)[got] = static_cast<wchar_t>(c);
                    } else {
                        char             mb[8];
                        ::std::mbstate_t st{};
                        const size_t     r = ::std::wcrtomb(mb, static_cast<wchar_t>(c), &st);
                        if (r == static_cast<size_t>(-1)) return assigned ? assigned : -1;
                        for (size_t i = 0; i < r; ++i) *mbp++ = mb[i];
                    }
                }
                ++got;
            }
            if (got == 0) return assigned;
            if (p) {
                if (sp.length == 'l') static_cast<wchar_t *>(p)[got] = L'\0';
                else *mbp = '\0';
                ++assigned;
            }
            break;
        }

        default:
            return assigned;
        }
    }

    if (assigned == 0 && AtEnd(s)) return -1;
    return assigned;
}

} // namespace

namespace std {

int vfwscanf(FILE *stream, const wchar_t *format, va_list arg) noexcept
{
    if (!stream || !format) return -1;
    WSource s{};
    s.file = stream;
    const int r = Run(s, format, arg);
    // The look-ahead lives in a stack local to this call, so anything still in
    // it when Run returns has been read out of the stream and would simply
    // vanish. One character can go back; C promises no more, and the narrow
    // vfscanf hands its own back on exactly these terms. It is the LAST one
    // pushed, which is the character the failing conversion refused — the one
    // [fwscanf] requires to be left unread.
    if (s.npb > 0) ungetwc(s.pb[s.npb - 1], stream);
    return r;
}

int vwscanf(const wchar_t *format, va_list arg) noexcept
{
    return vfwscanf(stdin, format, arg);
}

int vswscanf(const wchar_t *s, const wchar_t *format, va_list arg) noexcept
{
    if (!s || !format) return -1;
    WSource src{};
    src.str = s;
    return Run(src, format, arg);
}

int fwscanf(FILE *stream, const wchar_t *format, ...) noexcept
{
    va_list ap;
    va_start(ap, format);
    const int rc = vfwscanf(stream, format, ap);
    va_end(ap);
    return rc;
}

int wscanf(const wchar_t *format, ...) noexcept
{
    va_list ap;
    va_start(ap, format);
    const int rc = vfwscanf(stdin, format, ap);
    va_end(ap);
    return rc;
}

int swscanf(const wchar_t *s, const wchar_t *format, ...) noexcept
{
    va_list ap;
    va_start(ap, format);
    const int rc = vswscanf(s, format, ap);
    va_end(ap);
    return rc;
}

} // namespace std
