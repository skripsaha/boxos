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

const Color Paper       = COLOR_RGB(0x12, 0x12, 0x12);
const Color BarPaper    = COLOR_RGB(0x3A, 0x3A, 0x3A);
const Color ChosenPaper = COLOR_RGB(0x2E, 0x2E, 0x2E);
const Color FlashPaper  = COLOR_RGB(0x1E, 0x5A, 0x2A);
const Color RuleInk     = COLOR_RGB(0x3A, 0x3A, 0x3A);
const Color TitleInk    = COLOR_AMBER;
const Color TitleText   = COLOR_RGB(0x1A, 0x1A, 0x1A);
const Color ViewInk     = COLOR_RGB(0x60, 0xA0, 0xE0);
const Color TextInk     = COLOR_RGB(0xEE, 0xEE, 0xEE);
const Color DoneInk     = COLOR_RGB(0x6E, 0x6E, 0x6E);
const Color TickInk     = COLOR_RGB(0x60, 0xE0, 0x70);
const Color StarInk     = COLOR_AMBER;
const Color HintInk     = COLOR_RGB(0x8C, 0x8C, 0x8C);
const Color KeyInk      = COLOR_AMBER;
const Color BadInk      = COLOR_RGB(0xFF, 0x70, 0x70);
const Color GaugeLeft   = COLOR_RGB(0x4A, 0x4A, 0x4A);

/* A tag's colour, the same for it every time: one of six light hues, each a
 * family of its own on a sixteen-colour console too. */
const Color TagInks[] = {COLOR_RGB(0x60, 0xA0, 0xF0), COLOR_RGB(0x70, 0xD0, 0x80), COLOR_RGB(0x60, 0xD0, 0xD0),
                         COLOR_RGB(0xD0, 0x80, 0xE0), COLOR_RGB(0xF0, 0x80, 0x80), COLOR_RGB(0xE0, 0xD0, 0x60)};

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
    int Col = Glass.Write(0, 1, " todo ", TitleText, TitleInk);
    std::string View = List.Everywhere ? "everywhere" : Context.empty() ? "everything" : "for " + Spoken(Context);
    Col = Glass.Write(0, Col + 2, View, ViewInk, Paper);

    std::size_t Done = static_cast<std::size_t>(
        std::count_if(Places.begin(), Places.end(), [&](std::size_t At) { return List.Items[At].Done; }));
    if (!Places.empty() && Col + 4 + GaugeWidth + 16 < Glass.Cols) {
        int Lit = static_cast<int>(Done * GaugeWidth / Places.size());
        Col = Glass.Write(0, Col + 4, std::string(static_cast<std::size_t>(Lit), FilledGlyph), TickInk, Paper);
        Col = Glass.Write(0, Col, std::string(static_cast<std::size_t>(GaugeWidth - Lit), Glyph::Dot), GaugeLeft, Paper);
        Glass.Write(0, Col + 1, std::format("{} of {} done", Done, Places.size()), HintInk, Paper);
    }
    Glass.Fill(1, 0, Glass.Cols, RuleGlyph, RuleInk, Paper);
}

/* One thing on its row: the box, the star, the words, the context's chips. */
void TodoDesk::Thing(int Row, const TodoItem &Item, std::size_t At, bool Chosen)
{
    Color         Back  = Chosen ? ChosenPaper : Paper;
    std::uint64_t Since = clock_uptime_us() - FlashAt;
    if (At == Flashed && Since < TodoFlashUs)
        Back = color_mix(FlashPaper, Back, static_cast<std::uint8_t>(Since * 255 / TodoFlashUs));

    Glass.Fill(Row, 0, Glass.Cols, ' ', TextInk, Back);
    if (Chosen) Glass.Put(Row, 1, Glyph::Pointer, TitleInk, Back);
    Glass.Write(Row, 3, "[ ]", Item.Done ? DoneInk : HintInk, Back);
    if (Item.Done) Glass.Put(Row, 4, Glyph::Tick, TickInk, Back);
    if (Item.Star) Glass.Put(Row, 7, StarGlyph, Item.Done ? DoneInk : StarInk, Back);

    std::vector<std::string_view> Tags = TodoTags(Item.Context);
    int Chips = ChipsWidth(Tags);
    if (Chips > Glass.Cols / 3) Chips = 0;
    int         Room = Glass.Cols - 9 - (Chips ? Chips + 3 : 1);
    std::string Text = Item.Text;
    if (static_cast<int>(Text.size()) > Room) Text = Text.substr(0, static_cast<std::size_t>(std::max(0, Room - 3))) + "...";
    Glass.Write(Row, 9, Text, Item.Done ? DoneInk : TextInk, Back);
    if (!Chips) return;

    int Col = Glass.Cols - 1 - Chips;
    for (std::string_view Tag : Tags) {
        Color Ink = Item.Done ? color_mix(TagInk(Tag), Back, 150) : TagInk(Tag);
        Glass.Put(Row, Col, Glyph::Bullet, Ink, Back);
        Col = Glass.Write(Row, Col + 2, Tag, Ink, Back) + 2;
    }
}

