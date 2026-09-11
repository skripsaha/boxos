
#include <iostream>
#include <print>
#include <string_view>

#include "box/current.h"

namespace std {

namespace {

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

class ConsoleInBuf final : public streambuf {
public:
    ConsoleInBuf() { setg(__buf, __buf, __buf); }

protected:
    int_type underflow() override
    {
        if (gptr() < egptr()) return traits_type::to_int_type(*gptr());

        ::Current *k = Keyboard();
        if (!k) return traits_type::eof();

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

[[gnu::init_priority(102)]] ConsoleOutBuf g_out;
[[gnu::init_priority(102)]] ConsoleInBuf  g_in;

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

}

[[gnu::init_priority(102)]] istream cin(&g_in);
[[gnu::init_priority(102)]] ostream cout(&g_out);
[[gnu::init_priority(102)]] ostream cerr(&g_out);
[[gnu::init_priority(102)]] ostream clog(&g_out);

[[gnu::init_priority(102)]] wistream wcin(&g_win);
[[gnu::init_priority(102)]] wostream wcout(&g_wout);
[[gnu::init_priority(102)]] wostream wcerr(&g_wout);
[[gnu::init_priority(102)]] wostream wclog(&g_wout);

namespace {

[[gnu::constructor(103)]] void BoxCxxIostreamTies()
{
    cin.tie(&cout);
    cerr.tie(&cout);
    cerr.setf(ios_base::unitbuf);

    wcin.tie(&wcout);
    wcerr.tie(&wcout);
    wcerr.setf(ios_base::unitbuf);
}

}

ios_base::Init::Init() { ++g_init_refs; }

ios_base::Init::~Init()
{
    if (--g_init_refs != 0) return;
    cout.flush();
    cerr.flush();
    clog.flush();
}

}