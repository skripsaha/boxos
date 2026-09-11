
#include <cerrno>
#include <charconv>
#include <climits>
#include <cstdlib>
#include <__bits/c_utf8>
#include <cstring>

#include "box/sync.h"

extern "C" {
int  __cxa_atexit(void (*fn)(void *), void *arg, void *dso);
extern void *__dso_handle;
const char *__boxcxx_generic_text(int code) noexcept;
}

namespace {

struct QuickExitList {
    void (**Fns)() = nullptr;
    size_t  Count  = 0;
    size_t  Cap    = 0;
};

QuickExitList g_quick_exit;
uspin_t       g_quick_exit_lock = USPIN_INIT;

constexpr unsigned int kDefaultSeed = 1u;

thread_local unsigned long long g_rand_state = kDefaultSeed;

int Utf8Decode(const char *s, size_t n, char32_t *out)
{
    const auto d = ::std::__utf8::Decode(s, n);
    switch (d.status) {
    case ::std::__utf8::Status::Nul: if (out) *out = 0; return 0;
    case ::std::__utf8::Status::Ok:  if (out) *out = d.cp; return d.len;
    default:                         return -1;
    }
}

int Utf8Encode(char *s, char32_t cp) { return ::std::__utf8::Encode(s, cp); }

bool IsSpace(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' ||
           c == '\r';
}

int DigitValue(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'z') return c - 'a' + 10;
    if (c >= 'A' && c <= 'Z') return c - 'A' + 10;
    return -1;
}

struct IntParse {
    unsigned long long Value    = 0;
    bool               Negative = false;
    bool               Overflow = false;
    bool               Valid    = false;
    const char        *End      = nullptr;
};

IntParse ParseInteger(const char *nptr, int base, unsigned long long limit)
{
    IntParse out;
    const char *p = nptr;
    out.End = nptr;
    if (!p) return out;

    while (IsSpace(*p)) p++;

    if (*p == '+' || *p == '-') {
        out.Negative = (*p == '-');
        p++;
    }

    if (base == 16 || base == 0) {
        if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X') &&
            DigitValue(p[2]) >= 0 && DigitValue(p[2]) < 16) {
            p += 2;
            base = 16;
        } else if (base == 0) {
            base = (p[0] == '0') ? 8 : 10;
        }
    } else if (base == 0) {
        base = 10;
    }

    const unsigned long long ubase = static_cast<unsigned long long>(base);
    const unsigned long long cutoff = limit / ubase;
    const unsigned long long cutlim = limit % ubase;

    for (;; p++) {
        const int d = DigitValue(*p);
        if (d < 0 || d >= base) break;
        out.Valid = true;
        if (out.Value > cutoff ||
            (out.Value == cutoff && static_cast<unsigned long long>(d) > cutlim)) {
            out.Overflow = true;
        } else {
            out.Value = out.Value * ubase + static_cast<unsigned long long>(d);
        }
    }

    if (out.Valid) out.End = p;
    return out;
}

template <class T>
T StrToSigned(const char *nptr, char **endptr, int base, T lo, T hi)
{
    const unsigned long long limit =
        static_cast<unsigned long long>(hi) + 1;
    IntParse r = ParseInteger(nptr, base, limit);
    if (endptr) *endptr = const_cast<char *>(r.End);
    if (!r.Valid) return 0;

    if (r.Overflow) { errno = ERANGE; return r.Negative ? lo : hi; }
    if (r.Negative) {
        if (r.Value > static_cast<unsigned long long>(hi) + 1) {
            errno = ERANGE;
            return lo;
        }
        return static_cast<T>(0 - static_cast<T>(r.Value));
    }
    if (r.Value > static_cast<unsigned long long>(hi)) {
        errno = ERANGE;
        return hi;
    }
    return static_cast<T>(r.Value);
}

template <class T>
T StrToUnsigned(const char *nptr, char **endptr, int base, T hi)
{
    IntParse r = ParseInteger(nptr, base, hi);
    if (endptr) *endptr = const_cast<char *>(r.End);
    if (!r.Valid) return 0;
    if (r.Overflow) { errno = ERANGE; return hi; }
    return r.Negative ? static_cast<T>(0 - static_cast<T>(r.Value))
                      : static_cast<T>(r.Value);
}

