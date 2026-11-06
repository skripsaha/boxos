/* todo_view.cpp — the list as it looks (todo_desk.h).
 *
 * A header with how far along the view is, the things a row each with their
 * context's tags as coloured chips, a picture when there is nothing to do,
 * the flash of a thing just done, the cheer when the last one is, and the
 * keys along the bottom, as many as the width holds, the most needed first. */

#include <algorithm>
#include <format>

#include "box/clock.h"

#include "todo_desk.h"

namespace {

using Color = painter::core::Color;
namespace Colors = painter::core::Colors;
using painter::tui::Frame;
namespace Glyphs = painter::tui::Glyphs;
using painter::tui::TextStyle;

const Color Paper       = Color::Rgb(0x12, 0x12, 0x12);
const Color BarPaper    = Color::Rgb(0x3A, 0x3A, 0x3A);
const Color ChosenPaper = Color::Rgb(0x2E, 0x2E, 0x2E);
const Color FlashPaper  = Color::Rgb(0x1E, 0x5A, 0x2A);
const Color RuleInk     = Color::Rgb(0x3A, 0x3A, 0x3A);
const Color TitleInk    = painter::core::Colors::Amber;
const Color TitleText   = Color::Rgb(0x1A, 0x1A, 0x1A);
const Color ViewInk     = Color::Rgb(0x60, 0xA0, 0xE0);
const Color TextInk     = Color::Rgb(0xEE, 0xEE, 0xEE);
const Color DoneInk     = Color::Rgb(0x6E, 0x6E, 0x6E);
const Color TickInk     = Color::Rgb(0x60, 0xE0, 0x70);
const Color StarInk     = painter::core::Colors::Amber;
const Color HintInk     = Color::Rgb(0x8C, 0x8C, 0x8C);
const Color KeyInk      = painter::core::Colors::Amber;
const Color BadInk      = Color::Rgb(0xFF, 0x70, 0x70);
const Color GaugeLeft   = Color::Rgb(0x4A, 0x4A, 0x4A);

/* A tag's colour, the same for it every time: one of six light hues, each a
 * family of its own on a sixteen-colour console too. */
const Color TagInks[] = {Color::Rgb(0x60, 0xA0, 0xF0), Color::Rgb(0x70, 0xD0, 0x80), Color::Rgb(0x60, 0xD0, 0xD0),
                         Color::Rgb(0xD0, 0x80, 0xE0), Color::Rgb(0xF0, 0x80, 0x80), Color::Rgb(0xE0, 0xD0, 0x60)};

constexpr char StarGlyph   = static_cast<char>(0x0F);
constexpr char FilledGlyph = static_cast<char>(0xFE);
constexpr char RuleGlyph   = static_cast<char>(0xC4);
constexpr int  GaugeWidth  = 12;
constexpr int  FirstRow    = 2;
constexpr std::uint64_t SparkUs = 600000;

Color TagInk(std::string_view Tag)
{
    std::uint32_t Hash = 2166136261u;
    for (char Ch : Tag) Hash = (Hash ^ static_cast<unsigned char>(Ch)) * 16777619u;
    return TagInks[Hash % (sizeof(TagInks) / sizeof(TagInks[0]))];
}

/* "code,cpp" as it is said: code cpp. */
std::string Spoken(std::string_view Context)
{
    std::string Out;
    for (std::string_view Tag : TodoTags(Context)) Out += (Out.empty() ? "" : " ") + std::string(Tag);
    return Out;
}

int ChipsWidth(const std::vector<std::string_view> &Tags)
{
    int Wide = 0;
    for (std::string_view Tag : Tags) Wide += static_cast<int>(Tag.size()) + 2 + (Wide ? 2 : 0);
    return Wide;
}

}

/* The title, what is in view, and how much of it is done. */
void TodoDesk::Head(const std::vector<std::size_t> &Places)
{
    Frame &FrameBuffer = Glass->FrameBuffer();
    const painter::core::Size Dimensions = Glass->Size();
    int Col = 1 + static_cast<int>(FrameBuffer.Write({1, 0}, " todo ", TextStyle{TitleText, TitleInk}));
    std::string View = List.Everywhere ? "everywhere" : Context.empty() ? "everything" : "for " + Spoken(Context);
    Col += 2;
    Col += static_cast<int>(FrameBuffer.Write({Col, 0}, View, TextStyle{ViewInk, Paper}));

    std::size_t Done = static_cast<std::size_t>(
        std::count_if(Places.begin(), Places.end(), [&](std::size_t At) { return List.Items[At].Done; }));
    if (!Places.empty() && Col + 4 + GaugeWidth + 16 < Dimensions.Width) {
        int Lit = static_cast<int>(Done * GaugeWidth / Places.size());
        Col += 4;
        Col += static_cast<int>(FrameBuffer.Write({Col, 0}, std::string(static_cast<std::size_t>(Lit), FilledGlyph),
                                                   TextStyle{TickInk, Paper}));
        Col += static_cast<int>(FrameBuffer.Write({Col, 0}, std::string(static_cast<std::size_t>(GaugeWidth - Lit), Glyphs::Dot),
                                                   TextStyle{GaugeLeft, Paper}));
        FrameBuffer.Write({Col + 1, 0}, std::format("{} of {} done", Done, Places.size()), TextStyle{HintInk, Paper});
    }
    FrameBuffer.Fill({0, 1, Dimensions.Width, 1}, RuleGlyph, TextStyle{RuleInk, Paper});
}

