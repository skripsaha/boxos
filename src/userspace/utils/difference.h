#ifndef DIFFERENCE_H
#define DIFFERENCE_H

/* difference — what is not the same in two files.
 *
 * Four parts: difference_myers.cpp is the engine — lines in, the lines taken
 * out and put in marked; difference_text.cpp is what is known of text and
 * bytes before anything is shown (lines, line ends, blocks, hunks, the part of
 * a line that changed, the binary rule); difference_print.cpp is the glass
 * and the arrow's text; difference.cpp reads the files and decides.
 *
 * Nothing in this header knows BoxOS, so the first two parts can be tried on
 * any machine.
 */

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

/* ---- The engine ---------------------------------------------------------- */

/* One flag per line: 1 where the line of the first file is taken out, or the
 * line of the second is put in. The lines not flagged are the same lines in
 * the same order in both files. */
struct DifferenceMarks {
    std::vector<std::uint8_t> TakenOut;
    std::vector<std::uint8_t> PutIn;
};

/* How many edits on each side the search for a middle point may take before
 * it settles for the furthest point it has reached. */
constexpr std::size_t DifferencePatience = 4096;

/* The fewest lines taken out and put in that turn First into Second (Myers'
 * O(ND), in linear space), for any two files that differ by fewer than
 * Patience edits in a stretch; for files that differ more the answer is a
 * good one that may not be the least. Lines are the same when their bytes are. */
DifferenceMarks DifferenceCompare(std::span<const std::string_view> First, std::span<const std::string_view> Second,
                                  std::size_t Patience = DifferencePatience);

/* ---- Text ---------------------------------------------------------------- */

/* How the lines of a file end. A line ends at "\n", and at "\r\n" when a CR
 * stands before it; a CR anywhere else is part of the line. */
struct TextFacts {
    std::size_t CrLf         = 0;
    std::size_t Lf           = 0;
    bool        Unterminated = false;   /* the last line has no end */
};

struct TextFile {
    std::string                   Label;
    std::string_view              Bytes;
    std::vector<std::string_view> Lines;   /* without their ends */
    TextFacts                     Facts;
};

TextFile DifferenceSplit(std::string Label, std::string_view Bytes);

/* "CRLF", "LF", "a mix of CRLF and LF", or "no line end at all". */
std::string_view DifferenceEndStyle(const TextFacts &Facts);

/* A file is not text when it holds a NUL byte, or when one byte in fifty or
 * more is a control that no text uses: any below 0x20 except tab, backspace,
 * line feed, form feed, carriage return and escape. Bytes of 0x80 and over do
 * not count; Latin-1 and UTF-8 are both text. */
bool DifferenceIsBinary(std::string_view Bytes);

/* A run of changed lines: Count lines of the first file from Start taken out,
 * those of the second put in, with the same lines on each side of it. */
struct DifferenceBlock {
    std::size_t FirstStart  = 0;
    std::size_t FirstCount  = 0;
    std::size_t SecondStart = 0;
    std::size_t SecondCount = 0;

    std::size_t FirstEnd() const { return FirstStart + FirstCount; }
    std::size_t SecondEnd() const { return SecondStart + SecondCount; }
};

std::vector<DifferenceBlock> DifferenceBlocks(const DifferenceMarks &Marks);

/* The blocks Begin up to End that are shown together, with the same lines
 * around them: blocks whose gap is no more than twice the context share a hunk. */
struct DifferenceHunk {
    std::size_t Begin = 0;
    std::size_t End   = 0;
};

std::vector<DifferenceHunk> DifferenceHunks(const std::vector<DifferenceBlock> &Blocks, std::size_t Context);

/* The bytes of Old and New that changed when what stands before and after
 * them is the same, never inside a UTF-8 character. False when the lines have
 * too little in common for the middle to say anything. */
struct DifferenceSpan {
    std::size_t Begin = 0;
    std::size_t End   = 0;
};

bool DifferenceInnerSpan(std::string_view Old, std::string_view New, DifferenceSpan &InOld, DifferenceSpan &InNew);

/* ---- Bytes --------------------------------------------------------------- */

struct BinaryReport {
    std::size_t   Common     = 0;   /* the length both files have */
    std::size_t   FirstAt    = 0;   /* where they first differ: Common when they never do inside it */
    std::uint64_t Differing  = 0;   /* bytes that differ inside Common */
};

BinaryReport DifferenceCompareBytes(std::string_view First, std::string_view Second);

/* ---- The glass and the arrow's text (difference_print.cpp) --------------- */

struct DifferenceTally {
    std::size_t TakenOut = 0;
    std::size_t PutIn    = 0;
    std::size_t Places   = 0;
    std::size_t Cut      = 0;   /* lines cut to the screen's width */
};

/* The hunks, and under no arrow the lines cut to the screen's width. */
DifferenceTally DifferenceShowText(const TextFile &First, const TextFile &Second, const DifferenceMarks &Marks);
void DifferenceSayTextEnd(const TextFile &First, const TextFile &Second, const DifferenceTally &Tally);

/* The same lines in the same order, differing only in how they end. */
void DifferenceSayEndsOnly(const TextFile &First, const TextFile &Second);

void DifferenceShowBytes(std::string_view FirstLabel, std::string_view FirstBytes,
                         std::string_view SecondLabel, std::string_view SecondBytes, const BinaryReport &Report);

void DifferenceSaySame(std::string_view FirstLabel, std::string_view SecondLabel, std::uint64_t Size);

#endif /* DIFFERENCE_H */