template <class T>
T StrToFloat(const char *nptr, char **endptr)
{
    const char *p = nptr;
    if (endptr) *endptr = const_cast<char *>(nptr);
    if (!p) return T{};

    while (IsSpace(*p)) p++;

    bool neg = false;
    if (*p == '+' || *p == '-') {
        neg = (*p == '-');
        p++;
    }

    std::chars_format fmt = std::chars_format::general;
    const char       *mag = p;
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X') &&
        (DigitValue(p[2]) >= 0 || p[2] == '.')) {
        fmt = std::chars_format::hex;
        mag = p + 2;
    }

    const char *const end = mag + std::strlen(mag);

    T                      value{};
    std::from_chars_result r = std::from_chars(mag, end, value, fmt);
    if (r.ec == std::errc::invalid_argument) return T{};

    if (r.ec == std::errc::result_out_of_range) {
        errno = ERANGE;
        long double wide{};
        std::from_chars_result w = std::from_chars(mag, end, wide, fmt);
        T out{};
        if (w.ec == std::errc{}) {
            const long double mag_abs = wide < 0 ? -wide : wide;
            out = (mag_abs > static_cast<long double>(1))
                      ? __builtin_huge_vall()
                      : T{};
            if (r.ptr < w.ptr) r.ptr = w.ptr;
        } else {
            out = __builtin_huge_vall();
        }
        if (endptr) *endptr = const_cast<char *>(r.ptr);
        return neg ? static_cast<T>(-out) : out;
    }

    if (endptr) *endptr = const_cast<char *>(r.ptr);
    return neg ? static_cast<T>(-value) : value;
}

void SwapBytes(unsigned char *a, unsigned char *b, size_t n)
{
    while (n--) {
        const unsigned char t = *a;
        *a++ = *b;
        *b++ = t;
    }
}

using Cmp = int (*)(const void *, const void *);

void InsertionSort(unsigned char *base, size_t n, size_t size, Cmp cmp)
{
    for (size_t i = 1; i < n; i++)
        for (size_t j = i; j > 0 && cmp(base + (j - 1) * size, base + j * size) > 0;
             j--)
            SwapBytes(base + (j - 1) * size, base + j * size, size);
}

void SiftDown(unsigned char *base, size_t root, size_t n, size_t size, Cmp cmp)
{
    for (;;) {
        size_t child = 2 * root + 1;
        if (child >= n) return;
        if (child + 1 < n &&
            cmp(base + child * size, base + (child + 1) * size) < 0)
            child++;
        if (cmp(base + root * size, base + child * size) >= 0) return;
        SwapBytes(base + root * size, base + child * size, size);
        root = child;
    }
}

void HeapSort(unsigned char *base, size_t n, size_t size, Cmp cmp)
{
    for (size_t i = n / 2; i > 0; i--) SiftDown(base, i - 1, n, size, cmp);
    for (size_t i = n; i > 1; i--) {
        SwapBytes(base, base + (i - 1) * size, size);
        SiftDown(base, 0, i - 1, size, cmp);
    }
}

void IntroSort(unsigned char *base, size_t n, size_t size, Cmp cmp, int depth)
{
    while (n > 16) {
        if (depth == 0) { HeapSort(base, n, size, cmp); return; }
        depth--;

        unsigned char *lo  = base;
        unsigned char *mid = base + (n / 2) * size;
        unsigned char *hi  = base + (n - 1) * size;
        if (cmp(mid, lo) < 0) SwapBytes(mid, lo, size);
        if (cmp(hi, lo) < 0) SwapBytes(hi, lo, size);
        if (cmp(hi, mid) < 0) SwapBytes(hi, mid, size);
        SwapBytes(base, mid, size);

        size_t i = 0;
        size_t j = n;
        for (;;) {
            do { i++; } while (i < n && cmp(base + i * size, base) < 0);
            do { j--; } while (j > 0 && cmp(base + j * size, base) > 0);
            if (i >= j) break;
            SwapBytes(base + i * size, base + j * size, size);
        }
        SwapBytes(base, base + j * size, size);

        if (j < n - j - 1) {
            IntroSort(base, j, size, cmp, depth);
            base += (j + 1) * size;
            n -= j + 1;
        } else {
            IntroSort(base + (j + 1) * size, n - j - 1, size, cmp, depth);
            n = j;
        }
    }
    InsertionSort(base, n, size, cmp);
}

