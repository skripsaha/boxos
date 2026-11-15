#ifndef STRINGVIEW_H
#define STRINGVIEW_H

/* stringview — the text in any file, on the glass, narrowed as it is typed.
 *
 * Three parts: stringview_sift.cpp is the sieve — bytes in, texts out;
 * stringview.cpp decides — the Form, the sifting, the keys, the filter, and the
 * text an arrow takes instead of a frame; stringview_paint.cpp is the glass.
 */

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "box/file.h"
#include "painter/core/color.h"
#include "painter/tui/stage.h"

/* A text found: where it begins in the file, whether it was written in
 * UTF-16, and its characters as UTF-8. */
struct StringviewText {
    std::uint64_t Offset = 0;
    bool          Wide   = false;
    std::string   Text;
};

/*
 * Text is found in two spellings at once:
 *
 *   narrow  UTF-8, which ASCII is part of: printable ASCII, the tab, and every
 *           well-formed sequence of a character that is seen when written;
 *   wide    UTF-16 little-endian, as UEFI and Windows write it: code units
 *           0x0020-0x007e, the tab and 0x00a0-0x00ff, at even offsets and at
 *           odd ones.
 *
 * Wide text is held to the first 256 characters: a unit of any other page is
 * two bytes of anything, and UTF-8 or a table of numbers read two bytes at a
 * time would be found as text. Held so, a wide text has a zero every other
 * byte and a narrow text none, so no two texts of two characters or more are
 * ever open at once, and each is given as it ends, in the order they began.
 */
class Sifter {
public:
    using Found = std::function<void(StringviewText &&)>;

    Sifter(std::uint64_t Shortest, Found OnText) : Shortest(Shortest), OnText(std::move(OnText)) {}

    /* The file's bytes, from offset 0 on, a piece at a time. */
    void Take(std::uint64_t At, const std::uint8_t *Bytes, std::size_t Count);
    /* The file ended: what is open ends with it. */
    void Finish();

private:
    struct Run {
        std::uint64_t Start  = 0;
        std::uint64_t Length = 0;      /* in characters */
        std::string   Text;
        bool          Open   = false;
    };

    void Put(Run &Into, std::uint64_t At, const std::uint8_t *Bytes, std::size_t Count);
    void End(Run &Ending, bool Wide);
    void Begin(std::uint8_t Lead, std::uint64_t At, unsigned Need, std::uint8_t Low, std::uint8_t High);
    void Narrow(std::uint8_t Byte, std::uint64_t At);
    void WideUnit(std::uint8_t Low, std::uint8_t High, std::uint64_t At);

    std::uint64_t Shortest;
    Found         OnText;

    Run           NarrowRun;
    std::uint8_t  Sequence[4] = {};    /* a UTF-8 sequence under way */
    unsigned      SequenceUsed = 0;
    unsigned      SequenceNeed = 0;
    std::uint8_t  NextLow = 0;         /* the bounds of its next byte */
    std::uint8_t  NextHigh = 0;
    std::uint32_t Point = 0;
    std::uint64_t SequenceAt = 0;

    Run           WideRuns[2];         /* units at even offsets, at odd ones */
    std::uint8_t  Previous = 0;
};

class Stringview {
public:
    Stringview(file_id_t File, std::string Name, std::string Tags, std::uint64_t Size, std::uint64_t Shortest);

    std::optional<std::string> TakeScreen();
    void Run();
    void GiveScreenBack();
    std::string Summary() const;

private:
    /* stringview.cpp */
    bool SiftStep();
    void Add(StringviewText &&Text);
    bool Matches(const StringviewText &Text) const;
    void Refilter();
    void Narrow();
    void OnKey(const painter::tui::KeyEvent &Key);
    void Scroll(std::int64_t Lines);

    /* stringview_paint.cpp */
    bool          Paint();
    void          PaintTitle();
    void          PaintList();
    void          PaintFoot();
    void          Put(std::uint32_t Row, std::uint32_t Col, char32_t Ch, painter::core::Color Fg,
                      painter::core::Color Bg);
    std::uint32_t Write(std::uint32_t Row, std::uint32_t Col, std::string_view Text, painter::core::Color Fg,
                        painter::core::Color Bg);
    std::uint32_t ListRows() const { return Rows - 2; }

    file_id_t     File;
    std::string   Name;
    std::string   Tags;
    std::uint64_t Size;
    std::uint64_t Shortest;
    unsigned      Digits;

    Sifter                      Sieve;
    std::vector<std::uint8_t>   Chunk;
    std::uint64_t               SiftedTo  = 0;
    bool                        Sifting   = true;
    std::string                 Trouble;          /* why the sifting stopped short */

    std::vector<StringviewText> Texts;
    std::vector<std::size_t>    Shown;            /* the texts the filter keeps */
    std::string                 Filter;
    std::size_t                 Top  = 0;         /* the first of Shown drawn */
    std::uint32_t               Left = 0;         /* the first character drawn */

    std::uint32_t         Rows = 0;
    std::uint32_t         Cols = 0;
    std::optional<painter::tui::Stage> Screen;
    bool                  Leaving = false;
};

/* Where Filter first stands in Text, big and small letters alike; npos when
 * it does not. */
std::size_t StringviewFind(std::string_view Text, std::string_view Filter);

/* Under an arrow: every text as a line of text, for the file. False when the
 * file would not be read to its end. */
bool StringviewList(file_id_t File, const FileMetadata &Metadata, std::uint64_t Shortest);

#endif /* STRINGVIEW_H */
