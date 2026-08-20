/*
 * iostream.cpp — the four standard stream objects, and the two stream
 * buffers that bind them to the BoxOS Current spine.
 *
 * Why these live in exactly one translation unit, built by a prioritised
 * constructor: [iostream.objects] requires the objects to be usable from any
 * other global constructor in the program, and ordinary namespace-scope
 * globals give no such guarantee across translation units. Priority 102 puts
 * them after the TLS bootstrap (tls_init.cpp, priority 101 — the streams
 * allocate, and allocation touches thread_local state) and ahead of every
 * default-priority global anywhere in the program. Destruction is the mirror
 * of construction, so they are torn down last, after the ios_base::Init in
 * any user translation unit has already flushed them.
 *
 *   std::cout / cerr / clog -> the "screen" Current, through the SAME handle
 *                              std::print uses (std::__print::Console), so the
 *                              two interleave in call order.
 *   std::cin                -> the "keyboard" Current.
 *
 * ios_base::Init lives here rather than in ios.cpp because what it sequences
 * lives here. A program that never mentions a stream object still links
 * cleanly; one that constructs an ios_base::Init pulls this object file in,
 * which is exactly what asking for the guarantee means.
 */

#include <iostream>
#include <print>       // std::__print::Console / ToConsole — the one screen road
#include <string_view>

#include "box/current.h"

namespace std {

namespace {

// ── output ──────────────────────────────────────────────────────────────
// No buffer of its own. print_bytes underneath the screen Current already
// keeps one (flushed at 256 bytes and at exit), and a second buffer stacked
// on top of it would only add a way for std::cout and std::print to come out
// in an order neither caller asked for.
class ConsoleOutBuf final : public streambuf {
protected:
    streamsize xsputn(const char_type *s, streamsize n) override
    {
        if (n <= 0) return 0;
        __print::ToConsole(string_view(s, static_cast<size_t>(n)));
        return n;
    }

    int_type overflow(int_type c = traits_type::eof()) override
    {
        if (traits_type::eq_int_type(c, traits_type::eof())) return traits_type::not_eof(c);
        const char ch = traits_type::to_char_type(c);
        __print::ToConsole(string_view(&ch, 1));
        return c;
    }

    int sync() override
    {
        if (::Current *c = __print::Console()) ::current_flush(c);
        return 0;
    }
};

// ── input ───────────────────────────────────────────────────────────────
// The keyboard Current is line-oriented: one call hands back one edited line,
// with the terminator STRIPPED. That line is the get area. The newline is put
// back on the end, because a character stream without line terminators is a
// different stream — std::getline would never find its delimiter and would
// splice every line the user ever typed into one.
class ConsoleInBuf final : public streambuf {
public:
    ConsoleInBuf() { setg(__buf, __buf, __buf); }

protected:
    int_type underflow() override
    {
        if (gptr() < egptr()) return traits_type::to_int_type(*gptr());

        ::Current *k = Keyboard();
        if (!k) return traits_type::eof();

        // One byte held back so the terminator always has somewhere to go.
        const int n = ::current_read(k, __buf, sizeof(__buf) - 1);
        if (n < 0) return traits_type::eof();
        __buf[n] = '\n';
        setg(__buf, __buf, __buf + n + 1);
        return traits_type::to_int_type(*gptr());
    }

private:
    static ::Current *Keyboard()
    {
        static ::Current *k = ::current_open("keyboard", CURRENT_READ, 0, 0);
        return k;
    }

    char __buf[1024];
};

// Constructed at 102 along with the streams below, in declaration order.
[[gnu::init_priority(102)]] ConsoleOutBuf g_out;
[[gnu::init_priority(102)]] ConsoleInBuf  g_in;

// ── the wide half (Ф42-f) ───────────────────────────────────────────────
// ‼ MIXING cout AND wcout IS WELL-DEFINED HERE, AND THAT IS A DIVERGENCE
// WORTH STATING. [iostream.objects]/2 defers to C, and C forbids mixing
// byte and wide operations on one stream — because a wide write can leave a
// conversion half-done in a shift state that a byte write then walks over.
// This system has no such state: the encoding is UTF-8 and Ф42-e proved its
// codecvt reads and writes none. Both halves emit the same alphabet through
// the same road (__print::ToConsole), and neither keeps a buffer of its own,
// so `cout << "a"; wcout << L"б";` comes out in call order. Refusing it would
// be a restriction with nothing underneath it.
class WideConsoleOutBuf final : public wstreambuf {
    using Codecvt = codecvt<wchar_t, char, mbstate_t>;

protected:
    streamsize xsputn(const char_type *s, streamsize n) override
    {
        if (n <= 0) return 0;
        const Codecvt &cvt = use_facet<Codecvt>(this->__loc_ref());

        const char_type       *from = s;
        const char_type *const end  = s + n;
        while (from < end) {
            char             ext[256];
            const char_type *fn = nullptr;
            char            *tn = nullptr;
            mbstate_t        st{};
            const codecvt_base::result r =
                cvt.out(st, from, end, fn, ext, ext + sizeof(ext), tn);
            // error is a character no byte sequence can carry. What was
            // already converted has been shown; the count says how much.
            if (r == codecvt_base::error || tn == ext || fn == from)
                return static_cast<streamsize>(from - s);
            __print::ToConsole(string_view(ext, static_cast<size_t>(tn - ext)));
            from = fn;
        }
        return n;
    }

