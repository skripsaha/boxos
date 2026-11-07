/* difference — what is not the same in two files.
 *
 *   difference a.txt b.txt              the lines taken out (-, red) and put in
 *                                       (+, green), each hunk with three lines
 *                                       around it and where it stands in both
 *                                       files; in a line that changed, the
 *                                       part that changed is marked stronger
 *   difference a.txt b.txt -> NAME      the same as text into NAME, as an arrow
 *                                       puts any program's text; lines are
 *                                       written whole there, not cut to the
 *                                       width of the screen
 *
 * The answer is the number the program ends with:
 *
 *   0   the files are the same (byte for byte, or the one file given twice)
 *   1   they differ, in lines, in bytes, or only in how lines end
 *   2   they could not be compared, and the reason is said
 *
 * Text is compared line by line, "\n" and "\r\n" ending a line alike, so a
 * file saved with the other line ends is not every line changed; if the lines
 * are the same and only their ends are not, that is what is said. A last line
 * with no end is said and not shown as a change. A file that is not text (see
 * DifferenceIsBinary) is compared byte by byte. Both files are read whole, a
 * piece at a time, before anything is compared.
 */

#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "box/file.h"
#include "box/form.h"
#include "box/print.h"

#include "difference.h"

BOX_FORM("takes thing \"the first file\"\n"
         "takes thing \"the second file\"\n"
         "gives text\n");

namespace {

constexpr std::size_t ReadChunk = 65536;
constexpr int         Same      = 0;
constexpr int         Differ    = 1;
constexpr int         Unable    = 2;

struct Loaded {
    file_id_t                 Id = 0;
    std::string               Label;
    std::uint64_t             Size = 0;
    std::vector<std::uint8_t> Bytes;

    std::string_view View() const { return {reinterpret_cast<const char *>(Bytes.data()), Bytes.size()}; }
};

void Say(const std::string &Text)
{
    std::string Line = Text + '\n';
    print_bytes_to_screen(Line.data(), Line.size());
}

bool Look(file_id_t Id, std::string_view Which, Loaded &File)
{
    FileMetadata Metadata;
    if (FileMetadataRead(Id, &Metadata) != 0) {
        Say("difference: could not look at the " + std::string(Which) + " file; it may have been erased");
        return false;
    }
    File.Id    = Id;
    File.Label = Metadata.Filename ? Metadata.Filename : "a file with no name";
    File.Size  = Metadata.Size;
    FileMetadataRelease(&Metadata);
    return true;
}

bool Read(Loaded &File)
{
    try {
        File.Bytes.resize(File.Size);
    } catch (const std::bad_alloc &) {
        Say("difference: there is no memory to hold " + File.Label + ", " + std::to_string(File.Size) + " bytes");
        return false;
    } catch (const std::length_error &) {
        Say("difference: there is no memory to hold " + File.Label + ", " + std::to_string(File.Size) + " bytes");
        return false;
    }

    std::uint64_t At = 0;
    while (At < File.Size) {
        std::uint64_t Want = File.Size - At > ReadChunk ? ReadChunk : File.Size - At;
        std::int64_t  Got  = fread(File.Id, At, File.Bytes.data() + At, static_cast<std::size_t>(Want));
        if (Got < 0) {
            Say("difference: " + File.Label + " would not be read at byte " + std::to_string(At));
            return false;
        }
        if (Got == 0) {
            Say("difference: " + File.Label + " was cut short while it was read; it ends at byte " + std::to_string(At));
            return false;
        }
        At += static_cast<std::uint64_t>(Got);
    }
    return true;
}

/* Two files of one name are told apart by their order. */
void NameApart(Loaded &First, Loaded &Second)
{
    if (First.Label != Second.Label) return;
    First.Label += " (the first)";
    Second.Label += " (the second)";
}

int CompareText(const Loaded &First, const Loaded &Second)
{
    TextFile Left  = DifferenceSplit(First.Label, First.View());
    TextFile Right = DifferenceSplit(Second.Label, Second.View());

    DifferenceMarks Marks = DifferenceCompare(Left.Lines, Right.Lines);
    if (DifferenceBlocks(Marks).empty()) {
        DifferenceSayEndsOnly(Left, Right);
        return Differ;
    }
    DifferenceTally Tally = DifferenceShowText(Left, Right, Marks);
    DifferenceSayTextEnd(Left, Right, Tally);
    return Differ;
}

int Compare(file_id_t FirstId, file_id_t SecondId)
{
    Loaded First, Second;
    if (!Look(FirstId, "first", First) || !Look(SecondId, "second", Second)) return Unable;
    if (FirstId == SecondId) {
        Say("difference: " + First.Label + " was given twice; a file is the same as itself");
        return Same;
    }
    NameApart(First, Second);
    if (!Read(First) || !Read(Second)) return Unable;

    if (First.Bytes == Second.Bytes) {
        DifferenceSaySame(First.Label, Second.Label, First.Size);
        return Same;
    }
    if (DifferenceIsBinary(First.View()) || DifferenceIsBinary(Second.View())) {
        DifferenceShowBytes(First.Label, First.View(), Second.Label, Second.View(),
                            DifferenceCompareBytes(First.View(), Second.View()));
        return Differ;
    }
    return CompareText(First, Second);
}

}

int main()
{
    try {
        const file_id_t *Firsts;
        const file_id_t *Seconds;
        std::size_t      FirstCount;
        std::size_t      SecondCount;
        if (form_things(0, &Firsts, &FirstCount) != 0 || form_things(1, &Seconds, &SecondCount) != 0) return Unable;
        return Compare(Firsts[0], Seconds[0]);
    } catch (const std::bad_alloc &) {
        Say("difference: there is no memory to compare these files");
        return Unable;
    }
}