/* One thing on its row: the box, the star, the words, the context's chips. */
void TodoDesk::Thing(int Row, const TodoItem &Item, std::size_t At, bool Chosen)
{
    Frame &FrameBuffer = Glass->FrameBuffer();
    const painter::core::Size Dimensions = Glass->Size();
    Color         Back  = Chosen ? ChosenPaper : Paper;
    std::uint64_t Since = clock_uptime_us() - FlashAt;
    if (At == Flashed && Since < TodoFlashUs)
        Back = Color::Mix(FlashPaper, Back, static_cast<std::uint8_t>(Since * 255 / TodoFlashUs));

    FrameBuffer.Fill({0, Row, Dimensions.Width, 1}, ' ', TextStyle{TextInk, Back});
    if (Chosen) FrameBuffer.Put({1, Row}, Glyphs::Pointer, TextStyle{TitleInk, Back});
    FrameBuffer.Write({3, Row}, "[ ]", TextStyle{Item.Done ? DoneInk : HintInk, Back});
    if (Item.Done) FrameBuffer.Put({4, Row}, Glyphs::Tick, TextStyle{TickInk, Back});
    if (Item.Star) FrameBuffer.Put({7, Row}, StarGlyph, TextStyle{Item.Done ? DoneInk : StarInk, Back});

    std::vector<std::string_view> Tags = TodoTags(Item.Context);
    int Chips = ChipsWidth(Tags);
    if (Chips > Dimensions.Width / 3) Chips = 0;
    int         Room = Dimensions.Width - 9 - (Chips ? Chips + 3 : 1);
    std::string Text = Item.Text;
    if (static_cast<int>(Text.size()) > Room) Text = Text.substr(0, static_cast<std::size_t>(std::max(0, Room - 3))) + "...";
    FrameBuffer.Write({9, Row}, Text, TextStyle{Item.Done ? DoneInk : TextInk, Back});
    if (!Chips) return;

    int Col = Dimensions.Width - 1 - Chips;
    for (std::string_view Tag : Tags) {
        Color Ink = Item.Done ? Color::Mix(TagInk(Tag), Back, 150) : TagInk(Tag);
        FrameBuffer.Put({Col, Row}, Glyphs::Bullet, TextStyle{Ink, Back});
        Col += 2;
        Col += static_cast<int>(FrameBuffer.Write({Col, Row}, Tag, TextStyle{Ink, Back}));
        Col += 2;
    }
}

/* A box ticked, and what to do about there being nothing. */
void TodoDesk::Empty()
{
    Frame &FrameBuffer = Glass->FrameBuffer();
    const painter::core::Size Dimensions = Glass->Size();
    int Row = Dimensions.Height / 2 - 2, Col = Dimensions.Width / 2 - 14;
    static const char Box[3][5] = {{'\xDA', '\xC4', '\xC4', '\xC4', '\xBF'},
                                   {'\xB3', ' ', '\xFB', ' ', '\xB3'},
                                   {'\xC0', '\xC4', '\xC4', '\xC4', '\xD9'}};
    for (int R = 0; R < 3; R++)
        for (int C = 0; C < 5; C++)
            FrameBuffer.Put({Col + C, Row + R}, Box[R][C], TextStyle{R == 1 && C == 2 ? TickInk : HintInk, Paper});
    FrameBuffer.Write({Col + 8, Row}, "nothing to do here", TextStyle{TextInk, Paper});
    FrameBuffer.Write({Col + 8, Row + 1}, "a adds a thing", TextStyle{HintInk, Paper});
    std::size_t Elsewhere = List.Items.size();
    if (!List.Everywhere && !Context.empty() && Elsewhere > 0)
        FrameBuffer.Write({Col + 8, Row + 2}, std::format("tab shows all {}", Elsewhere), TextStyle{HintInk, Paper});
}