    int_type overflow(int_type c = traits_type::eof()) override
    {
        if (traits_type::eq_int_type(c, traits_type::eof())) return traits_type::not_eof(c);
        const char_type ch = traits_type::to_char_type(c);
        return xsputn(&ch, 1) == 1 ? c : traits_type::eof();
    }

    int sync() override
    {
        if (::Current *c = __print::Console()) ::current_flush(c);
        return 0;
    }
};

// The wide input side draws its BYTES FROM g_in, the narrow console buffer,
// rather than from the keyboard Current directly. One line buffer and one
// cursor, so a line typed at the keyboard cannot end up half in cin and half
// in wcin — which is what two independent readers of one device would give,
// with nothing to say which half went where.
class WideConsoleInBuf final : public wstreambuf {
    using Codecvt = codecvt<wchar_t, char, mbstate_t>;
    using NTraits = char_traits<char>;

public:
    WideConsoleInBuf() { setg(__wbuf, __wbuf, __wbuf); }

protected:
    int_type underflow() override
    {
        if (gptr() < egptr()) return traits_type::to_int_type(*gptr());

        const Codecvt &cvt = use_facet<Codecvt>(this->__loc_ref());
        char_type      *w  = __wbuf;
        char_type *const wend = __wbuf + kW;

        while (w < wend) {
            // The first character may block — that is what reading a console
            // is. After it, stop as soon as the narrow buffer runs dry, which
            // for a line-oriented device is the end of the line.
            if (w != __wbuf && g_in.in_avail() <= 0) break;

            char ext[8];
            int  n   = 0;
            bool got = false;
            while (n < static_cast<int>(sizeof(ext))) {
                const NTraits::int_type b = g_in.sbumpc();
                if (NTraits::eq_int_type(b, NTraits::eof())) break;
                ext[n++] = NTraits::to_char_type(b);

                const char *fn = nullptr;
                char_type  *tn = nullptr;
                mbstate_t   st{};
                const codecvt_base::result r = cvt.in(st, ext, ext + n, fn, w, w + 1, tn);
                if (r == codecvt_base::error) break;
                if (tn > w) { got = true; break; }
            }
            if (!got) break;
            w++;
            if (w[-1] == L'\n') break;
        }
        if (w == __wbuf) return traits_type::eof();
        setg(__wbuf, __wbuf, w);
        return traits_type::to_int_type(*gptr());
    }

private:
    static constexpr int kW = 256;
    char_type            __wbuf[kW];
};

[[gnu::init_priority(102)]] WideConsoleOutBuf g_wout;
[[gnu::init_priority(102)]] WideConsoleInBuf  g_win;

int g_init_refs = 0;

} // namespace

[[gnu::init_priority(102)]] istream cin(&g_in);
[[gnu::init_priority(102)]] ostream cout(&g_out);
[[gnu::init_priority(102)]] ostream cerr(&g_out);
[[gnu::init_priority(102)]] ostream clog(&g_out);

[[gnu::init_priority(102)]] wistream wcin(&g_win);
[[gnu::init_priority(102)]] wostream wcout(&g_wout);
[[gnu::init_priority(102)]] wostream wcerr(&g_wout);
[[gnu::init_priority(102)]] wostream wclog(&g_wout);

namespace {

// [iostream.objects]/3-5: cin is tied to cout, cerr is tied to cout and is
// unit-buffered, clog is neither. Done at 103 rather than in the object
// initialisers above so it cannot depend on the emission order of two
// entries that share a priority.
[[gnu::constructor(103)]] void BoxCxxIostreamTies()
{
    cin.tie(&cout);
    cerr.tie(&cout);
    cerr.setf(ios_base::unitbuf);

    wcin.tie(&wcout);
    wcerr.tie(&wcout);
    wcerr.setf(ios_base::unitbuf);
}

} // namespace

// ── ios_base::Init ──────────────────────────────────────────────────────
// A counter, not a constructor of the streams: they are already built by the
// time any Init can run. What it owns is the other end — flushing while the
// objects are still alive, which is only knowable at the last Init's death.
ios_base::Init::Init() { ++g_init_refs; }

ios_base::Init::~Init()
{
    if (--g_init_refs != 0) return;
    cout.flush();
    cerr.flush();
    clog.flush();
}

} // namespace std