/* A box ticked, and what to do about there being nothing. */
void TodoDesk::Empty()
{
    int Row = Glass.Rows / 2 - 2, Col = Glass.Cols / 2 - 14;
    static const char Box[3][5] = {{'\xDA', '\xC4', '\xC4', '\xC4', '\xBF'},
                                   {'\xB3', ' ', '\xFB', ' ', '\xB3'},
                                   {'\xC0', '\xC4', '\xC4', '\xC4', '\xD9'}};
    for (int R = 0; R < 3; R++)
        for (int C = 0; C < 5; C++) Glass.Put(Row + R, Col + C, Box[R][C], R == 1 && C == 2 ? TickInk : HintInk, Paper);
    Glass.Write(Row, Col + 8, "nothing to do here", TextInk, Paper);
    Glass.Write(Row + 1, Col + 8, "a adds a thing", HintInk, Paper);
    std::size_t Elsewhere = List.Items.size();
    if (!List.Everywhere && !Context.empty() && Elsewhere > 0)
        Glass.Write(Row + 2, Col + 8, std::format("tab shows all {}", Elsewhere), HintInk, Paper);
}

/* The cheer: a banner, and sparks that grow and fade where they fall. */
void TodoDesk::Sparks()
{
    std::uint64_t Since = clock_uptime_us() - CheerAt;
    if (CheerAt == 0 || Since >= TodoCheerUs) return;
    static const char Phases[] = {static_cast<char>(0xFA), '+', '*', StarGlyph, '*', '+', static_cast<char>(0xFA)};
    for (const TodoSpark &One : Confetti) {
        if (Since < One.Born || Since - One.Born >= SparkUs) continue;
        if (One.Row >= Glass.Rows - 1 || One.Col >= Glass.Cols) continue;
        std::size_t Phase = static_cast<std::size_t>((Since - One.Born) * sizeof(Phases) / SparkUs);
        TextCell   &Cell  = Glass.At(One.Row, One.Col);
        if (Cell.ch != ' ') continue;
        Cell.ch = Phases[Phase];
        Cell.fg = One.Ink;
    }
    std::string Banner = std::format("  {} all done here  ", Glyph::Tick);
    int         Row    = Glass.Rows / 2;
    int         Col    = Glass.Centre(Row, Banner, COLOR_WHITE, FlashPaper);
    Glass.Fill(Row - 1, Col, static_cast<int>(Banner.size()), ' ', COLOR_WHITE, FlashPaper);
    Glass.Fill(Row + 1, Col, static_cast<int>(Banner.size()), ' ', COLOR_WHITE, FlashPaper);
}

/* The words being typed, a note, or the keys: as many as fit, chosen by how
 * much they are needed and laid in their own order. */
void TodoDesk::Bar()
{
    int Row = Glass.Rows - 1;
    Glass.Fill(Row, 0, Glass.Cols, ' ', TextInk, BarPaper);
    if (Now != TodoMode::Browse) {
        int Col  = Glass.Write(Row, 1, Now == TodoMode::Adding ? "to do: " : "change: ", KeyInk, BarPaper);
        int Room = Glass.Cols - Col - 1;
        std::size_t From = Caret > static_cast<std::size_t>(Room) ? Caret - static_cast<std::size_t>(Room) : 0;
        Glass.Write(Row, Col, std::string_view(Line).substr(From, static_cast<std::size_t>(Room)), TextInk, BarPaper);
        Glass.Put(Row, Col + static_cast<int>(Caret - From), Caret < Line.size() ? Line[Caret] : ' ', BarPaper, TextInk);
        return;
    }
    if (!Note.empty()) {
        Glass.Write(Row, 1, Note, BadInk, BarPaper);
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
            if (One.Need == Need && Used + Width(One) <= Glass.Cols && Most == Need - 1) {
                Used += Width(One);
                Most = Need;
            }
    int Col = 1;
    for (const Hint &One : Hints) {
        if (One.Need > Most) continue;
        Col = Glass.Write(Row, Col, One.Keys, KeyInk, BarPaper);
        Col = Glass.Write(Row, Col + 1, One.What, HintInk, BarPaper) + 2;
    }
}

void TodoDesk::Draw()
{
    Glass.Clear(Paper);
    std::vector<std::size_t> Places = List.View(Here());
    if (!Places.empty()) Selected = std::min(Selected, Places.size() - 1);
    Head(Places);

    int Room = Glass.Rows - 1 - FirstRow;
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
    if (AllDone && Shown + 1 < Room) Glass.Centre(FirstRow + Shown + 1, "all done here, enjoy the quiet", TickInk, Paper);
    Sparks();
    Bar();
}
