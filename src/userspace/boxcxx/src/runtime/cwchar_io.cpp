// boxcxx — <cwchar> runtime, wide character input and output
//
// ── What a stream's "end" actually is here ─────────────────────────────────
// These functions return WEOF, and `<cwchar>`'s `wctob` returns EOF, because
// ISO C says so. Neither is a BoxOS concept and neither is inherited: BoxOS
// replaced the Unix end-of-file with an honest channel state years before this
// header existed, and `box/current.h` says so in as many words — the row in its
// table reads "EOF (Ctrl-D / file size) -> honest writer closed
// (CURRENT_CLOSED)". There is exactly one place where the two vocabularies
// meet, `cstdio.cpp`'s refill, which turns CURRENT_CLOSED into the stream's eof
// bit. Everything here reads that bit; nothing here invents an end.
//
// The consequence is visible and documented (§2 <iostream>): the keyboard
// Current is a live console with no terminator, so a wide read from `stdin`
// never reports an end either. That is a fact about the channel, not a defect
// in the wrapper.
//
// ── Why this file needs the inside of a FILE ───────────────────────────────
// A wide character is up to four bytes, and it must be assembled while the
// stream's lock is held ONCE. Taking and releasing the lock per byte would let
// another strand interleave halfway through a character, and half a character
// is not something a reader can recover from. So the bytes move through
// GetLocked/PutLocked under a single Guard, all of which <__bits/c_file> now
// publishes — see that leaf for why it exists at all.
//
// ── Orientation, and the push-back slot it pays for ────────────────────────
// C forbids mixing byte and wide operations on one stream; that prohibition is
// what `fwide` reports. It also buys something concrete: the byte push-back and
// the wide push-back can never both be live, so `FILE::__unget` holds either,
// and `ungetwc` cost no new field and no change to the struct's layout.

#include <cerrno>
#include <cstddef>
#include <cwchar>
#include <__bits/c_file>

namespace {

using ::std::__stdio::ClaimByte;
using ::std::__stdio::ClaimWide;
using ::std::__stdio::GetLocked;
using ::std::__stdio::Guard;
using ::std::__stdio::kEof;
using ::std::__stdio::kErr;
using ::std::__stdio::PutLocked;

// FILE::__unget holds EOF when empty, which <cstdio> sets in six places. This
// file has <cstdio> through the leaf, so the value is read from there rather
// than restated — a second spelling of it is a second thing to keep level.
//
// GetLocked consumes __unget itself, as a BYTE. A wide character parked there
// would be handed back one byte at a time by the byte path — and the only
// reason that cannot happen is the orientation check below, which refuses a
// byte read on a wide-oriented stream. Verified against cstdio.cpp:203.

// Reads one whole character with the lock already held. Returns WEOF at the end
// of the stream and on an ill-formed sequence, which C makes indistinguishable
// except through ferror/feof — so the error bit is what separates them.
::std::wint_t GetWideLocked(::std::FILE *f) noexcept
{
    if (f->__unget != EOF) {
        const ::std::wint_t w = static_cast<::std::wint_t>(f->__unget);
        f->__unget = EOF;
        return w;
    }

    ::std::mbstate_t st{};
    for (int taken = 0; taken < 4; ++taken) {
        const int b = GetLocked(f);
        if (b == EOF) {
            // A stream that ends mid-character is ill-formed, not merely over.
            if (taken > 0) { f->__flags |= kErr; errno = EILSEQ; }
            return WEOF;
        }
        const char        byte = static_cast<char>(b);
        wchar_t           wc   = 0;
        const ::std::size_t r  = ::std::mbrtowc(&wc, &byte, 1, &st);
        if (r == static_cast<::std::size_t>(-2)) continue;          // need more
        if (r == static_cast<::std::size_t>(-1)) {
            f->__flags |= kErr;
            errno = EILSEQ;
            return WEOF;
        }
        return static_cast<::std::wint_t>(wc);
    }
    f->__flags |= kErr;                 // four bytes that did not resolve
    errno = EILSEQ;
    return WEOF;
}

::std::wint_t PutWideLocked(::std::FILE *f, wchar_t c) noexcept
{
    char              buf[8];
    ::std::mbstate_t  st{};
    const ::std::size_t n = ::std::wcrtomb(buf, c, &st);
    if (n == static_cast<::std::size_t>(-1)) {
        f->__flags |= kErr;
        errno = EILSEQ;
        return WEOF;
    }
    for (::std::size_t i = 0; i < n; ++i)
        if (PutLocked(f, static_cast<unsigned char>(buf[i])) == EOF) return WEOF;
    return static_cast<::std::wint_t>(c);
}

} // namespace

