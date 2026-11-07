/* stringview — the text in any file, on the glass, narrowed as it is typed.
 *
 *   stringview FILE           every text of 4 characters or more, on the screen
 *   stringview FILE 8         of 8 or more
 *   stringview FILE -> NAME   the texts as lines into NAME, as an arrow puts any
 *                             program's text
 *
 *   typing                    keeps only the texts that hold what is typed,
 *                             big and small letters alike
 *   Backspace                 takes the last letter back
 *   arrows, PgUp PgDn, Home End   move through the list, and along long texts
 *   ESC                       clears what is typed; with nothing typed, leaves
 *   ^Q                        leaves
 *
 * The file is sifted a piece at a time between keys, so the first texts are on
 * the glass before the last are found and the list can be narrowed while it
 * grows. Nothing may print between the first paint and the screen given back.
 * The sieve is stringview_sift.cpp, the glass stringview_paint.cpp.
 */

#include <format>
#include <new>
#include <string>
#include <string_view>

#include "box/file.h"
#include "box/form.h"
#include "box/print.h"

#include "file_heading.h"
#include "stringview.h"

BOX_FORM("takes thing \"the file to look into\"\n"
         "takes optional words \"the fewest characters a text has\"\n"
         "gives things\n");

namespace {

constexpr std::uint64_t ShortestUsual = 4;
constexpr std::size_t   SiftChunk     = 65536;
/* Pieces sifted between two frames: often enough that the list is seen to
 * grow, seldom enough that drawing it is not most of the work. */
constexpr unsigned      PiecesAFrame  = 8;

void Say(std::string_view Text)
{
    std::string Line(Text);
    Line += '\n';
    print_to_screen(Line.c_str());
}

std::optional<std::string> ReadShortest(const char *const *Words, std::size_t Count, std::uint64_t &Shortest)
{
    Shortest = ShortestUsual;
    if (Count == 0) return std::nullopt;
    if (Count > 1) return std::string("stringview takes one number, the fewest characters a text has: stringview say.elf 8");

    std::string_view Word  = Words[0];
    std::uint64_t    Value = 0;
    for (char Ch : Word) {
        if (Ch < '0' || Ch > '9' || Value > (UINT64_MAX - 9) / 10)
            return std::format("'{}' is no file here, nor a number of characters: stringview say.elf 8", Word);
        Value = Value * 10 + static_cast<std::uint64_t>(Ch - '0');
    }
    if (Value < 2) return std::string("a text is 2 characters or more: one character is any printable byte");
    Shortest = Value;
    return std::nullopt;
}

char Lower(char Ch)
{
    return Ch >= 'A' && Ch <= 'Z' ? static_cast<char>(Ch - 'A' + 'a') : Ch;
}

}

std::size_t StringviewFind(std::string_view Text, std::string_view Filter)
{
    if (Filter.empty()) return 0;
    for (std::size_t I = 0; I + Filter.size() <= Text.size(); I++) {
        std::size_t J = 0;
        while (J < Filter.size() && Lower(Text[I + J]) == Lower(Filter[J])) J++;
        if (J == Filter.size()) return I;
    }
    return std::string_view::npos;
}

Stringview::Stringview(file_id_t File, std::string Name, std::string Tags, std::uint64_t Size, std::uint64_t Shortest)
    : File(File), Name(std::move(Name)), Tags(std::move(Tags)), Size(Size), Shortest(Shortest),
      Digits(OffsetDigits(Size)), Sieve(Shortest, [this](StringviewText &&Text) { Add(std::move(Text)); }),
      Chunk(SiftChunk), Sifting(Size > 0)
{
}

std::optional<std::string> Stringview::TakeScreen()
{
    std::expected<painter::tui::Stage, painter::tui::StageError> Opened =
        painter::tui::Stage::Open("stringview", static_cast<std::int32_t>(Digits + 8), 3);
    if (!Opened) return Opened.error().Describe("stringview");
    Screen.emplace(std::move(*Opened));
    Rows = static_cast<std::uint32_t>(Screen->Size().Height);
    Cols = static_cast<std::uint32_t>(Screen->Size().Width);
    return std::nullopt;
}

