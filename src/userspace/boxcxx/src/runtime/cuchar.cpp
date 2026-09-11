
#include <cerrno>
#include <cuchar>
#include <__bits/c_utf8>

namespace {

using ::std::mbstate_t;

thread_local mbstate_t g_mbrtoc8;
thread_local mbstate_t g_mbrtoc16;
thread_local mbstate_t g_mbrtoc32;
thread_local mbstate_t g_c8rtomb;
thread_local mbstate_t g_c16rtomb;
thread_local mbstate_t g_c32rtomb;

constexpr ::std::size_t kIncomplete = static_cast<::std::size_t>(-2);
constexpr ::std::size_t kInvalid    = static_cast<::std::size_t>(-1);
constexpr ::std::size_t kPending    = static_cast<::std::size_t>(-3);

void Reset(mbstate_t &st) noexcept { st = mbstate_t{}; }

unsigned short TakePending(mbstate_t &st) noexcept
{
    const unsigned short v = st.pend[0];
    st.npend--;
    for (unsigned i = 0; i < st.npend; i++) st.pend[i] = st.pend[i + 1];
    return v;
}

::std::__utf8::Decoded Assemble(mbstate_t &st, const char *s, ::std::size_t n,
                                unsigned char *buf, unsigned &taken) noexcept
{
    unsigned have = st.nin;
    for (unsigned i = 0; i < have; i++) buf[i] = st.in[i];

    const unsigned room = 4u - have;
    taken = static_cast<unsigned>(n < room ? n : room);
    for (unsigned i = 0; i < taken; i++)
        buf[have + i] = static_cast<unsigned char>(s[i]);

    return ::std::__utf8::Decode(reinterpret_cast<const char *>(buf),
                                 have + taken);
}

::std::__utf8::Decoded Step(mbstate_t &st, const char *&s, ::std::size_t &n,
                            unsigned char *buf, ::std::size_t &consumed) noexcept
{
    unsigned taken = 0;
    const auto d   = Assemble(st, s, n, buf, taken);

    if (d.status == ::std::__utf8::Status::Ok) {
        consumed = d.len - st.nin;
    } else if (d.status == ::std::__utf8::Status::Incomplete) {
        for (unsigned i = 0; i < taken; i++) st.in[st.nin + i] = buf[st.nin + i];
        st.nin = static_cast<unsigned char>(st.nin + taken);
    }
    return d;
}

}

