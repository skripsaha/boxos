/* What difference knows of text and of bytes before it shows anything. */

#include <algorithm>
#include <cstring>

#include "difference.h"

namespace {

bool IsContinuation(char Byte)
{
    return (static_cast<std::uint8_t>(Byte) & 0xc0) == 0x80;
}

bool IsStrayControl(std::uint8_t Byte)
{
    if (Byte >= 0x20) return false;
    return !(Byte == '\t' || Byte == '\b' || Byte == '\n' || Byte == '\f' || Byte == '\r' || Byte == 0x1b);
}

}

TextFile DifferenceSplit(std::string Label, std::string_view Bytes)
{
    TextFile File;
    File.Label = std::move(Label);
    File.Bytes = Bytes;

    std::size_t At = 0;
    while (At < Bytes.size()) {
        const void *Found = std::memchr(Bytes.data() + At, '\n', Bytes.size() - At);
        if (!Found) {
            File.Lines.push_back(Bytes.substr(At));
            File.Facts.Unterminated = true;
            break;
        }
        std::size_t End  = static_cast<std::size_t>(static_cast<const char *>(Found) - Bytes.data());
        std::size_t Stop = End;
        if (Stop > At && Bytes[Stop - 1] == '\r') {
            Stop--;
            File.Facts.CrLf++;
        } else {
            File.Facts.Lf++;
        }
        File.Lines.push_back(Bytes.substr(At, Stop - At));
        At = End + 1;
    }
    return File;
}

std::string_view DifferenceEndStyle(const TextFacts &Facts)
{
    if (Facts.CrLf > 0 && Facts.Lf > 0) return "a mix of CRLF and LF";
    if (Facts.CrLf > 0) return "CRLF";
    if (Facts.Lf > 0) return "LF";
    return "no line end at all";
}

bool DifferenceIsBinary(std::string_view Bytes)
{
    if (Bytes.empty()) return false;
    if (std::memchr(Bytes.data(), 0, Bytes.size())) return true;
    std::size_t Strays = 0;
    for (char Byte : Bytes)
        if (IsStrayControl(static_cast<std::uint8_t>(Byte))) Strays++;
    return Strays * 50 >= Bytes.size() && Strays > 0;
}

std::vector<DifferenceBlock> DifferenceBlocks(const DifferenceMarks &Marks)
{
    std::vector<DifferenceBlock> Blocks;
    std::size_t                  I = 0;
    std::size_t                  J = 0;
    while (I < Marks.TakenOut.size() || J < Marks.PutIn.size()) {
        bool Taken = I < Marks.TakenOut.size() && Marks.TakenOut[I];
        bool Put   = J < Marks.PutIn.size() && Marks.PutIn[J];
        if (!Taken && !Put) {
            I++;
            J++;
            continue;
        }
        DifferenceBlock Block;
        Block.FirstStart  = I;
        Block.SecondStart = J;
        while (I < Marks.TakenOut.size() && Marks.TakenOut[I]) I++;
        while (J < Marks.PutIn.size() && Marks.PutIn[J]) J++;
        Block.FirstCount  = I - Block.FirstStart;
        Block.SecondCount = J - Block.SecondStart;
        Blocks.push_back(Block);
    }
    return Blocks;
}

std::vector<DifferenceHunk> DifferenceHunks(const std::vector<DifferenceBlock> &Blocks, std::size_t Context)
{
    std::vector<DifferenceHunk> Hunks;
    for (std::size_t I = 0; I < Blocks.size(); I++) {
        if (!Hunks.empty() && Blocks[I].FirstStart - Blocks[I - 1].FirstEnd() <= 2 * Context) {
            Hunks.back().End = I + 1;
        } else {
            Hunks.push_back({I, I + 1});
        }
    }
    return Hunks;
}

bool DifferenceInnerSpan(std::string_view Old, std::string_view New, DifferenceSpan &InOld, DifferenceSpan &InNew)
{
    std::size_t Shortest = std::min(Old.size(), New.size());

    std::size_t Prefix = 0;
    while (Prefix < Shortest && Old[Prefix] == New[Prefix]) Prefix++;
    while (Prefix > 0 && ((Prefix < Old.size() && IsContinuation(Old[Prefix])) ||
                          (Prefix < New.size() && IsContinuation(New[Prefix]))))
        Prefix--;

    std::size_t Suffix = 0;
    while (Suffix < Shortest - Prefix && Old[Old.size() - 1 - Suffix] == New[New.size() - 1 - Suffix]) Suffix++;
    while (Suffix > 0 && (IsContinuation(Old[Old.size() - Suffix]) || IsContinuation(New[New.size() - Suffix]))) Suffix--;

    if ((Prefix + Suffix) * 3 < std::max(Old.size(), New.size())) return false;
    InOld = {Prefix, Old.size() - Suffix};
    InNew = {Prefix, New.size() - Suffix};
    return true;
}

BinaryReport DifferenceCompareBytes(std::string_view First, std::string_view Second)
{
    BinaryReport Report;
    Report.Common  = std::min(First.size(), Second.size());
    Report.FirstAt = Report.Common;
    for (std::size_t I = 0; I < Report.Common; I++) {
        if (First[I] == Second[I]) continue;
        if (Report.Differing == 0) Report.FirstAt = I;
        Report.Differing++;
    }
    return Report;
}