/* One piece of the file through the sieve. False once the file is sifted. */
bool Stringview::SiftStep()
{
    std::uint64_t Want = Size - SiftedTo > SiftChunk ? SiftChunk : Size - SiftedTo;
    std::int64_t  Got  = fread(File, SiftedTo, Chunk.data(), static_cast<std::size_t>(Want));
    if (Got > 0) {
        Sieve.Take(SiftedTo, Chunk.data(), static_cast<std::size_t>(Got));
        SiftedTo += static_cast<std::uint64_t>(Got);
        if (SiftedTo < Size) return true;
    } else if (Got < 0) {
        Trouble = std::format("the file would not be read at 0x{:x}; what is shown is what came before", SiftedTo);
    } else {
        Trouble = std::format("the file was cut short while it was read: it ends at 0x{:x}", SiftedTo);
    }
    Sieve.Finish();
    return false;
}

void Stringview::Add(StringviewText &&Text)
{
    Texts.push_back(std::move(Text));
    if (Matches(Texts.back())) Shown.push_back(Texts.size() - 1);
}

bool Stringview::Matches(const StringviewText &Text) const
{
    return StringviewFind(Text.Text, Filter) != std::string_view::npos;
}

/* Every text again: what was typed was taken back or cleared. */
void Stringview::Refilter()
{
    Shown.clear();
    for (std::size_t I = 0; I < Texts.size(); I++)
        if (Matches(Texts[I])) Shown.push_back(I);
    Top = 0;
}

/* A letter more can only keep fewer: only those kept are asked again. */
void Stringview::Narrow()
{
    std::size_t Kept = 0;
    for (std::size_t Index : Shown)
        if (Matches(Texts[Index])) Shown[Kept++] = Index;
    Shown.resize(Kept);
    Top = 0;
}

void Stringview::Scroll(std::int64_t Lines)
{
    std::size_t Visible = ListRows();
    std::size_t Last    = Shown.size() > Visible ? Shown.size() - Visible : 0;
    if (Lines < 0) Top = static_cast<std::size_t>(-Lines) > Top ? 0 : Top - static_cast<std::size_t>(-Lines);
    else           Top = Top + static_cast<std::size_t>(Lines) > Last ? Last : Top + static_cast<std::size_t>(Lines);
}

void Stringview::OnKey(const painter::tui::KeyEvent &Key)
{
    if (Key.IsCtrl()) {
        if (Key.Ascii == 'q' || Key.Ascii == 'Q') Leaving = true;
        return;
    }

    std::int64_t Page = ListRows();
    if (Key.IsExtended()) {
        switch (Key.Scancode) {
        case painter::tui::KeyCodes::Up:      Scroll(-1); break;
        case painter::tui::KeyCodes::Down:    Scroll(1); break;
        case painter::tui::KeyCodes::PageUp:  Scroll(-Page); break;
        case painter::tui::KeyCodes::PageDown: Scroll(Page); break;
        case painter::tui::KeyCodes::Home:    Top = 0; Left = 0; break;
        case painter::tui::KeyCodes::End:     Scroll(static_cast<std::int64_t>(Shown.size())); break;
        case painter::tui::KeyCodes::Left:    Left = Left > 8 ? Left - 8 : 0; break;
        case painter::tui::KeyCodes::Right:   Left += 8; break;
        default: break;
        }
        return;
    }

    char Ch = Key.Ascii;
    if (Ch == painter::tui::KeyCodes::Escape) {
        if (Filter.empty()) {
            Leaving = true;
        } else {
            Filter.clear();
            Refilter();
        }
        return;
    }
    if (Ch == painter::tui::KeyCodes::Backspace || Ch == 0x7f) {
        if (Filter.empty()) return;
        Filter.pop_back();
        Refilter();
        return;
    }
    /* What is typed is shown in the foot, after "find ". */
    if (Ch >= 0x20 && Ch < 0x7f && Filter.size() + 7 < Cols) {
        Filter += Ch;
        Narrow();
    }
}