namespace std {

int fwide(FILE *stream, int mode) noexcept
{
    if (!stream) return 0;
    Guard g(stream);
    if (mode > 0) ClaimWide(stream);
    else if (mode < 0) ClaimByte(stream);
    if (stream->__flags & __stdio::kWideOriented) return 1;
    if (stream->__flags & __stdio::kByteOriented) return -1;
    return 0;
}

wint_t fgetwc(FILE *stream) noexcept
{
    if (!stream) return WEOF;
    Guard g(stream);
    if (!ClaimWide(stream)) { stream->__flags |= kErr; return WEOF; }
    return GetWideLocked(stream);
}

wint_t fputwc(wchar_t c, FILE *stream) noexcept
{
    if (!stream) return WEOF;
    Guard g(stream);
    if (!ClaimWide(stream)) { stream->__flags |= kErr; return WEOF; }
    return PutWideLocked(stream, c);
}

// C makes getwc and putwc macros that may evaluate their stream argument more
// than once. Functions cannot do that, and a function is what [cwchar.syn]
// requires of a C++ implementation — so the licence is simply unused.
wint_t getwc(FILE *stream) noexcept { return fgetwc(stream); }
wint_t putwc(wchar_t c, FILE *stream) noexcept { return fputwc(c, stream); }
wint_t getwchar() noexcept { return fgetwc(stdin); }
wint_t putwchar(wchar_t c) noexcept { return fputwc(c, stdout); }

// One character of push-back, which is all C guarantees. It cannot go through
// ungetc: that promises one BYTE, and a character here may be four.
wint_t ungetwc(wint_t c, FILE *stream) noexcept
{
    if (!stream || c == WEOF) return WEOF;
    Guard g(stream);
    if (!ClaimWide(stream)) return WEOF;
    if (stream->__unget != EOF) return WEOF;        // the slot is already full
    stream->__unget = static_cast<int>(c);
    stream->__flags &= ~kEof;                       // pushing back undoes the end
    return c;
}

// Stops after a newline and keeps it, as C specifies — and writes nothing at
// all if the very first read reports the end, so a caller can tell "nothing
// left" from "an empty line".
wchar_t *fgetws(wchar_t *s, int n, FILE *stream) noexcept
{
    if (!s || n <= 0 || !stream) return nullptr;
    Guard g(stream);
    if (!ClaimWide(stream)) { stream->__flags |= kErr; return nullptr; }

    int i = 0;
    while (i < n - 1) {
        const wint_t w = GetWideLocked(stream);
        if (w == WEOF) { if (i == 0) return nullptr; break; }
        s[i++] = static_cast<wchar_t>(w);
        if (w == static_cast<wint_t>(L'\n')) break;
    }
    s[i] = L'\0';
    return s;
}

int fputws(const wchar_t *s, FILE *stream) noexcept
{
    if (!s || !stream) return -1;
    Guard g(stream);
    if (!ClaimWide(stream)) { stream->__flags |= kErr; return -1; }
    for (; *s != L'\0'; ++s)
        if (PutWideLocked(stream, *s) == WEOF) return -1;
    return 0;
}

} // namespace std
