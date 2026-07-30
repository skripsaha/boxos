/*
 * string_io.cpp — out-of-line [string.io]/[string.view.io] bodies.
 *
 * <string>/<string_view> declare operator<</operator>>/getline against
 * <iosfwd>'s forward-declared basic_ostream/basic_istream only: <string>
 * cannot #include <ostream> (<ios>, needed by every stream header,
 * already transitively needs <string> via ios_base::failure : system_error
 * : runtime_error(const string&), so the reverse edge would cycle back
 * through <ios>). This is the one translation unit that includes both
 * sides together and provides the bodies, explicitly instantiated for
 * boxcxx's one real instantiation (CharT=char, Traits=char_traits<char>,
 * Allocator=allocator<char>) -- wide streams are a documented, permanent
 * exclusion (see <iosfwd>). Every OTHER translation unit only ever sees
 * the template declaration in <string>/<string_view>; the linker resolves
 * calls against the explicit instantiations emitted at the bottom of this
 * file.
 */

#include <string>
#include <string_view>
#include <ostream>
#include <istream>

namespace std {

// [string.view.io]: formatted output, the same shape as the const-char*
// free inserter in <ostream> -- right-padded via width()/fill()/
// adjustfield, no sign/prefix, single sputn-equivalent write.
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

// [string.io]: "Equivalent to: return os << basic_string_view<charT,
// traits>(str);" -- the real logic lives entirely on the string_view
// overload above.
template <class CharT, class Traits, class Allocator>
basic_ostream<CharT, Traits> &operator<<(basic_ostream<CharT, Traits> &os,
                                          const basic_string<CharT, Traits, Allocator> &s)
{
    return os << basic_string_view<CharT, Traits>(s);
}

// [string.io]: formatted input. Caps at is.width() if set (>0), else at
// s.max_size(); stops at the first "C"-locale whitespace character (which
// is NOT extracted) or EOF. width() is reset to 0 afterward unconditionally.
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
            auto *buf = is.rdbuf();
            for (; count < n;) {
                typename Traits::int_type c = buf->sgetc();
                if (Traits::eq_int_type(c, Traits::eof())) {
                    hitEof = true;
                    break;
                }
                CharT ch = Traits::to_char_type(c);
                if (__ios::IsSpace(ch)) break;
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

// [string.io]: unformatted (per its own wording, does not affect gcount()
// -- never touched here since this is built purely on the streambuf's
// public API, not on basic_istream's own gcount tracker). Extracts raw
// characters (no leading-whitespace skip) and appends them until an
// unextracted delimiter, s.max_size() characters, or EOF; the delimiter
// itself IS extracted but never appended.
//
// Reaching the delimiter counts as "extracting a character" for the
// failbit rule below even though nothing is appended for it -- a blank
// line (delimiter found immediately) is therefore a successful EMPTY
// read, matching basic_istream::getline's own already-shipped member
// semantics exactly (Ф30e commit 3; cxxtest phase117's own getline
// HOTSPOT coverage), not a failure.
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

// C++11 rvalue-stream overloads: `is` is an lvalue expression inside the
// function body (only its declared TYPE is an rvalue reference), so this
// forwards straight to the lvalue overload above.
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

// Explicit instantiation -- boxcxx's one real instantiation.
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

} // namespace std
