/* speech.cpp — the bubble: where it stands, how it is typed, held and faded. */

#include "speech.h"

#include <algorithm>

#include "painter/tui/glyph.h"
#include "painter/tui/text.h"

#include "numbers.h"

namespace wizard {

namespace {

namespace tui = painter::tui;
namespace core = painter::core;

constexpr std::int32_t WidestBubble = 46;
constexpr std::int32_t LeastBubble = 22;
constexpr std::uint64_t HoldBase = 2800;
constexpr std::uint64_t HoldPerLetter = 48;
constexpr std::uint64_t FadeMs = 800;
constexpr char32_t TailLeft = U'\u25C4';    /* ◄ */
constexpr char32_t TailRight = U'\u25BA';   /* ► */

// What the bubble is made of at its fullest and at the moment: Weight 0..256.
struct Stuff {
    Color Paper, Ink, Accent;
    int   Weight;
};

// What was behind the bubble before it was drawn, a color for each cell of its box.
struct Backdrop {
    core::Rect         Area;
    std::vector<Color> Behind;

    Backdrop(const tui::Frame &Out, core::Rect Where) : Area(Where)
    {
        for (std::int32_t Row = Area.Y; Row < Area.Y + Area.Height; Row++)
            for (std::int32_t Column = Area.X; Column < Area.X + Area.Width; Column++) Behind.push_back(Out.Read({Column, Row}).Background);
    }
    Color At(std::int32_t Column, std::int32_t Row) const
    {
        return Behind[static_cast<std::size_t>(Row - Area.Y) * static_cast<std::size_t>(Area.Width) + static_cast<std::size_t>(Column - Area.X)];
    }
};

void Fill(tui::Frame &Out, const Backdrop &Behind, const Stuff &Of)
{
    const core::Rect Area = Behind.Area;
    for (std::int32_t Row = Area.Y; Row < Area.Y + Area.Height; Row++)
        for (std::int32_t Column = Area.X; Column < Area.X + Area.Width; Column++)
            Out.Put({Column, Row}, ' ', {Of.Ink, Mixed(Behind.At(Column, Row), Of.Paper, Of.Weight)});
}

void Shade(tui::Frame &Out, core::Rect Area, int Weight)
{
    for (std::int32_t Row = Area.Y; Row < Area.Y + Area.Height; Row++) {
        for (std::int32_t Column = Area.X; Column < Area.X + Area.Width; Column++) {
            const tui::Cell Here = Out.Read({Column, Row});
            Out.Put({Column, Row}, Here.Glyph, {Mixed(Here.Foreground, Color::Rgb(0, 0, 0), Weight * 110 / 256),
                                                Mixed(Here.Background, Color::Rgb(0, 0, 0), Weight * 130 / 256)});
        }
    }
}

void Frame(tui::Frame &Out, const Backdrop &Behind, const Stuff &Of)
{
    namespace G = tui::Glyphs;
    const core::Rect Area = Behind.Area;
    const auto Style = [&](std::int32_t Column, std::int32_t Row) {
        return tui::TextStyle{Mixed(Behind.At(Column, Row), Of.Ink, Of.Weight), Mixed(Behind.At(Column, Row), Of.Paper, Of.Weight)};
    };
    const std::int32_t Right = Area.X + Area.Width - 1, Bottom = Area.Y + Area.Height - 1;
    for (std::int32_t Column = Area.X + 1; Column < Right; Column++) {
        Out.Put({Column, Area.Y}, G::RuleAcross, Style(Column, Area.Y));
        Out.Put({Column, Bottom}, G::RuleAcross, Style(Column, Bottom));
    }
    for (std::int32_t Row = Area.Y + 1; Row < Bottom; Row++) {
        Out.Put({Area.X, Row}, G::RuleDown, Style(Area.X, Row));
        Out.Put({Right, Row}, G::RuleDown, Style(Right, Row));
    }
    Out.Put({Area.X, Area.Y}, G::CornerTL, Style(Area.X, Area.Y));
    Out.Put({Right, Area.Y}, G::CornerTR, Style(Right, Area.Y));
    Out.Put({Area.X, Bottom}, G::CornerBL, Style(Area.X, Bottom));
    Out.Put({Right, Bottom}, G::CornerBR, Style(Right, Bottom));
}

}  // namespace

std::vector<std::string> Wrapped(std::string_view Words, std::size_t Width)
{
    std::vector<std::string> Lines(1);
    while (!Words.empty()) {
        const std::size_t End = Words.find(' ');
        const std::string_view Word = Words.substr(0, End);
        if (!Lines.back().empty() && tui::CellsOf(Lines.back()) + 1 + tui::CellsOf(Word) > Width) Lines.emplace_back();
        if (!Lines.back().empty()) Lines.back() += ' ';
        Lines.back() += Word;
        Words = End == std::string_view::npos ? std::string_view() : Words.substr(End + 1);
    }
    return Lines;
}

void Speech::Say(std::string Heading, std::string Spoken, std::uint64_t Ms)
{
    Title = std::move(Heading);
    Words = std::move(Spoken);
    Began = Ms;
    Any = true;
}

bool Speech::Showing(std::uint64_t Ms) const
{
    if (!Any) return false;
    const std::uint64_t Whole = (Words.size() + Title.size()) * 1000u / 40u + HoldBase + HoldPerLetter * Words.size() + FadeMs;
    return Ms - Began < Whole;
}

std::size_t Speech::Typed(std::uint64_t Ms, std::uint32_t PerSecond) const
{
    return std::min<std::size_t>(Words.size(), static_cast<std::size_t>((Ms - Began) * PerSecond / 1000u));
}

float Speech::Mouth(std::uint64_t Ms, std::uint32_t PerSecond) const
{
    if (!Any || Ms < Began) return 0.0f;
    const std::size_t Done = Typed(Ms, PerSecond);
    if (Done >= Words.size()) return 0.0f;
    return Words[Done] == ' ' ? 0.1f : (Done % 3 == 0 ? 0.9f : (Done % 3 == 1 ? 0.45f : 0.7f));
}

void Speech::Paint(tui::Frame &Out, const Scene &Where, const Look &Shown, std::uint64_t Ms) const
{
    if (!Showing(Ms)) return;
    const std::uint64_t Age = Ms - Began;
    const std::uint64_t TypedAt = static_cast<std::uint64_t>(Words.size()) * 1000u / std::max<std::uint32_t>(1, Shown.TalkPerSecond);
    const std::uint64_t Ends = TypedAt + HoldBase + HoldPerLetter * Words.size();
    const int Weight = Age <= Ends ? 256 : static_cast<int>(256 - std::min<std::uint64_t>(256, (Age - Ends) * 256u / FadeMs));
    const Stuff Of{Shown.Paper, Shown.Ink, Mixed(Shown.Glow, Shown.Ink, 150), Weight};

    const core::Size Size = Out.Size();
    const std::int32_t HeadRow = static_cast<std::int32_t>(WizardPixelY(Where, 38.0f)) / 2;
    std::int32_t Left = static_cast<std::int32_t>(WizardPixelX(Where, 31.0f)) + 3;
    const bool OnRight = Size.Width - Left - 1 >= LeastBubble;
    std::int32_t Width = std::min(WidestBubble, OnRight ? Size.Width - Left - 1 : static_cast<std::int32_t>(WizardPixelX(Where, -31.0f)) - 4);
    if (!OnRight) Left = static_cast<std::int32_t>(WizardPixelX(Where, -31.0f)) - 3 - Width;
    if (Width < 12) return;

    const std::vector<std::string> Lines = Wrapped(Words, static_cast<std::size_t>(Width - 4));
    const std::int32_t Height = static_cast<std::int32_t>(Lines.size()) + (Title.empty() ? 0 : 1) + 2;
    const std::int32_t Floor = Where.ShelfTop / 2 - 1;
    const std::int32_t Top = std::clamp(HeadRow - Height / 2, BarRows, std::max(BarRows, Floor - Height));
    const core::Rect Box{Left, Top, Width, Height};

    const Backdrop Behind(Out, Box);
    const core::Point TailAt{OnRight ? Left - 1 : Left + Width, std::clamp(HeadRow, Box.Y + 1, Box.Y + Box.Height - 2)};
    const Color Tail = Out.Read(TailAt).Background;
    Shade(Out, core::Rect{Box.X + 1, Box.Y + 1, Box.Width, Box.Height}, Weight);
    Fill(Out, Behind, Of);
    Frame(Out, Behind, Of);
    Out.Put(TailAt, OnRight ? TailLeft : TailRight, {Mixed(Tail, Of.Paper, Weight), Tail});

    const auto Style = [&](std::int32_t Row, Color Ink) {
        return std::pair{Mixed(Behind.At(Box.X + 2, Row), Ink, Weight), Mixed(Behind.At(Box.X + 2, Row), Of.Paper, Weight)};
    };
    std::int32_t Row = Box.Y + 1;
    if (!Title.empty()) {
        const auto [Ink, Paper] = Style(Row, Of.Accent);
        tui::PutText(Out, {Box.X + 2, Row++}, Title, Width - 4, Ink, Paper);
    }
    std::size_t Remaining = Typed(Ms, Shown.TalkPerSecond);
    for (const std::string &Line : Lines) {
        const std::size_t Show = std::min(Remaining, Line.size());
        const auto [Ink, Paper] = Style(Row, Of.Ink);
        tui::PutText(Out, {Box.X + 2, Row}, std::string_view(Line).substr(0, Show), Width - 4, Ink, Paper);
        Remaining -= Show;
        Row++;
        if (Remaining > 0) Remaining--;
    }
}

}  // namespace wizard
