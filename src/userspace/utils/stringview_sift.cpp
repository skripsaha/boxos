/* stringview_sift.cpp — the sieve: the file's bytes in, from offset 0 on, and
 * each text out as it ends. stringview.h says what a text is. */

#include "stringview.h"

namespace {

/* A code point past ASCII that is seen when written: not a C1 control, not a
 * noncharacter, and none of the invisible marks that steer text without being
 * any — zero widths, direction marks, the line and paragraph separators, the
 * byte order mark. */
bool Seen(std::uint32_t Point)
{
    if (Point < 0xa0) return false;
    if (Point >= 0xfdd0 && Point <= 0xfdef) return false;
    if ((Point & 0xfffe) == 0xfffe) return false;
    if (Point >= 0x200b && Point <= 0x200f) return false;
    if (Point >= 0x2028 && Point <= 0x202e) return false;
    if (Point >= 0x2060 && Point <= 0x206f) return false;
    return Point != 0xfeff;
}

}

void Sifter::Put(Run &Into, std::uint64_t At, const std::uint8_t *Bytes, std::size_t Count)
{
    if (!Into.Open) {
        Into.Open   = true;
        Into.Start  = At;
        Into.Length = 0;
        Into.Text.clear();
    }
    Into.Text.append(reinterpret_cast<const char *>(Bytes), Count);
    Into.Length++;
}

void Sifter::End(Run &Ending, bool Wide)
{
    if (!Ending.Open) return;
    Ending.Open = false;
    if (Ending.Length < Shortest) return;
    OnText(StringviewText{Ending.Start, Wide, std::move(Ending.Text)});
    Ending.Text.clear();
}

/* A UTF-8 sequence of Need bytes begins; its second byte lies in Low..High. */
void Sifter::Begin(std::uint8_t Lead, std::uint64_t At, unsigned Need, std::uint8_t Low, std::uint8_t High)
{
    Sequence[0]  = Lead;
    SequenceUsed = 1;
    SequenceNeed = Need;
    SequenceAt   = At;
    NextLow      = Low;
    NextHigh     = High;
    Point        = Lead & (0x7fu >> Need);
}

void Sifter::Narrow(std::uint8_t Byte, std::uint64_t At)
{
    if (SequenceNeed != 0) {
        if (Byte >= NextLow && Byte <= NextHigh) {
            Sequence[SequenceUsed++] = Byte;
            Point    = (Point << 6) | (Byte & 0x3fu);
            NextLow  = 0x80;
            NextHigh = 0xbf;
            if (SequenceUsed < SequenceNeed) return;

            SequenceNeed = 0;
            if (Seen(Point)) Put(NarrowRun, SequenceAt, Sequence, SequenceUsed);
            else             End(NarrowRun, false);
            return;
        }
        /* The sequence is broken; the byte that broke it may begin a text. */
        SequenceNeed = 0;
        End(NarrowRun, false);
    }

    if (Byte == '\t' || (Byte >= 0x20 && Byte < 0x7f)) Put(NarrowRun, At, &Byte, 1);
    else if (Byte >= 0xc2 && Byte <= 0xdf)             Begin(Byte, At, 2, 0x80, 0xbf);
    else if (Byte == 0xe0)                             Begin(Byte, At, 3, 0xa0, 0xbf);
    else if (Byte == 0xed)                             Begin(Byte, At, 3, 0x80, 0x9f);
    else if (Byte >= 0xe1 && Byte <= 0xef)             Begin(Byte, At, 3, 0x80, 0xbf);
    else if (Byte == 0xf0)                             Begin(Byte, At, 4, 0x90, 0xbf);
    else if (Byte >= 0xf1 && Byte <= 0xf3)             Begin(Byte, At, 4, 0x80, 0xbf);
    else if (Byte == 0xf4)                             Begin(Byte, At, 4, 0x80, 0x8f);
    else                                               End(NarrowRun, false);
}

/* The UTF-16 unit Low, High that begins at At. */
void Sifter::WideUnit(std::uint8_t Low, std::uint8_t High, std::uint64_t At)
{
    Run &Into = WideRuns[At & 1];
    if (High != 0 || !(Low == '\t' || (Low >= 0x20 && Low < 0x7f) || Low >= 0xa0)) {
        End(Into, true);
        return;
    }
    std::uint8_t Utf8[2] = {Low, 0};
    std::size_t  Count   = 1;
    if (Low >= 0x80) {
        Utf8[0] = static_cast<std::uint8_t>(0xc0 | (Low >> 6));
        Utf8[1] = static_cast<std::uint8_t>(0x80 | (Low & 0x3f));
        Count   = 2;
    }
    Put(Into, At, Utf8, Count);
}

void Sifter::Take(std::uint64_t At, const std::uint8_t *Bytes, std::size_t Count)
{
    for (std::size_t I = 0; I < Count; I++) {
        std::uint64_t Position = At + I;
        if (Position > 0) WideUnit(Previous, Bytes[I], Position - 1);
        Narrow(Bytes[I], Position);
        Previous = Bytes[I];
    }
}

void Sifter::Finish()
{
    End(NarrowRun, false);
    End(WideRuns[0], true);
    End(WideRuns[1], true);
}