int Log2Floor(size_t n)
{
    int r = 0;
    while (n > 1) { n >>= 1; r++; }
    return r;
}

}

namespace std {

int atexit(void (*func)()) noexcept
{
    if (!func) return -1;
    return __cxa_atexit(reinterpret_cast<void (*)(void *)>(func), nullptr,
                        &__dso_handle);
}

int at_quick_exit(void (*func)()) noexcept
{
    if (!func) return -1;

    uspin_lock(&g_quick_exit_lock);
    if (g_quick_exit.Count == g_quick_exit.Cap) {
        const size_t next = g_quick_exit.Cap ? g_quick_exit.Cap * 2 : 32;
        void *grown = ::realloc(g_quick_exit.Fns, next * sizeof(void (*)()));
        if (!grown) {
            uspin_unlock(&g_quick_exit_lock);
            return -1;
        }
        g_quick_exit.Fns = static_cast<void (**)()>(grown);
        g_quick_exit.Cap = next;
    }
    g_quick_exit.Fns[g_quick_exit.Count++] = func;
    uspin_unlock(&g_quick_exit_lock);
    return 0;
}

[[noreturn]] void quick_exit(int status) noexcept
{
    for (;;) {
        uspin_lock(&g_quick_exit_lock);
        if (g_quick_exit.Count == 0) {
            uspin_unlock(&g_quick_exit_lock);
            break;
        }
        void (*fn)() = g_quick_exit.Fns[--g_quick_exit.Count];
        uspin_unlock(&g_quick_exit_lock);
        fn();
    }
    ::_Exit(status);
}

char *getenv(const char *) noexcept
{
    return nullptr;
}

int system(const char *string)
{
    if (!string) return 0;
    return -1;
}

long strtol(const char *nptr, char **endptr, int base) noexcept
{
    return StrToSigned<long>(nptr, endptr, base, LONG_MIN, LONG_MAX);
}

long long strtoll(const char *nptr, char **endptr, int base) noexcept
{
    return StrToSigned<long long>(nptr, endptr, base, LLONG_MIN, LLONG_MAX);
}

unsigned long strtoul(const char *nptr, char **endptr, int base) noexcept
{
    return StrToUnsigned<unsigned long>(nptr, endptr, base, ULONG_MAX);
}

unsigned long long strtoull(const char *nptr, char **endptr, int base) noexcept
{
    return StrToUnsigned<unsigned long long>(nptr, endptr, base, ULLONG_MAX);
}

double strtod(const char *nptr, char **endptr) noexcept
{
    return StrToFloat<double>(nptr, endptr);
}

float strtof(const char *nptr, char **endptr) noexcept
{
    return StrToFloat<float>(nptr, endptr);
}

long double strtold(const char *nptr, char **endptr) noexcept
{
    return StrToFloat<long double>(nptr, endptr);
}

int atoi(const char *nptr) noexcept
{
    return static_cast<int>(StrToSigned<long>(nptr, nullptr, 10, INT_MIN, INT_MAX));
}
long atol(const char *nptr) noexcept { return strtol(nptr, nullptr, 10); }
long long atoll(const char *nptr) noexcept { return strtoll(nptr, nullptr, 10); }
double atof(const char *nptr) noexcept { return strtod(nptr, nullptr); }

int mblen(const char *s, size_t n) noexcept
{
    if (!s) return 0;
    return Utf8Decode(s, n, nullptr);
}

int mbtowc(wchar_t *pwc, const char *s, size_t n) noexcept
{
    if (!s) return 0;
    char32_t cp  = 0;
    const int len = Utf8Decode(s, n, &cp);
    if (len > 0 && pwc) *pwc = static_cast<wchar_t>(cp);
    if (len == 0 && pwc) *pwc = 0;
    return len;
}

int wctomb(char *s, wchar_t wc) noexcept
{
    if (!s) return 0;
    return Utf8Encode(s, static_cast<char32_t>(wc));
}

size_t mbstowcs(wchar_t *pwcs, const char *s, size_t n) noexcept
{
    if (!s) return static_cast<size_t>(-1);
    size_t written = 0;
    while (!pwcs || written < n) {
        char32_t  cp  = 0;
        const int len = Utf8Decode(s, 4, &cp);
        if (len < 0) return static_cast<size_t>(-1);
        if (len == 0) {
            if (pwcs && written < n) pwcs[written] = 0;
            return written;
        }
        if (pwcs) pwcs[written] = static_cast<wchar_t>(cp);
        written++;
        s += len;
    }
    return written;
}

size_t wcstombs(char *s, const wchar_t *pwcs, size_t n) noexcept
{
    if (!pwcs) return static_cast<size_t>(-1);
    size_t written = 0;
    for (;;) {
        const char32_t cp = static_cast<char32_t>(*pwcs);
        char           buf[4];
        const int      len = Utf8Encode(buf, cp);
        if (len < 0) return static_cast<size_t>(-1);
        if (cp == 0) {
            if (s && written < n) s[written] = '\0';
            return written;
        }
        if (s) {
            if (written + static_cast<size_t>(len) > n) return written;
            for (int i = 0; i < len; i++) s[written + i] = buf[i];
        }
        written += static_cast<size_t>(len);
        pwcs++;
    }
}

const void *bsearch(const void *key, const void *base, size_t nmemb,
                    size_t size, __boxcxx_compare *compar)
{
    if (!key || !base || !compar || size == 0) return nullptr;
    const auto *p = static_cast<const unsigned char *>(base);
    while (nmemb > 0) {
        const size_t half = nmemb / 2;
        const unsigned char *mid = p + half * size;
        const int c = compar(key, mid);
        if (c == 0) return mid;
        if (c > 0) {
            p = mid + size;
            nmemb -= half + 1;
        } else {
            nmemb = half;
        }
    }
    return nullptr;
}

void *bsearch(const void *key, void *base, size_t nmemb, size_t size,
              __boxcxx_compare *compar)
{
    return const_cast<void *>(
        bsearch(key, static_cast<const void *>(base), nmemb, size, compar));
}

void qsort(void *base, size_t nmemb, size_t size, __boxcxx_compare *compar)
{
    if (!base || !compar || size == 0 || nmemb < 2) return;
    IntroSort(static_cast<unsigned char *>(base), nmemb, size, compar,
              2 * Log2Floor(nmemb));
}

int rand() noexcept
{
    g_rand_state = g_rand_state * 6364136223846793005ULL + 1442695040888963407ULL;
    return static_cast<int>((g_rand_state >> 33) & 0x7FFFFFFFULL);
}

void srand(unsigned int seed) noexcept { g_rand_state = seed; }

}

namespace std {

char *strerror(int errnum) noexcept
{
    static thread_local char buf[48];

    if (const char *text = __boxcxx_generic_text(errnum)) {
        size_t i = 0;
        for (; text[i] && i + 1 < sizeof buf; i++) buf[i] = text[i];
        buf[i] = '\0';
        return buf;
    }

    const char prefix[] = "generic error ";
    size_t     i        = 0;
    for (; prefix[i]; i++) buf[i] = prefix[i];

    unsigned v = errnum < 0 ? static_cast<unsigned>(-errnum)
                            : static_cast<unsigned>(errnum);
    if (errnum < 0) buf[i++] = '-';
    char digits[12];
    int  d = 0;
    do { digits[d++] = static_cast<char>('0' + v % 10); v /= 10; } while (v);
    while (d > 0 && i + 1 < sizeof buf) buf[i++] = digits[--d];
    buf[i] = '\0';
    return buf;
}

}