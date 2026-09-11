
#include <charconv>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

constexpr int kEnd = -1;

struct Source {
    ::std::FILE  *stream;
    const char   *text;
    ::std::size_t at;
    int           pb[8];
    int           npb;
    long long     consumed;
    bool          hit_end;
};

int Get(Source &s)
{
    if (s.npb > 0) {
        s.consumed++;
        return s.pb[--s.npb];
    }
    int c;
    if (s.stream) {
        c = ::std::fgetc(s.stream);
    } else {
        c = s.text[s.at] ? static_cast<unsigned char>(s.text[s.at]) : kEnd;
        if (c != kEnd) s.at++;
    }
    if (c == kEnd) { s.hit_end = true; return kEnd; }
    s.consumed++;
    return c;
}

void Unget(Source &s, int c)
{
    if (c == kEnd) return;
    if (s.npb < static_cast<int>(sizeof(s.pb) / sizeof(s.pb[0]))) s.pb[s.npb++] = c;
    s.consumed--;
}

bool AtEnd(const Source &s) { return s.npb == 0 && s.hit_end; }

bool IsSpace(int c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r';
}

void SkipSpace(Source &s)
{
    int c;
    while ((c = Get(s)) != kEnd && IsSpace(c)) {}
    Unget(s, c);
}

void StoreSigned(void *dst, char length, long long v)
{
    switch (length) {
    case 'H': *static_cast<signed char *>(dst) = static_cast<signed char>(v); break;
    case 'h': *static_cast<short *>(dst) = static_cast<short>(v); break;
    case 'l': *static_cast<long *>(dst) = static_cast<long>(v); break;
    case 'q': *static_cast<long long *>(dst) = v; break;
    case 'j': *static_cast<::std::intmax_t *>(dst) = static_cast<::std::intmax_t>(v); break;
    case 'z': *static_cast<::std::size_t *>(dst) = static_cast<::std::size_t>(v); break;
    case 't': *static_cast<::std::ptrdiff_t *>(dst) = static_cast<::std::ptrdiff_t>(v); break;
    default:  *static_cast<int *>(dst) = static_cast<int>(v); break;
    }
}

void StoreUnsigned(void *dst, char length, unsigned long long v)
{
    switch (length) {
    case 'H': *static_cast<unsigned char *>(dst) = static_cast<unsigned char>(v); break;
    case 'h': *static_cast<unsigned short *>(dst) = static_cast<unsigned short>(v); break;
    case 'l': *static_cast<unsigned long *>(dst) = static_cast<unsigned long>(v); break;
    case 'q': *static_cast<unsigned long long *>(dst) = v; break;
    case 'j': *static_cast<::std::uintmax_t *>(dst) = static_cast<::std::uintmax_t>(v); break;
    case 'z': *static_cast<::std::size_t *>(dst) = static_cast<::std::size_t>(v); break;
    case 't': *static_cast<::std::ptrdiff_t *>(dst) = static_cast<::std::ptrdiff_t>(v); break;
    default:  *static_cast<unsigned int *>(dst) = static_cast<unsigned int>(v); break;
    }
}

int GatherInteger(Source &s, char *buf, int cap, int width, unsigned &base, bool allow_sign)
{
    int n = 0;
    auto take = [&](int c) { if (n < cap - 1) buf[n++] = static_cast<char>(c); };
    int taken = 0;
    auto budget = [&] { return width <= 0 || taken < width; };

    int c = Get(s);
    if (allow_sign && (c == '+' || c == '-') && budget()) { take(c); taken++; c = Get(s); }

    if (c == '0' && budget()) {
        take(c); taken++;
        const int c2 = Get(s);
        if ((base == 16 || base == 0) && (c2 == 'x' || c2 == 'X') && budget()) {
            const int c3 = Get(s);
            const bool hex = (c3 >= '0' && c3 <= '9') || (c3 >= 'a' && c3 <= 'f') ||
                             (c3 >= 'A' && c3 <= 'F');
            if (hex) {
                base = 16;
                n--;
                take(c3); taken += 2;
                c = Get(s);
            } else {
                Unget(s, c3);
                if (base == 0) base = 8;
                Unget(s, c2);
                return n;
            }
        } else {
            if (base == 0) base = 8;
            c = c2;
        }
    }
    if (base == 0) base = 10;

    for (;;) {
        if (c == kEnd || !budget()) break;
        int v;
        if (c >= '0' && c <= '9') v = c - '0';
        else if (c >= 'a' && c <= 'z') v = c - 'a' + 10;
        else if (c >= 'A' && c <= 'Z') v = c - 'A' + 10;
        else break;
        if (static_cast<unsigned>(v) >= base) break;
        take(c); taken++;
        c = Get(s);
    }
    Unget(s, c);
    buf[n] = '\0';
    return n;
}

