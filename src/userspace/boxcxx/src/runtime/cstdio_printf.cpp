// boxcxx — <cstdio> runtime: the printf family
//
// One engine, two sinks. A stream sink hands bytes to the byte layer; a memory
// sink fills a caller's buffer and keeps counting past the end, because that
// count IS snprintf's return value and truncating it would lose the number the
// caller needs to allocate correctly.
//
// The floating conversions do NOT carry their own digit generation. <charconv>
// already produces correctly-rounded output in fixed, scientific, general and
// hex forms (Ф9A, MPFR-verified), and printf's job here is the part around it:
// the sign, the flags, the width and the padding. A second digit generator
// would be a second thing to be right, and this one has been checked against
// an oracle in a way a fresh one would not be.
//
// boxlib's printf, which owns the global name, converts %s %d %i %u %x %X %c
// %p and %% and nothing else. This engine is the full C set. The two are
// different functions reached by different spellings — see the note in
// <cstdio> about why the global name stays boxlib's.

#include <charconv>
#include <cstdarg>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>

namespace {

// ── the sink ────────────────────────────────────────────────────────────
struct Sink {
    ::std::FILE   *stream;  // one of these two is null
    char          *mem;
    ::std::size_t  cap;     // mem: bytes available INCLUDING the terminator
    ::std::size_t  len;     // bytes the result WOULD have; may exceed cap
    bool           bad;
};

void Emit(Sink &s, const char *p, ::std::size_t n)
{
    if (s.bad || n == 0) return;
    if (s.stream) {
        if (::std::fwrite(p, 1, n, s.stream) != n) { s.bad = true; return; }
    } else if (s.mem && s.cap > 0) {
        const ::std::size_t room = (s.len < s.cap - 1) ? (s.cap - 1 - s.len) : 0;
        const ::std::size_t take = n < room ? n : room;
        if (take) ::std::memcpy(s.mem + s.len, p, take);
    }
    s.len += n;
}

void EmitRepeat(Sink &s, char c, int n)
{
    char block[64];
    while (n > 0) {
        const int chunk = n < 64 ? n : 64;
        ::std::memset(block, c, static_cast<::std::size_t>(chunk));
        Emit(s, block, static_cast<::std::size_t>(chunk));
        n -= chunk;
    }
}

// ── one conversion's parsed shape ───────────────────────────────────────
struct Spec {
    bool left = false;      // '-'
    bool plus = false;      // '+'
    bool space = false;     // ' '
    bool alt = false;       // '#'
    bool zero = false;      // '0'
    int  width = 0;
    int  prec = -1;         // -1 = absent
    char length = 0;        // 'H'=hh 'h' 'l' 'q'=ll 'j' 'z' 't' 'L'
    char conv = 0;
};

// Writes body with sign/prefix, honouring width, zero-padding and adjustment.
// One place, because getting padding right once is the difference between this
// and a family of near-identical bugs.
void Pad(Sink &s, const Spec &sp, const char *sign, const char *prefix,
         const char *body, int bodyn, int zeros)
{
    const int signn   = sign ? static_cast<int>(::std::strlen(sign)) : 0;
    const int prefixn = prefix ? static_cast<int>(::std::strlen(prefix)) : 0;
    const int total   = signn + prefixn + zeros + bodyn;
    const int pad     = sp.width > total ? sp.width - total : 0;

    // '0' is ignored when '-' is given or when a precision was specified for an
    // integer conversion — both are C's rules, and both are the kind of thing a
    // reader expects to see stated rather than inferred from the code.
    const bool zeropad = sp.zero && !sp.left;

    if (!sp.left && !zeropad) EmitRepeat(s, ' ', pad);
    if (sign) Emit(s, sign, static_cast<::std::size_t>(signn));
    if (prefix) Emit(s, prefix, static_cast<::std::size_t>(prefixn));
    if (zeropad) EmitRepeat(s, '0', pad);
    EmitRepeat(s, '0', zeros);
    Emit(s, body, static_cast<::std::size_t>(bodyn));
    if (sp.left) EmitRepeat(s, ' ', pad);
}

const char *kLowerDigits = "0123456789abcdef";
const char *kUpperDigits = "0123456789ABCDEF";

// Unsigned magnitude in the given base, written backwards into buf.
int Digits(char *buf, unsigned long long v, unsigned base, bool upper)
{
    const char *tab = upper ? kUpperDigits : kLowerDigits;
    int n = 0;
    do { buf[n++] = tab[v % base]; v /= base; } while (v);
    for (int i = 0; i < n / 2; i++) {
        const char t = buf[i];
        buf[i] = buf[n - 1 - i];
        buf[n - 1 - i] = t;
    }
    return n;
}

// `is_signed` is not the same question as `negative`: '+' and ' ' apply to a
// SIGNED conversion whatever its value, and to an unsigned one never. Passing
// only `negative` made %+u print a plus, which the host disagreed with on 6554
// of 35817 pairs — the first thing the differential sweep found.
void Integer(Sink &s, const Spec &sp, bool is_signed, bool negative,
             unsigned long long mag, unsigned base, bool upper)
{
    char body[72];
    int  n = Digits(body, mag, base, upper);

    // A precision of 0 and a value of 0 produce NO digits at all. Every other
    // conversion has something to show; this one deliberately does not, and it
    // is the case an implementation forgets.
    if (sp.prec == 0 && mag == 0) n = 0;

    int zeros = (sp.prec > n) ? sp.prec - n : 0;

    const char *sign = nullptr;
    if (negative)                   sign = "-";
    else if (is_signed && sp.plus)  sign = "+";
    else if (is_signed && sp.space) sign = " ";

    const char *prefix = nullptr;
    if (sp.alt && base == 16 && mag != 0) prefix = upper ? "0X" : "0x";
    // '#' on octal is not a prefix: C says it INCREASES THE PRECISION if
    // necessary to force a leading zero. So it adds nothing when the precision
    // has already produced one — adding unconditionally made %#.12o one digit
    // too wide, which the host caught.
    if (sp.alt && base == 8 && zeros == 0 && (n == 0 || body[0] != '0')) zeros = 1;

    Spec eff = sp;
    if (sp.prec >= 0) eff.zero = false;   // C: precision defeats '0'
    Pad(s, eff, sign, prefix, body, n, zeros);
}

// ── floating point ──────────────────────────────────────────────────────

void Floating(Sink &s, const Spec &sp, long double value, char conv)
{
    const bool upper = (conv == 'F' || conv == 'E' || conv == 'G' || conv == 'A');
    const char low   = static_cast<char>(upper ? conv - 'A' + 'a' : conv);

    bool neg = false;
    // A negative zero must print its sign, so the test cannot be `value < 0`.
    if (value < 0 || (value == 0 && __builtin_signbit(static_cast<double>(value)))) {
        neg = true;
        value = -value;
    }

    const char *sign = neg ? "-" : (sp.plus ? "+" : (sp.space ? " " : nullptr));

    // Infinities and NaNs have no digits to generate and no precision to honour,
    // and '0' padding is forbidden for them.
    if (__builtin_isinf(static_cast<double>(value)) || value != value) {
        const bool isnan = (value != value);
        const char *txt = isnan ? (upper ? "NAN" : "nan") : (upper ? "INF" : "inf");
        Spec eff = sp;
        eff.zero = false;
        Pad(s, eff, sign, nullptr, txt, 3, 0);
        return;
    }

    int prec = sp.prec;
    if (prec < 0) prec = 6;
    if (low == 'g' && prec == 0) prec = 1;

    ::std::chars_format fmt = ::std::chars_format::fixed;
    switch (low) {
    case 'f': fmt = ::std::chars_format::fixed; break;
    case 'e': fmt = ::std::chars_format::scientific; break;
    case 'g': fmt = ::std::chars_format::general; break;
    case 'a': fmt = ::std::chars_format::hex; break;
    default: break;
    }

    // %#g keeps the trailing zeros %g drops, and chars_format::general drops
    // them by definition — so '#' cannot be a post-processing step here, it has
    // to change WHICH form is generated. C's rule is stated in terms of the
    // exponent X the value would have in %e form: use %f style with precision
    // P-1-X when -4 <= X < P, and %e style with precision P-1 otherwise. Ask
    // for the exponent first, then render the chosen form at full width.
    int alt_g_prec = -1;
    if (low == 'g' && sp.alt) {
        char probe[64];
        auto pr = ::std::to_chars(probe, probe + sizeof(probe),
                                  static_cast<double>(value),
                                  ::std::chars_format::scientific, prec - 1);
        if (pr.ec != ::std::errc{}) { s.bad = true; return; }
        int exp10 = 0;
        for (char *q = probe; q < pr.ptr; q++)
            if (*q == 'e') { exp10 = ::std::atoi(q + 1); break; }
        if (exp10 >= -4 && exp10 < prec) {
            fmt = ::std::chars_format::fixed;
            alt_g_prec = prec - 1 - exp10;
            if (alt_g_prec < 0) alt_g_prec = 0;
        } else {
            fmt = ::std::chars_format::scientific;
            alt_g_prec = prec - 1;
        }
    }

    // Generous: %f of DBL_MAX with a large precision is long, and a short
    // buffer here would be a truncation nobody asked for.
    char  body[1200];
    char *first = body;
    char *last  = body + sizeof(body);
    const int use_prec = (alt_g_prec >= 0) ? alt_g_prec : prec;
    auto  r = (low == 'a')
                  ? ::std::to_chars(first, last, static_cast<double>(value), fmt)
                  : ::std::to_chars(first, last, static_cast<double>(value), fmt, use_prec);
    if (r.ec != ::std::errc{}) { s.bad = true; return; }
    int n = static_cast<int>(r.ptr - body);

    if (upper)
        for (int i = 0; i < n; i++)
            if (body[i] >= 'a' && body[i] <= 'z') body[i] = static_cast<char>(body[i] - 'a' + 'A');

    // '#' keeps the point that %g would have dropped, and gives %f/%e one when
    // the precision is zero.
    if (sp.alt) {
        bool has_point = false;
        for (int i = 0; i < n; i++) if (body[i] == '.') { has_point = true; break; }
        if (!has_point) {
            // Insert before the exponent, if there is one.
            int at = n;
            for (int i = 0; i < n; i++)
                if (body[i] == 'e' || body[i] == 'E' || body[i] == 'p' || body[i] == 'P') { at = i; break; }
            ::std::memmove(body + at + 1, body + at, static_cast<::std::size_t>(n - at));
            body[at] = '.';
            n++;
        }
    }

    const char *prefix = (low == 'a') ? (upper ? "0X" : "0x") : nullptr;
    // to_chars' hex form has no 0x prefix; C's %a does.
    Pad(s, sp, sign, prefix, body, n, 0);
}

// ── the walk ────────────────────────────────────────────────────────────

int ReadInt(const char *&p)
{
    int v = 0;
    while (*p >= '0' && *p <= '9') v = v * 10 + (*p++ - '0');
    return v;
}

int Run(Sink &s, const char *fmt, va_list ap)
{
    if (!fmt) { s.bad = true; return -1; }

    for (const char *p = fmt; *p;) {
        if (*p != '%') {
            const char *run = p;
            while (*p && *p != '%') p++;
            Emit(s, run, static_cast<::std::size_t>(p - run));
            continue;
        }
        p++;                                   // past '%'
        if (*p == '%') { Emit(s, "%", 1); p++; continue; }

        Spec sp;
        for (;; p++) {
            if (*p == '-') sp.left = true;
            else if (*p == '+') sp.plus = true;
            else if (*p == ' ') sp.space = true;
            else if (*p == '#') sp.alt = true;
            else if (*p == '0') sp.zero = true;
            else break;
        }
        if (*p == '*') {
            sp.width = va_arg(ap, int);
            if (sp.width < 0) { sp.left = true; sp.width = -sp.width; }
            p++;
        } else {
            sp.width = ReadInt(p);
        }
        if (*p == '.') {
            p++;
            if (*p == '*') { sp.prec = va_arg(ap, int); p++; }
            else           { sp.prec = ReadInt(p); }
            if (sp.prec < 0) sp.prec = -1;     // C: a negative * precision is absent
        }
        // Length modifiers, folded to one character each so the switch below
        // has one case per width rather than one per spelling.
        if (p[0] == 'h' && p[1] == 'h') { sp.length = 'H'; p += 2; }
        else if (p[0] == 'l' && p[1] == 'l') { sp.length = 'q'; p += 2; }
        else if (*p == 'h' || *p == 'l' || *p == 'j' || *p == 'z' || *p == 't' || *p == 'L') {
            sp.length = *p++;
        }
        sp.conv = *p;
        if (!sp.conv) break;
        p++;

        switch (sp.conv) {
        case 'd': case 'i': {
            long long v;
            switch (sp.length) {
            case 'H': v = static_cast<signed char>(va_arg(ap, int)); break;
            case 'h': v = static_cast<short>(va_arg(ap, int)); break;
            case 'l': v = va_arg(ap, long); break;
            case 'q': v = va_arg(ap, long long); break;
            case 'j': v = va_arg(ap, ::std::intmax_t); break;
            case 'z': v = static_cast<long long>(va_arg(ap, ::std::size_t)); break;
            case 't': v = static_cast<long long>(va_arg(ap, ::std::ptrdiff_t)); break;
            default:  v = va_arg(ap, int); break;
            }
            const bool neg = v < 0;
            // -LLONG_MIN does not exist; negating in the unsigned domain does.
            const unsigned long long mag =
                neg ? (~static_cast<unsigned long long>(v) + 1ull)
                    : static_cast<unsigned long long>(v);
            Integer(s, sp, true, neg, mag, 10, false);
            break;
        }
        case 'o': case 'u': case 'x': case 'X': {
            unsigned long long v;
            switch (sp.length) {
            case 'H': v = static_cast<unsigned char>(va_arg(ap, unsigned int)); break;
            case 'h': v = static_cast<unsigned short>(va_arg(ap, unsigned int)); break;
            case 'l': v = va_arg(ap, unsigned long); break;
            case 'q': v = va_arg(ap, unsigned long long); break;
            case 'j': v = va_arg(ap, ::std::uintmax_t); break;
            case 'z': v = va_arg(ap, ::std::size_t); break;
            case 't': v = static_cast<unsigned long long>(va_arg(ap, ::std::ptrdiff_t)); break;
            default:  v = va_arg(ap, unsigned int); break;
            }
            const unsigned base = (sp.conv == 'o') ? 8u : (sp.conv == 'u' ? 10u : 16u);
            Integer(s, sp, false, false, v, base, sp.conv == 'X');
            break;
        }
        case 'f': case 'F': case 'e': case 'E':
        case 'g': case 'G': case 'a': case 'A': {
            const long double v = (sp.length == 'L') ? va_arg(ap, long double)
                                                     : static_cast<long double>(va_arg(ap, double));
            Floating(s, sp, v, sp.conv);
            break;
        }
        case 'c': {
            const char ch = static_cast<char>(va_arg(ap, int));
            Spec eff = sp;
            eff.zero = false;
            Pad(s, eff, nullptr, nullptr, &ch, 1, 0);
            break;
        }
        case 's': {
            const char *str = va_arg(ap, const char *);
            if (!str) str = "(null)";
            int n = 0;
            // A precision on %s bounds how far the argument is READ, not merely
            // how much is printed: the array need not be terminated.
            if (sp.prec >= 0) { while (n < sp.prec && str[n]) n++; }
            else              { n = static_cast<int>(::std::strlen(str)); }
            Spec eff = sp;
            eff.zero = false;
            Pad(s, eff, nullptr, nullptr, str, n, 0);
            break;
        }
        case 'p': {
            const void *v = va_arg(ap, void *);
            if (!v) {
                Spec eff = sp;
                eff.zero = false;
                Pad(s, eff, nullptr, nullptr, "(nil)", 5, 0);
            } else {
                Spec eff = sp;
                eff.alt = true;
                Integer(s, eff, false, false, reinterpret_cast<unsigned long long>(v), 16, false);
            }
            break;
        }
        case 'n': {
            // The one conversion that writes through its argument. C++ keeps it;
            // it is the caller's business whether that is wise.
            void *dst = va_arg(ap, void *);
            const long long written = static_cast<long long>(s.len);
            switch (sp.length) {
            case 'H': *static_cast<signed char *>(dst) = static_cast<signed char>(written); break;
            case 'h': *static_cast<short *>(dst) = static_cast<short>(written); break;
            case 'l': *static_cast<long *>(dst) = static_cast<long>(written); break;
            case 'q': *static_cast<long long *>(dst) = written; break;
            case 'j': *static_cast<::std::intmax_t *>(dst) = written; break;
            case 'z': *static_cast<::std::size_t *>(dst) = static_cast<::std::size_t>(written); break;
            case 't': *static_cast<::std::ptrdiff_t *>(dst) = static_cast<::std::ptrdiff_t>(written); break;
            default:  *static_cast<int *>(dst) = static_cast<int>(written); break;
            }
            break;
        }
        default:
            // An unknown conversion is undefined in C. Printing it back is the
            // version of undefined that tells the author what they wrote.
            Emit(s, "%", 1);
            Emit(s, &sp.conv, 1);
            break;
        }
    }

    if (s.mem && s.cap > 0) {
        const ::std::size_t at = (s.len < s.cap - 1) ? s.len : s.cap - 1;
        s.mem[at] = '\0';
    }
    return s.bad ? -1 : static_cast<int>(s.len);
}

} // namespace

