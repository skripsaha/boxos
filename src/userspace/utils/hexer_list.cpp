/* hexer_list.cpp — under an arrow the result is text for a file, not a frame:
 * the part a line of sixteen bytes at a time, offset, hex and their text, each
 * line at a whole multiple of sixteen so a column is always the same low digit
 * of the offset, and the lines equal to the one above counted in words. No
 * colour: a file keeps none. What goes wrong is said to the person. */

#include <array>
#include <cstring>
#include <format>
#include <string>
#include <vector>

#include "box/print.h"

#include "file_heading.h"
#include "hexer.h"

namespace {

constexpr std::uint32_t ListWidth = 16;
constexpr std::size_t   ListChunk = 65536;
constexpr char          HexDigit[] = "0123456789abcdef";

class Listing {
public:
    explicit Listing(unsigned Digits) : Digits(Digits) {}

    /* Bytes read at At, into the lines they belong to. */
    void Take(std::uint64_t At, const std::uint8_t *Bytes, std::size_t Count)
    {
        for (std::size_t I = 0; I < Count; I++) {
            std::uint64_t Place  = At + I;
            unsigned      Column = static_cast<unsigned>(Place % ListWidth);
            if (!Open) {
                Open      = true;
                RowAt     = Place - Column;
                RowFirst  = Column;
            }
            Row[Column] = Bytes[I];
            RowLast     = Column + 1;
            if (RowLast == ListWidth) EndRow();
        }
    }

    void Finish()
    {
        if (Open) EndRow();
        SayRepeats();
    }

private:
    void Show(std::uint64_t At, const std::uint8_t *Bytes, unsigned First, unsigned Last)
    {
        std::string Line = std::format("{:0{}x}  ", At, Digits);
        for (unsigned I = 0; I < ListWidth; I++) {
            if (I == ListWidth / 2) Line += ' ';
            if (I >= First && I < Last) {
                Line += HexDigit[Bytes[I] >> 4];
                Line += HexDigit[Bytes[I] & 0xf];
            } else {
                Line += "  ";
            }
            Line += ' ';
        }
        Line.append(1 + First, ' ');
        for (unsigned I = First; I < Last; I++)
            Line += Bytes[I] >= 0x20 && Bytes[I] < 0x7f ? static_cast<char>(Bytes[I]) : '.';
        Line += '\n';
        print_bytes(Line.data(), Line.size());
    }

    /* The whole lines equal to the last one shown: one is shown as it is,
     * more are counted. */
    void SayRepeats()
    {
        if (Repeats == 1) {
            Show(ShownAt + ListWidth, Shown.data(), 0, ListWidth);
        } else if (Repeats > 1) {
            std::string Line = std::format("{:{}}  ... {} more lines the same\n", "", Digits, Repeats);
            print_bytes(Line.data(), Line.size());
        }
        Repeats = 0;
    }

    void EndRow()
    {
        bool Whole = RowFirst == 0 && RowLast == ListWidth;
        Open = false;
        if (Whole && ShownWhole && Row == Shown) {
            Repeats++;
            return;
        }
        SayRepeats();
        Show(RowAt, Row.data(), RowFirst, RowLast);
        ShownWhole = Whole;
        if (Whole) {
            Shown   = Row;
            ShownAt = RowAt;
        }
    }

    unsigned                               Digits;
    std::array<std::uint8_t, ListWidth>    Row{};
    std::uint64_t                          RowAt    = 0;
    unsigned                               RowFirst = 0;
    unsigned                               RowLast  = 0;
    bool                                   Open     = false;
    std::array<std::uint8_t, ListWidth>    Shown{};
    std::uint64_t                          ShownAt    = 0;
    bool                                   ShownWhole = false;
    std::uint64_t                          Repeats    = 0;
};

}

bool HexerList(file_id_t File, const FileMetadata &Metadata, HexerPart Part)
{
    std::string Heading = FileHeading(Metadata);
    print_bytes(Heading.data(), Heading.size());
    if (Part.Lo == Part.Hi) {
        println("the file is empty");
        return true;
    }

    Listing                   Lines(OffsetDigits(Part.Hi));
    std::vector<std::uint8_t> Chunk(ListChunk);
    std::uint64_t             At  = Part.Lo;
    std::int64_t              Got = 1;
    while (At < Part.Hi) {
        std::uint64_t Want = Part.Hi - At > ListChunk ? ListChunk : Part.Hi - At;
        Got = fread(File, At, Chunk.data(), static_cast<std::size_t>(Want));
        if (Got <= 0) break;
        Lines.Take(At, Chunk.data(), static_cast<std::size_t>(Got));
        At += static_cast<std::uint64_t>(Got);
    }
    Lines.Finish();

    if (Got < 0) {
        print_to_screen(std::format("hexer: the file would not be read at 0x{:x}\n", At).c_str());
        return false;
    }
    if (At < Part.Hi)
        print_to_screen(std::format("hexer: the file was cut short while it was read: it ends at 0x{:x}\n", At).c_str());
    return true;
}
