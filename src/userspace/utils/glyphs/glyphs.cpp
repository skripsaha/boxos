/* glyphs — every character the screen draws, block by block, and BoxOS's own.
 *
 *   glyphs                  Basic Latin first
 *   glyphs cyrillic         the first block whose name holds the word
 *   glyphs 1FB3C            the page of that character, the character chosen
 *   glyphs boxos            BoxOS's own characters, by their names
 *
 * A block of Unicode is shown in rows of sixteen, each row under the code it
 * begins at; the chosen character's code, its UTF-8 and its block stand
 * below. What the screen has no picture for it draws as its code in the
 * cell, so a page shows at a look what the display can show of it. BoxOS's
 * own characters are a list with their names, and all of them side by side,
 * as Duppo's two halves make one picture. Typed, a code or a word of a
 * block's name, then Enter, goes there. */

#include <algorithm>
#include <cstdint>
#include <expected>
#include <format>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>

#include "box/form.h"
#include "box/print.h"

#include "boxos_symbols.h"
#include "pages.h"
#include "painter/core/color.h"
#include "painter/tui/stage.h"

BOX_FORM("takes optional name \"where to begin: a character's code (0416, U+1FB3C) or a word of a block's name "
         "(cyrillic, braille, boxos)\"\n");

namespace {

using painter::core::Color;
namespace Colors = painter::core::Colors;
namespace Keys   = painter::tui::KeyCodes;

constexpr std::int32_t Label     = 10;   /* "U+10FFF0" and a gap */
constexpr std::int32_t Column    = 3;    /* a character and a blank each side */
constexpr std::int32_t LeastCols = Label + 16 * Column;
constexpr std::int32_t LeastRows = 8;

const painter::tui::TextStyle Plain{Color::Screen(), Color::Screen()};
const painter::tui::TextStyle Dim{Colors::Slate, Color::Screen()};
const painter::tui::TextStyle Title{Colors::Amber, Color::Screen()};
const painter::tui::TextStyle Chosen{Colors::White, Colors::Ocean};

void Say(std::string_view Text)
{
    std::string Line(Text);
    Line += '\n';
    print_bytes(Line.data(), Line.size());
}

std::string Code(char32_t Character)
{
    return std::format("U+{:04X}", static_cast<std::uint32_t>(Character));
}

std::string Utf8(char32_t Character)
{
    const std::uint32_t Value = Character;
    std::string         Bytes;
    if (Value < 0x80) Bytes = {static_cast<char>(Value)};
    else if (Value < 0x800) Bytes = {static_cast<char>(0xC0 | Value >> 6), static_cast<char>(0x80 | (Value & 0x3F))};
    else if (Value < 0x10000)
        Bytes = {static_cast<char>(0xE0 | Value >> 12), static_cast<char>(0x80 | (Value >> 6 & 0x3F)),
                 static_cast<char>(0x80 | (Value & 0x3F))};
    else
        Bytes = {static_cast<char>(0xF0 | Value >> 18), static_cast<char>(0x80 | (Value >> 12 & 0x3F)),
                 static_cast<char>(0x80 | (Value >> 6 & 0x3F)), static_cast<char>(0x80 | (Value & 0x3F))};
    std::string Said;
    for (const char Byte : Bytes) Said += std::format("{}{:02X}", Said.empty() ? "" : " ", static_cast<std::uint8_t>(Byte));
    return Said;
}

class Viewer {
public:
    explicit Viewer(painter::tui::Stage Opened) : Screen(std::move(Opened)) { Go(glyphs::Place{0, 0}); }

    void Go(glyphs::Place To)
    {
        Page   = To.Page;
        Choice = To.Character;
        Top    = 0;
    }

    void Run()
    {
        while (!Leaving) {
            Paint();
            if (!Screen.Present()) break;
            const painter::tui::Event Heard = Screen.Wait();
            if (Heard.Kind != painter::tui::EventKind::Key) break;
            Message.clear();
            OnKey(Heard.Key);
        }
        (void)Screen.Close();
    }

private:
    const glyphs::Page &Shown() const { return glyphs::Pages()[Page]; }

    std::int32_t Rows() const { return Screen.Size().Height - 3; }

    void Paint()
    {
        painter::tui::Frame &Out = Screen.FrameBuffer();
        Out.Clear(Plain);
        const glyphs::Page &On = Shown();
        Out.Write({0, 0}, std::string(On.Name), Title);
        Out.Write({static_cast<std::int32_t>(On.Name.size()) + 2, 0},
                  std::format("{}..{}   {} of {}", Code(On.First), Code(On.Last), Page + 1, glyphs::Pages().size()), Dim);
        if (On.Own) PaintOwn(Out);
        else        PaintGrid(Out);

        const std::int32_t Foot = Screen.Size().Height - 1;
        std::string        About = std::format("{}  UTF-8 {}  {}", Code(Choice), Utf8(Choice), On.Name);
        if (const std::string_view Name = glyphs::OwnName(Choice); !Name.empty()) About += std::format(": {}", Name);
        Out.Put({0, Foot - 1}, Choice, Plain);
        Out.Write({3, Foot - 1}, About, Plain);
        if (!Typed.empty()) Out.Write({0, Foot}, std::format("go to: {}_", Typed), Title);
        else if (!Message.empty()) Out.Write({0, Foot}, Message, Title);
        else Out.Write({0, Foot}, "arrows choose  PgUp PgDn blocks  type a code or a word of a name, then Enter  Esc leaves", Dim);
    }