namespace std {

int vfprintf(FILE *stream, const char *format, va_list arg) noexcept
{
    if (!stream) return -1;
    Sink s{stream, nullptr, 0, 0, false};
    return Run(s, format, arg);
}

int vsnprintf(char *str, size_t n, const char *format, va_list arg) noexcept
{
    Sink s{nullptr, str, n, 0, false};
    return Run(s, format, arg);
}

int vsprintf(char *str, const char *format, va_list arg) noexcept
{
    // No bound, which is what sprintf means and why it is the one to avoid.
    Sink s{nullptr, str, static_cast<size_t>(-1), 0, false};
    return Run(s, format, arg);
}

int vprintf(const char *format, va_list arg) noexcept
{
    return vfprintf(__stdout(), format, arg);
}

int fprintf(FILE *stream, const char *format, ...) noexcept
{
    va_list ap;
    va_start(ap, format);
    const int r = vfprintf(stream, format, ap);
    va_end(ap);
    return r;
}

int snprintf(char *str, size_t n, const char *format, ...) noexcept
{
    va_list ap;
    va_start(ap, format);
    const int r = vsnprintf(str, n, format, ap);
    va_end(ap);
    return r;
}

int sprintf(char *str, const char *format, ...) noexcept
{
    va_list ap;
    va_start(ap, format);
    const int r = vsprintf(str, format, ap);
    va_end(ap);
    return r;
}

} // namespace std

// The strong definition that overrides boxlib's weak one. It is not in
// namespace std because it IS ::printf — the same function the whole tree has
// always called, now converting everything C asks for. A C++ program links
// this; a C-only program, which cannot link boxcxx, keeps boxlib's.
extern "C" int printf(const char *format, ...)
{
    va_list ap;
    va_start(ap, format);
    const int r = ::std::vfprintf(::std::__stdout(), format, ap);
    va_end(ap);
    return r;
}