enum class FloatSpecial : unsigned char { None, Inf, Nan };

bool MatchWord(Source &s, const char *word, int &taken, int width)
{
    int seen[8];
    int n = 0;
    for (const char *w = word; *w; w++) {
        if (width > 0 && taken >= width) break;
        const int c = Get(s);
        const int lower = (c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c;
        seen[n++] = c;
        if (lower != *w) {
            while (n) Unget(s, seen[--n]);
            return false;
        }
        taken++;
    }
    return n > 0 && word[n] == '\0';
}

int GatherFloat(Source &s, char *buf, int cap, int width, FloatSpecial &special)
{
    int n = 0, taken = 0;
    auto take = [&](int c) { if (n < cap - 1) buf[n++] = static_cast<char>(c); };
    auto budget = [&] { return width <= 0 || taken < width; };
    special = FloatSpecial::None;

    int c = Get(s);
    bool neg = false;
    if ((c == '+' || c == '-') && budget()) { neg = (c == '-'); take(c); taken++; c = Get(s); }

    const int lower0 = (c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c;
    if (lower0 == 'i' || lower0 == 'n') {
        Unget(s, c);
        if (MatchWord(s, "inf", taken, width)) {
            int probe = taken;
            if (!MatchWord(s, "inity", probe, width)) {  }
            else taken = probe;
            special = FloatSpecial::Inf;
            buf[0] = neg ? '-' : '+';
            buf[1] = '\0';
            return 1;
        }
        if (MatchWord(s, "nan", taken, width)) {
            special = FloatSpecial::Nan;
            buf[0] = neg ? '-' : '+';
            buf[1] = '\0';
            return 1;
        }
        c = Get(s);
    }

    bool any_digit = false;
    while (c >= '0' && c <= '9' && budget()) { take(c); taken++; any_digit = true; c = Get(s); }
    if (c == '.' && budget()) {
        take(c); taken++; c = Get(s);
        while (c >= '0' && c <= '9' && budget()) { take(c); taken++; any_digit = true; c = Get(s); }
    }
    if (any_digit && (c == 'e' || c == 'E') && budget()) {
        const int save_n = n, save_t = taken;
        take(c); taken++;
        int c2 = Get(s);
        if ((c2 == '+' || c2 == '-') && budget()) { take(c2); taken++; c2 = Get(s); }
        if (c2 >= '0' && c2 <= '9') {
            while (c2 >= '0' && c2 <= '9' && budget()) { take(c2); taken++; c2 = Get(s); }
            c = c2;
        } else {
            Unget(s, c2);
            n = save_n; taken = save_t;
        }
    }
    Unget(s, c);
    buf[n] = '\0';
    return any_digit ? n : 0;
}

int ReadInt(const char *&p)
{
    int v = 0;
    while (*p >= '0' && *p <= '9') v = v * 10 + (*p++ - '0');
    return v;
}

int Run(Source &s, const char *fmt, va_list ap)
{
    if (!fmt) return EOF;
    int assigned = 0;

    for (const char *p = fmt; *p;) {
        if (IsSpace(static_cast<unsigned char>(*p))) {
            while (IsSpace(static_cast<unsigned char>(*p))) p++;
            SkipSpace(s);
            continue;
        }
        if (*p != '%') {
            const int c = Get(s);
            if (c == kEnd) return assigned ? assigned : EOF;
            if (c != static_cast<unsigned char>(*p)) { Unget(s, c); return assigned; }
            p++;
            continue;
        }

        p++;
        if (*p == '%') {
            SkipSpace(s);
            const int c = Get(s);
            if (c == kEnd) return assigned ? assigned : EOF;
            if (c != '%') { Unget(s, c); return assigned; }
            p++;
            continue;
        }

        bool suppress = false;
        if (*p == '*') { suppress = true; p++; }
        const int width = ReadInt(p);

        char length = 0;
        if (p[0] == 'h' && p[1] == 'h') { length = 'H'; p += 2; }
        else if (p[0] == 'l' && p[1] == 'l') { length = 'q'; p += 2; }
        else if (*p == 'h' || *p == 'l' || *p == 'j' || *p == 'z' || *p == 't' || *p == 'L') {
            length = *p++;
        }
        const char conv = *p;
        if (!conv) break;
        p++;

        char buf[512];

        switch (conv) {
        case 'd': case 'i': case 'o': case 'u': case 'x': case 'X': {
            SkipSpace(s);
            unsigned base = (conv == 'o') ? 8u
                          : (conv == 'x' || conv == 'X') ? 16u
                          : (conv == 'i') ? 0u : 10u;
            const int n = GatherInteger(s, buf, sizeof(buf), width, base, true);
            if (n == 0) return assigned ? assigned : (AtEnd(s) ? EOF : 0);
            const char *b = buf;
            bool neg = false;
            if (*b == '+') b++;
            else if (*b == '-') { neg = true; b++; }
            unsigned long long mag = 0;
            auto r = ::std::from_chars(b, buf + n, mag, static_cast<int>(base));
            if (r.ec == ::std::errc::result_out_of_range) {
                mag = ~0ull;
                neg = false;
            } else if (r.ec != ::std::errc{}) {
                return assigned;
            }
            if (!suppress) {
                void *dst = va_arg(ap, void *);
                if (conv == 'd' || conv == 'i')
                    StoreSigned(dst, length, neg ? -static_cast<long long>(mag)
                                                 : static_cast<long long>(mag));
                else
                    StoreUnsigned(dst, length, neg ? ~mag + 1ull : mag);
                assigned++;
            }
            break;
        }
        case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A': {
            SkipSpace(s);
            FloatSpecial special = FloatSpecial::None;
            const int n = GatherFloat(s, buf, sizeof(buf), width, special);
            if (n == 0) return assigned ? assigned : (AtEnd(s) ? EOF : 0);
            double d = 0;
            if (special != FloatSpecial::None) {
                d = (special == FloatSpecial::Inf) ? __builtin_inf() : __builtin_nan("");
                if (buf[0] == '-') d = -d;
                if (!suppress) {
                    void *dst = va_arg(ap, void *);
                    if (length == 'l')      *static_cast<double *>(dst) = d;
                    else if (length == 'L') *static_cast<long double *>(dst) = static_cast<long double>(d);
                    else                    *static_cast<float *>(dst) = static_cast<float>(d);
                    assigned++;
                }
                break;
            }
            auto r = ::std::from_chars(buf, buf + n, d);
            if (r.ec != ::std::errc{}) return assigned;
            if (!suppress) {
                void *dst = va_arg(ap, void *);
                if (length == 'l')      *static_cast<double *>(dst) = d;
                else if (length == 'L') *static_cast<long double *>(dst) = static_cast<long double>(d);
                else                    *static_cast<float *>(dst) = static_cast<float>(d);
                assigned++;
            }
            break;
        }
        case 'c': {
            const int count = width > 0 ? width : 1;
            char *dst = suppress ? nullptr : va_arg(ap, char *);
            int got = 0;
            for (; got < count; got++) {
                const int c = Get(s);
                if (c == kEnd) break;
                if (dst) dst[got] = static_cast<char>(c);
            }
            if (got < count) return assigned ? assigned : EOF;
            if (dst) assigned++;
            break;
        }
        case 's': {
            SkipSpace(s);
            char *dst = suppress ? nullptr : va_arg(ap, char *);
            int   got = 0;
            for (;;) {
                if (width > 0 && got >= width) break;
                const int c = Get(s);
                if (c == kEnd || IsSpace(c)) { Unget(s, c); break; }
                if (dst) dst[got] = static_cast<char>(c);
                got++;
            }
            if (got == 0) return assigned ? assigned : EOF;
            if (dst) { dst[got] = '\0'; assigned++; }
            break;
        }
        case '[': {
            bool member[256] = {};
            bool invert = false;
            if (*p == '^') { invert = true; p++; }
            if (*p == ']') { member[static_cast<unsigned char>(']')] = true; p++; }
            while (*p && *p != ']') {
                const unsigned char lo = static_cast<unsigned char>(*p);
                if (p[1] == '-' && p[2] && p[2] != ']') {
                    const unsigned char hi = static_cast<unsigned char>(p[2]);
                    for (int ch = lo; ch <= hi; ch++) member[ch] = true;
                    p += 3;
                } else {
                    member[lo] = true;
                    p++;
                }
            }
            if (*p == ']') p++;

            char *dst = suppress ? nullptr : va_arg(ap, char *);
            int   got = 0;
            for (;;) {
                if (width > 0 && got >= width) break;
                const int c = Get(s);
                if (c == kEnd) { Unget(s, c); break; }
                const bool in = member[static_cast<unsigned char>(c)];
                if (in == invert) { Unget(s, c); break; }
                if (dst) dst[got] = static_cast<char>(c);
                got++;
            }
            if (got == 0) return assigned ? assigned : (AtEnd(s) ? EOF : 0);
            if (dst) { dst[got] = '\0'; assigned++; }
            break;
        }
        case 'p': {
            SkipSpace(s);
            unsigned base = 16;
            const int n = GatherInteger(s, buf, sizeof(buf), width, base, false);
            if (n == 0) return assigned ? assigned : EOF;
            unsigned long long v = 0;
            auto r = ::std::from_chars(buf, buf + n, v, 16);
            if (r.ec != ::std::errc{}) return assigned;
            if (!suppress) {
                *static_cast<void **>(va_arg(ap, void **)) =
                    reinterpret_cast<void *>(static_cast<::std::uintptr_t>(v));
                assigned++;
            }
            break;
        }
        case 'n': {
            if (!suppress) StoreSigned(va_arg(ap, void *), length, s.consumed);
            break;
        }
        default:
            return assigned;
        }
    }
    return assigned;
}

}

namespace std {

int vfscanf(FILE *stream, const char *format, va_list arg) noexcept
{
    if (!stream) return EOF;
    Source s{stream, nullptr, 0, {}, 0, 0, false};
    const int r = Run(s, format, arg);
    if (s.npb > 0) ungetc(s.pb[s.npb - 1], stream);
    return r;
}

int vsscanf(const char *str, const char *format, va_list arg) noexcept
{
    if (!str) return EOF;
    Source s{nullptr, str, 0, {}, 0, 0, false};
    return Run(s, format, arg);
}

int vscanf(const char *format, va_list arg) noexcept
{
    return vfscanf(__stdin(), format, arg);
}

int fscanf(FILE *stream, const char *format, ...) noexcept
{
    va_list ap;
    va_start(ap, format);
    const int r = vfscanf(stream, format, ap);
    va_end(ap);
    return r;
}

int scanf(const char *format, ...) noexcept
{
    va_list ap;
    va_start(ap, format);
    const int r = vfscanf(__stdin(), format, ap);
    va_end(ap);
    return r;
}

int sscanf(const char *str, const char *format, ...) noexcept
{
    va_list ap;
    va_start(ap, format);
    const int r = vsscanf(str, format, ap);
    va_end(ap);
    return r;
}

}