/* The cheer: a banner, and sparks that grow and fade where they fall. */
void TodoDesk::Sparks()
{
    Frame &FrameBuffer = Glass->FrameBuffer();
    const painter::core::Size Dimensions = Glass->Size();
    std::uint64_t Since = clock_uptime_us() - CheerAt;
    if (CheerAt == 0 || Since >= TodoCheerUs) return;
    static const char Phases[] = {static_cast<char>(0xFA), '+', '*', StarGlyph, '*', '+', static_cast<char>(0xFA)};
    for (const TodoSpark &One : Confetti) {
        if (Since < One.Born || Since - One.Born >= SparkUs) continue;
        if (One.Row >= Dimensions.Height - 1 || One.Col >= Dimensions.Width) continue;
        std::size_t Phase = static_cast<std::size_t>((Since - One.Born) * sizeof(Phases) / SparkUs);
        painter::tui::Cell Cell = FrameBuffer.Read({One.Col, One.Row});
        if (Cell.Glyph != ' ') continue;
        Cell.Glyph = Phases[Phase];
        Cell.Foreground = One.Ink;
        FrameBuffer.Put({One.Col, One.Row}, Cell);
    }
    std::string Banner = std::format("  {} all done here  ", Glyphs::Tick);
    int         Row    = Dimensions.Height / 2;
    int         Col    = static_cast<int>(FrameBuffer.Centre(Row, Banner, TextStyle{Colors::White, FlashPaper}));
    FrameBuffer.Fill({Col, Row - 1, static_cast<int>(Banner.size()), 1}, ' ', TextStyle{Colors::White, FlashPaper});
    FrameBuffer.Fill({Col, Row + 1, static_cast<int>(Banner.size()), 1}, ' ', TextStyle{Colors::White, FlashPaper});
}

/* The words being typed, a note, or the keys: as many as fit, chosen by how
 * much they are needed and laid in their own order. */
void TodoDesk::Bar()
{
    Frame &FrameBuffer = Glass->FrameBuffer();
    const painter::core::Size Dimensions = Glass->Size();
    int Row = Dimensions.Height - 1;
    FrameBuffer.Fill({0, Row, Dimensions.Width, 1}, ' ', TextStyle{TextInk, BarPaper});
    if (Now != TodoMode::Browse) {
        int Col  = 1 + static_cast<int>(FrameBuffer.Write({1, Row}, Now == TodoMode::Adding ? "to do: " : "change: ",
                                                           TextStyle{KeyInk, BarPaper}));
        int Room = Dimensions.Width - Col - 1;
        std::size_t From = Caret > static_cast<std::size_t>(Room) ? Caret - static_cast<std::size_t>(Room) : 0;
        FrameBuffer.Write({Col, Row}, std::string_view(Line).substr(From, static_cast<std::size_t>(Room)),
                          TextStyle{TextInk, BarPaper});
        FrameBuffer.Put({Col + static_cast<int>(Caret - From), Row}, Caret < Line.size() ? Line[Caret] : ' ',
                        TextStyle{BarPaper, TextInk});
        return;
    }
    if (!Note.empty()) {
        FrameBuffer.Write({1, Row}, Note, TextStyle{BadInk, BarPaper});
        return;
    }
    struct Hint { const char *Keys, *What; int Need; };
    const Hint Hints[] = {{"\x18\x19", "choose", 7}, {"space", "done", 0}, {"s", "star", 4},
                          {"a", "add", 1},          {"enter", "change", 5}, {"d", "away", 6},
                          {"shift+\x18\x19", "move", 8}, {"tab", List.Everywhere ? "here" : "everywhere", 2},
                          {"q", "leave", 3}};
    auto Width = [](const Hint &One) { return static_cast<int>(std::string_view(One.Keys).size() + std::string_view(One.What).size()) + 3; };
    int  Most  = -1, Used = 1;
    for (int Need = 0; Need <= 8; Need++)
        for (const Hint &One : Hints)
            if (One.Need == Need && Used + Width(One) <= Dimensions.Width && Most == Need - 1) {
                Used += Width(One);
                Most = Need;
            }
    int Col = 1;
    for (const Hint &One : Hints) {
        if (One.Need > Most) continue;
        Col += static_cast<int>(FrameBuffer.Write({Col, Row}, One.Keys, TextStyle{KeyInk, BarPaper}));
        Col += 1;
        Col += static_cast<int>(FrameBuffer.Write({Col, Row}, One.What, TextStyle{HintInk, BarPaper}));
        Col += 2;
    }
}

void TodoDesk::Draw()
{
    Glass->FrameBuffer().Clear(TextStyle{Paper, Paper});
    std::vector<std::size_t> Places = List.View(Here());
    if (!Places.empty()) Selected = std::min(Selected, Places.size() - 1);
    Head(Places);

    const painter::core::Size Dimensions = Glass->Size();
    int Room = Dimensions.Height - 1 - FirstRow;
    if (Places.empty()) {
        Empty();
    } else {
        if (Selected < Scroll) Scroll = Selected;
        if (Selected >= Scroll + static_cast<std::size_t>(Room)) Scroll = Selected - static_cast<std::size_t>(Room) + 1;
    }
    int Shown = 0;
    for (; Shown < Room && Scroll + static_cast<std::size_t>(Shown) < Places.size(); Shown++) {
        std::size_t Index = Scroll + static_cast<std::size_t>(Shown);
        Thing(FirstRow + Shown, List.Items[Places[Index]], Places[Index], Index == Selected);
    }
    bool AllDone = !Places.empty() &&
                   std::all_of(Places.begin(), Places.end(), [&](std::size_t At) { return List.Items[At].Done; });
    if (AllDone && Shown + 1 < Room)
        Glass->FrameBuffer().Centre(FirstRow + Shown + 1, "all done here, enjoy the quiet", TextStyle{TickInk, Paper});
    Sparks();
    Bar();
}