namespace std {

size_t mbrtoc8(char8_t *pc8, const char *s, size_t n, mbstate_t *ps) noexcept
{
    mbstate_t &st = ps ? *ps : g_mbrtoc8;

    if (st.npend) {
        if (pc8) *pc8 = static_cast<char8_t>(TakePending(st));
        return kPending;
    }

    if (!s) { s = ""; n = 1; pc8 = nullptr; }

    unsigned char buf[4] = {};
    size_t        consumed = 0;
    const auto    d = Step(st, s, n, buf, consumed);

    switch (d.status) {
    case __utf8::Status::Nul:
        if (pc8) *pc8 = u8'\0';
        Reset(st);
        return 0;
    case __utf8::Status::Ok:
        if (pc8) *pc8 = static_cast<char8_t>(buf[0]);
        st.npend = 0;
        for (unsigned i = 1; i < d.len; i++) st.pend[st.npend++] = buf[i];
        st.nin = 0;
        return consumed;
    case __utf8::Status::Incomplete:
        return kIncomplete;
    case __utf8::Status::Invalid:
    default:
        errno = EILSEQ;
        return kInvalid;
    }
}

size_t mbrtoc16(char16_t *pc16, const char *s, size_t n, mbstate_t *ps) noexcept
{
    mbstate_t &st = ps ? *ps : g_mbrtoc16;

    if (st.npend) {
        if (pc16) *pc16 = static_cast<char16_t>(TakePending(st));
        return kPending;
    }

    if (!s) { s = ""; n = 1; pc16 = nullptr; }

    unsigned char buf[4] = {};
    size_t        consumed = 0;
    const auto    d = Step(st, s, n, buf, consumed);

    switch (d.status) {
    case __utf8::Status::Nul:
        if (pc16) *pc16 = u'\0';
        Reset(st);
        return 0;
    case __utf8::Status::Ok: {
        st.nin = 0;
        if (d.cp < 0x10000u) {
            if (pc16) *pc16 = static_cast<char16_t>(d.cp);
            return consumed;
        }
        const char32_t v = d.cp - 0x10000u;
        if (pc16) *pc16 = static_cast<char16_t>(0xD800u + (v >> 10));
        st.npend  = 1;
        st.pend[0] = static_cast<unsigned short>(0xDC00u + (v & 0x3FFu));
        return consumed;
    }
    case __utf8::Status::Incomplete:
        return kIncomplete;
    case __utf8::Status::Invalid:
    default:
        errno = EILSEQ;
        return kInvalid;
    }
}

size_t mbrtoc32(char32_t *pc32, const char *s, size_t n, mbstate_t *ps) noexcept
{
    mbstate_t &st = ps ? *ps : g_mbrtoc32;

    if (!s) { s = ""; n = 1; pc32 = nullptr; }

    unsigned char buf[4] = {};
    size_t        consumed = 0;
    const auto    d = Step(st, s, n, buf, consumed);

    switch (d.status) {
    case __utf8::Status::Nul:
        if (pc32) *pc32 = U'\0';
        Reset(st);
        return 0;
    case __utf8::Status::Ok:
        if (pc32) *pc32 = d.cp;
        st.nin = 0;
        return consumed;
    case __utf8::Status::Incomplete:
        return kIncomplete;
    case __utf8::Status::Invalid:
    default:
        errno = EILSEQ;
        return kInvalid;
    }
}

size_t c8rtomb(char *s, char8_t c8, mbstate_t *ps) noexcept
{
    mbstate_t &st = ps ? *ps : g_c8rtomb;

    char scratch[4];
    if (!s) { s = scratch; c8 = u8'\0'; Reset(st); }

    if (st.nin >= 4u) {
        Reset(st);
        errno = EILSEQ;
        return kInvalid;
    }
    st.in[st.nin++] = static_cast<unsigned char>(c8);

    const auto d = __utf8::Decode(reinterpret_cast<const char *>(st.in), st.nin);
    switch (d.status) {
    case __utf8::Status::Nul:
        s[0] = '\0';
        Reset(st);
        return 1;
    case __utf8::Status::Ok:
        for (unsigned i = 0; i < d.len; i++) s[i] = static_cast<char>(st.in[i]);
        Reset(st);
        return d.len;
    case __utf8::Status::Incomplete:
        return 0;
    case __utf8::Status::Invalid:
    default:
        Reset(st);
        errno = EILSEQ;
        return kInvalid;
    }
}

size_t c16rtomb(char *s, char16_t c16, mbstate_t *ps) noexcept
{
    mbstate_t &st = ps ? *ps : g_c16rtomb;

    char scratch[4];
    if (!s) { s = scratch; c16 = u'\0'; Reset(st); }

    const bool high = (c16 >= 0xD800u && c16 <= 0xDBFFu);
    const bool low  = (c16 >= 0xDC00u && c16 <= 0xDFFFu);

    if (st.npend) {
        if (!low) { Reset(st); errno = EILSEQ; return kInvalid; }
        const char32_t cp = 0x10000u +
                            ((static_cast<char32_t>(st.pend[0]) - 0xD800u) << 10) +
                            (static_cast<char32_t>(c16) - 0xDC00u);
        Reset(st);
        const int r = __utf8::Encode(s, cp);
        if (r < 0) { errno = EILSEQ; return kInvalid; }
        return static_cast<size_t>(r);
    }

    if (high) {
        st.npend   = 1;
        st.pend[0] = static_cast<unsigned short>(c16);
        return 0;
    }
    if (low) { errno = EILSEQ; return kInvalid; }

    const int r = __utf8::Encode(s, static_cast<char32_t>(c16));
    if (r < 0) { errno = EILSEQ; return kInvalid; }
    return static_cast<size_t>(r);
}

size_t c32rtomb(char *s, char32_t c32, mbstate_t *ps) noexcept
{
    mbstate_t &st = ps ? *ps : g_c32rtomb;

    char scratch[4];
    if (!s) { s = scratch; c32 = U'\0'; Reset(st); }

    const int r = __utf8::Encode(s, c32);
    if (r < 0) { errno = EILSEQ; return kInvalid; }
    return static_cast<size_t>(r);
}

}