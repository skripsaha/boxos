
#include <string>
#include <string_view>
#include <ostream>
#include <istream>

namespace std {

template <class CharT, class Traits>
basic_ostream<CharT, Traits> &operator<<(basic_ostream<CharT, Traits> &os,
                                          basic_string_view<CharT, Traits> sv)
{
    typename basic_ostream<CharT, Traits>::sentry ok(os);
    if (ok) {
        try {
            __ios::EmitField(os, nullptr, 0, nullptr, 0, sv.data(),
                              static_cast<streamsize>(sv.size()), __ios::Align::Right);
        } catch (...) {
            __ios::HandleException(os);
        }
        os.width(0);
    }
    return os;
}

template <class CharT, class Traits, class Allocator>
basic_ostream<CharT, Traits> &operator<<(basic_ostream<CharT, Traits> &os,
                                          const basic_string<CharT, Traits, Allocator> &s)
{
    return os << basic_string_view<CharT, Traits>(s);
}

template <class CharT, class Traits, class Allocator>
basic_istream<CharT, Traits> &operator>>(basic_istream<CharT, Traits> &is,
                                          basic_string<CharT, Traits, Allocator> &s)
{
    typename basic_istream<CharT, Traits>::sentry ok(is);
    if (ok) {
        streamsize w     = is.width();
        streamsize n     = w > 0 ? w : static_cast<streamsize>(s.max_size());
        streamsize count = 0;
        bool       hitEof = false;
        try {
            s.erase();
            auto               *buf = is.rdbuf();
            const locale        loc = is.getloc();
            const ctype<CharT> &ct  = use_facet<ctype<CharT>>(loc);
            for (; count < n;) {
                typename Traits::int_type c = buf->sgetc();
                if (Traits::eq_int_type(c, Traits::eof())) {
                    hitEof = true;
                    break;
                }
                CharT ch = Traits::to_char_type(c);
                if (ct.is(ctype_base::space, ch)) break;
                s.push_back(ch);
                buf->sbumpc();
                ++count;
            }
        } catch (...) {
            __ios::HandleException(is);
        }
        is.width(0);
        if (count == 0) {
            is.setstate(hitEof ? (ios_base::failbit | ios_base::eofbit) : ios_base::failbit);
        } else if (hitEof) {
            is.setstate(ios_base::eofbit);
        }
    } else {
        is.setstate(ios_base::failbit);
    }
    return is;
}

template <class CharT, class Traits, class Allocator>
basic_istream<CharT, Traits> &getline(basic_istream<CharT, Traits> &is,
                                       basic_string<CharT, Traits, Allocator> &s, CharT delim)
{
    typename basic_istream<CharT, Traits>::sentry ok(is, true);
    if (ok) {
        bool hitEof = false, foundDelim = false;
        try {
            s.erase();
            auto *buf = is.rdbuf();
            for (;;) {
                typename Traits::int_type c = buf->sgetc();
                if (Traits::eq_int_type(c, Traits::eof())) {
                    hitEof = true;
                    break;
                }
                CharT ch = Traits::to_char_type(c);
                if (Traits::eq(ch, delim)) {
                    buf->sbumpc();
                    foundDelim = true;
                    break;
                }
                if (s.size() >= s.max_size()) break;
                s.push_back(ch);
                buf->sbumpc();
            }
        } catch (...) {
            __ios::HandleException(is);
        }
        if (s.empty() && !foundDelim) {
            is.setstate(hitEof ? (ios_base::failbit | ios_base::eofbit) : ios_base::failbit);
        } else if (hitEof) {
            is.setstate(ios_base::eofbit);
        }
    } else {
        is.setstate(ios_base::failbit);
    }
    return is;
}

template <class CharT, class Traits, class Allocator>
basic_istream<CharT, Traits> &getline(basic_istream<CharT, Traits> &&is,
                                       basic_string<CharT, Traits, Allocator> &s, CharT delim)
{
    return getline(is, s, delim);
}
template <class CharT, class Traits, class Allocator>
basic_istream<CharT, Traits> &getline(basic_istream<CharT, Traits> &is,
                                       basic_string<CharT, Traits, Allocator> &s)
{
    return getline(is, s, is.widen('\n'));
}
template <class CharT, class Traits, class Allocator>
basic_istream<CharT, Traits> &getline(basic_istream<CharT, Traits> &&is,
                                       basic_string<CharT, Traits, Allocator> &s)
{
    return getline(is, s, is.widen('\n'));
}

template basic_ostream<wchar_t> &operator<< <wchar_t, char_traits<wchar_t>>(
    basic_ostream<wchar_t> &, basic_string_view<wchar_t, char_traits<wchar_t>>);
template basic_ostream<wchar_t> &operator<< <wchar_t, char_traits<wchar_t>, allocator<wchar_t>>(
    basic_ostream<wchar_t> &, const basic_string<wchar_t, char_traits<wchar_t>, allocator<wchar_t>> &);
template basic_istream<wchar_t> &operator>> <wchar_t, char_traits<wchar_t>, allocator<wchar_t>>(
    basic_istream<wchar_t> &, basic_string<wchar_t, char_traits<wchar_t>, allocator<wchar_t>> &);
template basic_istream<wchar_t> &getline<wchar_t, char_traits<wchar_t>, allocator<wchar_t>>(
    basic_istream<wchar_t> &, basic_string<wchar_t, char_traits<wchar_t>, allocator<wchar_t>> &, wchar_t);
template basic_istream<wchar_t> &getline<wchar_t, char_traits<wchar_t>, allocator<wchar_t>>(
    basic_istream<wchar_t> &&, basic_string<wchar_t, char_traits<wchar_t>, allocator<wchar_t>> &, wchar_t);
template basic_istream<wchar_t> &getline<wchar_t, char_traits<wchar_t>, allocator<wchar_t>>(
    basic_istream<wchar_t> &, basic_string<wchar_t, char_traits<wchar_t>, allocator<wchar_t>> &);
template basic_istream<wchar_t> &getline<wchar_t, char_traits<wchar_t>, allocator<wchar_t>>(
    basic_istream<wchar_t> &&, basic_string<wchar_t, char_traits<wchar_t>, allocator<wchar_t>> &);

template basic_ostream<char> &operator<< <char, char_traits<char>>(
    basic_ostream<char> &, basic_string_view<char, char_traits<char>>);
template basic_ostream<char> &operator<< <char, char_traits<char>, allocator<char>>(
    basic_ostream<char> &, const basic_string<char, char_traits<char>, allocator<char>> &);
template basic_istream<char> &operator>> <char, char_traits<char>, allocator<char>>(
    basic_istream<char> &, basic_string<char, char_traits<char>, allocator<char>> &);
template basic_istream<char> &getline<char, char_traits<char>, allocator<char>>(
    basic_istream<char> &, basic_string<char, char_traits<char>, allocator<char>> &, char);
template basic_istream<char> &getline<char, char_traits<char>, allocator<char>>(
    basic_istream<char> &&, basic_string<char, char_traits<char>, allocator<char>> &, char);
template basic_istream<char> &getline<char, char_traits<char>, allocator<char>>(
    basic_istream<char> &, basic_string<char, char_traits<char>, allocator<char>> &);
template basic_istream<char> &getline<char, char_traits<char>, allocator<char>>(
    basic_istream<char> &&, basic_string<char, char_traits<char>, allocator<char>> &);

}