    void PaintGrid(painter::tui::Frame &Out)
    {
        const glyphs::Page &On    = Shown();
        const std::int32_t  Lines = static_cast<std::int32_t>((On.Last - On.First) / 16 + 1);
        const std::int32_t  Row   = static_cast<std::int32_t>((Choice - On.First) / 16);
        Top = std::clamp(Top, std::max(0, Row - Rows() + 1), Row);
        for (std::int32_t Line = 0; Line < Rows() && Top + Line < Lines; Line++) {
            const char32_t Start = On.First + static_cast<char32_t>(16 * (Top + Line));
            Out.Write({0, 1 + Line}, Code(Start), Dim);
            for (std::int32_t Index = 0; Index < 16; Index++) {
                const char32_t Each = Start + static_cast<char32_t>(Index);
                const auto     Look = Each == Choice ? Chosen : Plain;
                const std::int32_t X = Label + Index * Column;
                Out.Fill({X, 1 + Line, Column, 1}, U' ', Look);
                Out.Put({X + 1, 1 + Line}, Each, Look);
            }
        }
    }

    void PaintOwn(painter::tui::Frame &Out)
    {
        std::int32_t Line = 1;
        for (const BoxSymbol &Each : BoxSymbols) {
            if (Line > Rows()) break;
            const auto Look = Each.Character == Choice ? Chosen : Plain;
            Out.Write({0, Line}, Code(Each.Character), Dim);
            Out.Fill({Label, Line, Column, 1}, U' ', Look);
            Out.Put({Label + 1, Line}, Each.Character, Look);
            Out.Write({Label + Column + 1, Line}, Each.Name, Plain);
            Line++;
        }
        if (Line + 1 > Rows()) return;
        Out.Write({0, Line + 1}, "together", Dim);
        std::int32_t X = Label + 1;
        for (const BoxSymbol &Each : BoxSymbols) Out.Put({X++, Line + 1}, Each.Character, Plain);
    }

    void Step(std::int32_t By)
    {
        const glyphs::Page &On = Shown();
        if (!On.Own) {
            const std::int64_t To = static_cast<std::int64_t>(Choice) + By;
            Choice = static_cast<char32_t>(std::clamp<std::int64_t>(To, On.First, On.Last));
            return;
        }
        std::size_t At = 0;
        while (At < std::size(BoxSymbols) && BoxSymbols[At].Character != Choice) At++;
        const std::int64_t To = std::clamp<std::int64_t>(static_cast<std::int64_t>(At) + (By > 0 ? 1 : -1), 0,
                                                         static_cast<std::int64_t>(std::size(BoxSymbols)) - 1);
        Choice = BoxSymbols[To].Character;
    }

    void Turn(std::int64_t By)
    {
        const std::int64_t Count = static_cast<std::int64_t>(glyphs::Pages().size());
        const std::size_t  To    = static_cast<std::size_t>(((static_cast<std::int64_t>(Page) + By) % Count + Count) % Count);
        Go(glyphs::Place{To, glyphs::Pages()[To].First});
    }

    void OnKey(const painter::tui::KeyEvent &Key)
    {
        if (Key.IsExtended()) {
            switch (Key.Scancode) {
            case Keys::Left:     Step(-1); break;
            case Keys::Right:    Step(1); break;
            case Keys::Up:       Step(-16); break;
            case Keys::Down:     Step(16); break;
            case Keys::PageUp:   Turn(-1); break;
            case Keys::PageDown: Turn(1); break;
            case Keys::Home:     Choice = Shown().First; break;
            case Keys::End:      Choice = Shown().Own ? std::end(BoxSymbols)[-1].Character : Shown().Last; break;
            default: break;
            }
            return;
        }
        if (Key.Character == Keys::Escape) {
            if (Typed.empty()) Leaving = true;
            Typed.clear();
            return;
        }
        if (Key.Character == Keys::Backspace) {
            while (!Typed.empty() && (static_cast<unsigned char>(Typed.back()) & 0xC0) == 0x80) Typed.pop_back();
            if (!Typed.empty()) Typed.pop_back();
            return;
        }
        if (Key.Character == Keys::Enter) {
            if (const std::optional<glyphs::Place> To = glyphs::Find(Typed, Page)) Go(*To);
            else if (!Typed.empty()) Message = std::format("no character and no block is called {}", Typed);
            Typed.clear();
            return;
        }
        if (Key.IsPrintable() && !Key.IsCtrl() && Typed.size() < 40) Typed += Key.Spelling();
    }

    painter::tui::Stage Screen;
    std::size_t         Page   = 0;
    char32_t            Choice = 0;
    std::int32_t        Top    = 0;
    std::string         Typed;
    std::string         Message;
    bool                Leaving = false;
};

}  // namespace

int main()
{
    try {
        const char *const *Words;
        std::size_t        Count;
        if (form_words(0, &Words, &Count) != 0) return 1;
        std::optional<glyphs::Place> First;
        if (Count > 0) {
            First = glyphs::Find(Words[0], glyphs::Pages().size() - 1);
            if (!First) {
                Say(std::format("glyphs: no character and no block is called {}", Words[0]));
                return 1;
            }
        }
        std::expected<painter::tui::Stage, painter::tui::StageError> Opened =
            painter::tui::Stage::Open(LeastCols, LeastRows);
        if (!Opened) {
            Say(Opened.error().Describe("glyphs"));
            return 1;
        }
        Viewer Shown(std::move(*Opened));
        if (First) Shown.Go(*First);
        Shown.Run();
        return 0;
    } catch (...) {
        Say("glyphs: no room to show the characters");
        return 1;
    }
}