void Stringview::Run()
{
    while (!Leaving) {
        if (!Paint()) break;
        if (Sifting) {
            for (unsigned Piece = 0; Piece < PiecesAFrame && Sifting; Piece++) Sifting = SiftStep();
            for (;;) {
                painter::tui::Event Event = Screen->Poll();
                if (Event.Kind == painter::tui::EventKind::Quiet) break;
                if (Event.Kind == painter::tui::EventKind::Gone) {
                    Leaving = true;
                    break;
                }
                OnKey(Event.Key);
            }
            continue;
        }

        /* Sifted: nothing changes between keys, and a refusal means there is
         * nothing left to hear. */
        painter::tui::Event Event = Screen->Wait();
        if (Event.Kind != painter::tui::EventKind::Key) break;
        OnKey(Event.Key);
    }
    GiveScreenBack();
}

void Stringview::GiveScreenBack()
{
    if (Screen) {
        (void)Screen->Close();
        Screen.reset();
    }
}

std::string Stringview::Summary() const
{
    std::string Line = std::format("stringview: {} - {} text{}", Name, Texts.size(), Texts.size() == 1 ? "" : "s");
    if (Sifting) Line += std::format(", left at 0x{:x} of {} bytes", SiftedTo, Size);
    if (!Trouble.empty()) Line += "; " + Trouble;
    return Line;
}

bool StringviewList(file_id_t File, const FileMetadata &Metadata, std::uint64_t Shortest)
{
    std::string Heading = FileHeading(Metadata);
    print_bytes(Heading.data(), Heading.size());

    unsigned    Digits = OffsetDigits(Metadata.Size);
    std::size_t Found  = 0;
    Sifter      Sieve(Shortest, [&](StringviewText &&Text) {
        std::string Line = std::format("{:0{}x}  {}{}\n", Text.Offset, Digits, Text.Wide ? "u16  " : "     ", Text.Text);
        print_bytes(Line.data(), Line.size());
        Found++;
    });

    std::vector<std::uint8_t> Chunk(SiftChunk);
    std::uint64_t             At  = 0;
    std::int64_t              Got = 1;
    while (At < Metadata.Size) {
        std::uint64_t Want = Metadata.Size - At > SiftChunk ? SiftChunk : Metadata.Size - At;
        Got = fread(File, At, Chunk.data(), static_cast<std::size_t>(Want));
        if (Got <= 0) break;
        Sieve.Take(At, Chunk.data(), static_cast<std::size_t>(Got));
        At += static_cast<std::uint64_t>(Got);
    }
    Sieve.Finish();

    if (Found == 0) println(std::format("no text of {} characters or more", Shortest).c_str());
    if (Got < 0) {
        Say(std::format("stringview: the file would not be read at 0x{:x}", At));
        return false;
    }
    if (At < Metadata.Size) Say(std::format("stringview: the file was cut short while it was read: it ends at 0x{:x}", At));
    return true;
}

int main()
{
    try {
        const file_id_t   *Files;
        std::size_t        FileCount;
        const char *const *Words;
        std::size_t        WordCount;
        if (form_things(0, &Files, &FileCount) != 0 || form_words(1, &Words, &WordCount) != 0) return 1;

        std::uint64_t Shortest = ShortestUsual;
        if (std::optional<std::string> Refusal = ReadShortest(Words, WordCount, Shortest)) {
            Say(*Refusal);
            return 1;
        }

        file_id_t    File = Files[0];
        FileMetadata Metadata;
        if (FileMetadataRead(File, &Metadata) != 0) {
            Say("stringview: could not look at that file; it may have been erased");
            return 1;
        }

        /* Under an arrow the result is text for the file, not a frame. */
        if (form_screen_file() != 0) {
            bool Listed = StringviewList(File, Metadata, Shortest);
            FileMetadataRelease(&Metadata);
            return Listed ? 0 : 1;
        }

        Stringview Viewer(File, Metadata.Filename ? Metadata.Filename : "a file with no name",
                          Metadata.Tags ? Metadata.Tags : "", Metadata.Size, Shortest);
        FileMetadataRelease(&Metadata);
        if (std::optional<std::string> Refused = Viewer.TakeScreen()) {
            Say(*Refused);
            return 1;
        }
        Viewer.Run();
        Say(Viewer.Summary());
        souvenir_things(&File, 1);
        return 0;
    } catch (const std::bad_alloc &) {
        Say("stringview: there is no memory for this file's texts");
        return 1;
    }
}
