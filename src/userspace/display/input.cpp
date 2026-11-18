/*
 * input.cpp — the keyboard's way to the things programs show. See input.h.
 */

#include "input.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

#include "areas.h"
#include "item_reception.h"
#include "painter/core/system_fonts.h"
#include "painter/gui/changes.h"
#include "typecase/text.h"

namespace {

using namespace std::chrono_literals;
namespace gui = painter::gui;
using painter::core::PointF;
using painter::core::Rect;

constexpr float PerPoint = 1.0f / ITEM_POINT;

/* The display's own area: what it says to itself goes as any program's does —
 * written by painter::gui into a ring of its own, read step by step into its
 * tree, played by its own animator. */
class Overlay {
public:
    Overlay(Rect Screen, float Scale, const items::Theme &Look)
        : Memory(sizeof(ItemAreaHeader) + RingBytes + ITEM_EVENT_BYTES, 0), State(Program)
    {
        Head             = reinterpret_cast<ItemAreaHeader *>(Memory.data());
        Head->Magic      = ITEM_AREA_MAGIC;
        Head->Bytes      = RingBytes;
        Head->EventBytes = ITEM_EVENT_BYTES;
        Program.From.Look  = &Look;
        Program.From.Scale = Scale;
        Program.From.Book  = []() -> const typecase::Fonts & {
            static const std::shared_ptr<const typecase::Fonts> System = painter::core::SystemFonts();
            static const typecase::Fonts                        None;
            return System ? *System : None;
        };
        State.Place = Screen;
        State.Shown = true;
    }

    gui::Changes Say;

    /* What was said, read as a step. */
    void Apply(std::uint64_t Now)
    {
        if (Say.Empty()) return;
        std::uint8_t *Ring  = Memory.data() + sizeof(ItemAreaHeader);
        std::uint64_t Write = Head->Write;
        (void)Say.Flush(Ring, RingBytes, Write, Head->Read, ++Steps);
        Head->Write = Write;
        items::Listener Tell;
        Tell.Animate = [this](std::uint32_t Item, const ItemAnimation &How, std::uint16_t Property,
                              std::span<const std::uint8_t> Value) { return State.Moving.Said(Item, How, Property, Value); };
        Tell.Cut     = [this](std::uint32_t Item, std::uint16_t Property) { State.Moving.Cut(Item, Property); };
        State.Moving.Reading(Now);
        (void)items::ReadSteps(*Head, std::span<const std::uint8_t>(Ring, RingBytes), State.Items, Tell);
        State.Moving.StepWhole();
    }

    items::AreaState &Area() noexcept { return State; }
    float             Scale() const noexcept { return Program.From.Scale; }

private:
    static constexpr std::uint32_t RingBytes = 64 * 1024;

    items::ProgramState       Program;
    std::vector<std::uint8_t> Memory;
    ItemAreaHeader           *Head = nullptr;
    items::AreaState          State;
    std::uint64_t             Steps = 0;
};

/* The display's own items. A label N is a group, its plate and its word. */
constexpr std::uint32_t RingOf = 1, FlashOf = 2, CaretOf = 3, LabelsFrom = 100;
constexpr std::uint32_t LabelGroup(std::size_t N) { return LabelsFrom + static_cast<std::uint32_t>(N) * 3; }

/* The letters labels are made of: under the fingers first. */
constexpr std::string_view Letters = "asdfghjklqwertyuiopzxcvbnm";

struct Label {
    std::string   Word;
    std::uint32_t Item = 0;
};

struct InputState {
    std::optional<Overlay>       Over;
    const items::Theme          *Look = nullptr;
    KeyChord                     Point{};
    bool                         Pointing = false;
    items::AreaState            *PointedIn = nullptr;
    std::string                  Typed;
    std::vector<Label>           Labels;
    std::size_t                  LabelsMade = 0;
    Rect                         RingBox{};
    bool                         RingShown = false;
    painter::core::RectF         CaretBox{};
    bool                         CaretShown = false;
} g;

bool Field(const items::Item &Of) noexcept { return Of.Kind == ITEM_TEXT && Of.Role == ITEM_ROLE_FIELD; }

/* What the keyboard reaches in Area, in the order its program made it:
 * shown, seen, to be used, on the glass within the area. */
void Gather(const items::AreaState &Area, const std::vector<std::uint32_t> &Ids, std::vector<std::uint32_t> &Into)
{
    for (std::uint32_t Id : Ids) {
        const items::Item *Of = Area.Items.Find(Id);
        if (!Of || items::HiddenOf(Of->Said) || !(Of->Seen > 0) || items::DisabledOf(Of->Said)) continue;
        if (items::FocusableOf(Of->Said, Of->Kind, Of->Role) && !Of->Box.Intersect(Area.Place).Empty()) Into.push_back(Id);
        Gather(Area, Of->Children, Into);
    }
}

std::vector<std::uint32_t> Reachable(const items::AreaState &Area)
{
    std::vector<std::uint32_t> Made;
    Gather(Area, Area.Items.Top(), Made);
    return Made;
}

/* The focus moved to Item — ITEM_AREA for none — the one it left and the one
 * it came to told; a field's caret after its last character. */
void Focus(items::AreaState &Area, std::uint32_t Item, const InputTeller &Tell)
{
    if (Area.Focused == Item) return;
    if (Area.Focused != ITEM_AREA) {
        const ItemFocus Left{0, {0, 0, 0, 0, 0, 0, 0}};
        Tell(ITEM_EVENT_FOCUS, Area.Focused, &Left, sizeof(Left));
    }
    Area.Focused = Item;
    Area.Caret   = 0;
    if (const items::Item *Of = Area.Items.Find(Item); Of && Field(*Of) && Of->Said.Text)
        Area.Caret = static_cast<std::uint32_t>(typecase::CharactersOf(*Of->Said.Text).size());
    if (Item != ITEM_AREA) {
        const ItemFocus Came{1, {0, 0, 0, 0, 0, 0, 0}};
        Tell(ITEM_EVENT_FOCUS, Item, &Came, sizeof(Came));
    }
}

/* The middle of a thing on the glass. */
PointF Middle(Rect Of) noexcept
{
    return PointF{static_cast<float>(Of.X) + static_cast<float>(Of.Width) / 2, static_cast<float>(Of.Y) + static_cast<float>(Of.Height) / 2};
}

/* The nearest thing reached the way Way points (Dx, Dy) from what has the
 * focus: the way along it counted once, the way aside it twice. */
std::uint32_t Nearest(const items::AreaState &Area, const items::Item &From, int Dx, int Dy)
{
    const PointF  Here = Middle(From.Box);
    std::uint32_t Best = ITEM_AREA;
    float         Least = 0;
    for (std::uint32_t Id : Reachable(Area)) {
        if (Id == From.Id) continue;
        const items::Item *Of = Area.Items.Find(Id);
        const PointF       There = Middle(Of->Box);
        const float        X = There.X - Here.X, Y = There.Y - Here.Y;
        const float        Along = X * static_cast<float>(Dx) + Y * static_cast<float>(Dy);
        const float        Aside = std::fabs(X * static_cast<float>(Dy) - Y * static_cast<float>(Dx));
        if (Along <= 0) continue;
        const float Cost = Along + 2 * Aside;
        if (Best == ITEM_AREA || Cost < Least) Best = Id, Least = Cost;
    }
    return Best;
}

/* ---- What the display draws for the keyboard ---- */

PointF PointsOf(float Pixels, float Pixels2) { return PointF{Pixels / g.Over->Scale(), Pixels2 / g.Over->Scale()}; }

/* A flash where a thing was pressed. */
void Flash(const items::Item &Of)
{
    if (!g.Over) return;
    gui::Changes &Say  = g.Over->Say;
    const float   Round = static_cast<float>(items::CornersOf(Of.Said, Of.Role, *g.Look)[0]) * PerPoint;
    Say.Place(FlashOf, PointsOf(static_cast<float>(Of.Box.X), static_cast<float>(Of.Box.Y)));
    Say.Size(FlashOf, PointsOf(static_cast<float>(Of.Box.Width), static_cast<float>(Of.Box.Height)));
    Say.Corners(FlashOf, painter::core::Corners::All(Round));
    Say.Opacity(FlashOf, 0.45f);
    Say.Animate(gui::Animation::Out(320ms), [&] { Say.Opacity(FlashOf, 0); });
    /* The ring gives a little, and springs back. */
    Say.Scale(RingOf, {0.94f, 0.94f});
    Say.Animate(gui::Animation::Spring(380ms, 0.4f), [&] { Say.Scale(RingOf, {1, 1}); });
}

void Press(const items::Item &Of, const InputTeller &Tell)
{
    Tell(ITEM_EVENT_PRESS, Of.Id, nullptr, 0);
    Flash(Of);
}

/* ---- A field typed into ---- */

std::string Utf8Of(const std::u32string &Characters)
{
    std::string Made;
    for (char32_t C : Characters) {
        if (C < 0x80) {
            Made += static_cast<char>(C);
        } else if (C < 0x800) {
            Made += static_cast<char>(0xC0 | (C >> 6)), Made += static_cast<char>(0x80 | (C & 0x3F));
        } else if (C < 0x10000) {
            Made += static_cast<char>(0xE0 | (C >> 12)), Made += static_cast<char>(0x80 | ((C >> 6) & 0x3F));
            Made += static_cast<char>(0x80 | (C & 0x3F));
        } else {
            Made += static_cast<char>(0xF0 | (C >> 18)), Made += static_cast<char>(0x80 | ((C >> 12) & 0x3F));
            Made += static_cast<char>(0x80 | ((C >> 6) & 0x3F)), Made += static_cast<char>(0x80 | (C & 0x3F));
        }
    }
    return Made;
}

/* The field's text said anew, as a program's change says it, and told back
 * to the program. */
void Retext(items::AreaState &Area, const items::Item &Of, const std::u32string &Characters, const InputTeller &Tell)
{
    const std::string Said = Utf8Of(Characters);
    gui::Changes      Scratch;
    Scratch.Text(Of.Id, Said);
    const std::vector<std::uint8_t> *Bytes = Scratch.Said(Of.Id, ITEM_PROP_TEXT);
    if (!Bytes) return;
    const std::uint32_t Id = Of.Id;
    Area.Moving.Cut(Id, ITEM_PROP_TEXT);
    if (Area.Items.Change(Id, ITEM_PROP_TEXT, *Bytes) != OK) return;
    std::vector<std::uint8_t> Told(sizeof(ItemBytes) + Said.size());
    const ItemBytes           Head{static_cast<std::uint32_t>(Said.size())};
    std::memcpy(Told.data(), &Head, sizeof(Head));
    std::memcpy(Told.data() + sizeof(Head), Said.data(), Said.size());
    Tell(ITEM_EVENT_TEXT, Id, Told.data(), static_cast<std::uint32_t>(Told.size()));
}

/* A key in a field: typed, the caret moved, its end, or left. True when used. */
bool FieldKey(items::AreaState &Area, const items::Item &Of, const kb_event_t &Key, const InputTeller &Tell)
{
    const bool          Extended = (Key.mods & KB_MOD_EXTENDED) != 0;
    const bool          Chorded  = (Key.mods & (KB_MOD_CTRL | KB_MOD_ALT)) != 0;
    std::u32string      Text     = Of.Said.Text ? typecase::CharactersOf(*Of.Said.Text) : std::u32string();
    const std::uint32_t Count    = static_cast<std::uint32_t>(Text.size());
    std::uint32_t      &Caret    = Area.Caret;
    Caret                        = std::min(Caret, Count);
    if (Extended) {
        switch (Key.scancode) {
        case KEY_LEFT: Caret = Caret > 0 ? Caret - 1 : 0; return true;
        case KEY_RIGHT: Caret = std::min(Caret + 1, Count); return true;
        case KEY_HOME: Caret = 0; return true;
        case KEY_END: Caret = Count; return true;
        case KEY_DELETE:
            if (Caret < Count) {
                Text.erase(Caret, 1);
                Retext(Area, Of, Text, Tell);
            }
            return true;
        default: return false;
        }
    }
    if (Chorded) return false;
    switch (Key.cp) {
    case KEY_ENTER: Press(Of, Tell); return true;
    case KEY_ESCAPE: Focus(Area, ITEM_AREA, Tell); return true;
    case KEY_TAB: return false;
    case KEY_BACKSPACE:
        if (Caret > 0) {
            Text.erase(--Caret, 1);
            Retext(Area, Of, Text, Tell);
        }
        return true;
    default:
        if (Key.cp < 0x20 || Key.cp == 0x7F || Key.cp > 0x10FFFF) return false;
        if (Utf8Of(Text).size() + 4 > ITEM_FIELD_TEXT_MOST) return true;
        Text.insert(Text.begin() + Caret++, static_cast<char32_t>(Key.cp));
        Retext(Area, Of, Text, Tell);
        return true;
    }
}

/* ---- Labels to type ---- */

/* The Nth label: a letter while there are few things, two when there are
 * more than letters. */
std::string LabelWord(std::size_t N, std::size_t Of)
{
    if (Of <= Letters.size()) return std::string(1, Letters[N]);
    return std::string{Letters[N / Letters.size() % Letters.size()], Letters[N % Letters.size()]};
}

void HideLabels()
{
    if (!g.Over) return;
    gui::Changes &Say = g.Over->Say;
    for (std::size_t N = 0; N < g.LabelsMade; N++)
        Say.Animate(gui::Animation::Out(140ms), [&] { Say.Hidden(LabelGroup(N), true); });
    g.Pointing  = false;
    g.PointedIn = nullptr;
    g.Typed.clear();
    g.Labels.clear();
}

void ShowLabels(items::AreaState &Area)
{
    const std::vector<std::uint32_t> Things = Reachable(Area);
    if (Things.empty()) return;
    g.Pointing  = true;
    g.PointedIn = &Area;
    g.Typed.clear();
    g.Labels.clear();
    for (std::size_t N = 0; N < Things.size(); N++) g.Labels.push_back(Label{LabelWord(N, Things.size()), Things[N]});
    if (!g.Over) return;
    gui::Changes &Say = g.Over->Say;
    for (std::size_t N = 0; N < g.Labels.size(); N++) {
        const std::uint32_t Group = LabelGroup(N), Plate = Group + 1, Word = Group + 2;
        if (N >= g.LabelsMade) {
            Say.Create(Group, gui::Kind::Group);
            Say.Create(Plate, gui::Kind::Shape, Group);
            Say.Rectangle(Plate);
            Say.Fills(Plate, std::vector<gui::Fill>{gui::Fill::OfTheme(gui::ThemeColor::Accent)});
            Say.Corners(Plate, painter::core::Corners::All(5));
            gui::Shadow Soft;
            Soft.Offset = PointF{0, 2};
            Soft.Blur   = 6;
            Say.Shadows(Plate, std::vector<gui::Shadow>{Soft});
            Say.Create(Word, gui::Kind::Text, Group, ITEM_TOP, gui::Role::Label);
            Say.Fills(Word, std::vector<gui::Fill>{gui::Fill::OfTheme(gui::ThemeColor::OnAccent)});
            gui::TextStyle Bold;
            Bold.Size   = 12;
            Bold.Weight = 700;
            Say.Style(Word, Bold);
            Say.Box(Word, gui::TextBox{ITEM_TEXT_CENTER, ITEM_OVERFLOW_GROW, false, 1});
            Say.Hidden(Group, true);
            g.LabelsMade = N + 1;
        }
        const items::Item *Of   = Area.Items.Find(g.Labels[N].Item);
        const float        Wide = 8 + 8 * static_cast<float>(g.Labels[N].Word.size()), High = 18;
        const PointF       At   = PointsOf(static_cast<float>(Of->Box.X), static_cast<float>(Of->Box.Y));
        Say.Place(Group, {At.X - 6, At.Y - 6});
        Say.Anchor(Group, {Wide / 2, High / 2});
        Say.Size(Plate, {Wide, High});
        Say.Text(Word, g.Labels[N].Word);
        Say.Place(Word, {0, 1.5f});
        Say.Size(Word, {Wide, gui::ByContent});
        Say.Scale(Group, {0.5f, 0.5f});
        gui::Animation Pop = gui::Animation::Spring(320ms, 0.4f);
        Pop.Delay          = std::chrono::milliseconds(static_cast<int>(std::min<std::size_t>(N, 30) * 10));
        Say.Animate(Pop, [&] {
            Say.Hidden(Group, false);
            Say.Scale(Group, {1, 1});
        });
    }
}

/* A key while the labels wait: a letter of one, a letter taken back, or
 * anything else that puts them away. */
bool LabelKey(const InputArea &To, const kb_event_t &Key)
{
    items::AreaState &Area = *To.State;
    if (g.PointedIn != &Area || (Key.mods & (KB_MOD_EXTENDED | KB_MOD_CTRL | KB_MOD_ALT)) != 0 || Key.cp == KEY_ESCAPE) {
        HideLabels();
        return true;
    }
    if (Key.cp == KEY_BACKSPACE) {
        if (!g.Typed.empty()) g.Typed.pop_back();
    } else {
        const char32_t C = Key.cp >= 'A' && Key.cp <= 'Z' ? Key.cp - 'A' + 'a' : Key.cp;
        if (C > 0x7F || Letters.find(static_cast<char>(C)) == std::string_view::npos) {
            HideLabels();
            return true;
        }
        g.Typed += static_cast<char>(C);
    }
    /* The labels the typed begins, the rest put away; one typed whole is
     * reached — and pressed if it is no field. */
    gui::Changes *Say = g.Over ? &g.Over->Say : nullptr;
    for (std::size_t N = 0; N < g.Labels.size(); N++) {
        const bool Begins = g.Labels[N].Word.compare(0, g.Typed.size(), g.Typed) == 0;
        if (Say) Say->Animate(gui::Animation::Out(120ms), [&] { Say->Hidden(LabelGroup(N), !Begins); });
        if (g.Labels[N].Word != g.Typed) continue;
        const std::uint32_t Item = g.Labels[N].Item;
        HideLabels();
        Focus(Area, Item, To.Tell);
        if (const items::Item *Of = Area.Items.Find(Item); Of && !Field(*Of)) Press(*Of, To.Tell);
        return true;
    }
    return true;
}

}  // namespace

void InputStart(Rect Screen, float Scale, bool Text, const items::Theme &Look)
{
    g.Look = &Look;
    if (Text) return;
    g.Over.emplace(Screen, Scale, Look);
    gui::Changes &Say = g.Over->Say;
    /* The ring round what is reached, in the accent, the theme's. */
    Say.Create(RingOf, gui::Kind::Shape);
    Say.Rectangle(RingOf);
    Say.Fills(RingOf, std::vector<gui::Fill>{});
    gui::Stroke Line;
    Line.Width = Look.FocusWidth;
    Line.With  = gui::Fill::OfTheme(gui::ThemeColor::Accent);
    Say.Stroke(RingOf, Line);
    Say.Opacity(RingOf, 0);
    /* A flash where a thing is pressed. */
    Say.Create(FlashOf, gui::Kind::Shape);
    Say.Rectangle(FlashOf);
    Say.Fills(FlashOf, std::vector<gui::Fill>{gui::Fill::Of(painter::core::Color::Rgb(0xFF, 0xFF, 0xFF))});
    Say.Opacity(FlashOf, 0);
    /* A field's caret, in the theme's text colour. */
    Say.Create(CaretOf, gui::Kind::Shape);
    Say.Rectangle(CaretOf);
    Say.Fills(CaretOf, std::vector<gui::Fill>{gui::Fill::OfTheme(gui::ThemeColor::Text)});
    Say.Opacity(CaretOf, 0);
}

void InputPoint(KeyChord Chord) noexcept { g.Point = Chord; }

bool InputReaches(const items::AreaState &Area) noexcept
{
    try {
        return !Reachable(Area).empty();
    } catch (...) {
        return false;
    }
}

bool InputKey(const kb_event_t &Key) noexcept
{
    try {
        const std::optional<InputArea> To = AreasKeyed();
        if (!To) {
            if (g.Pointing) HideLabels();
            return false;
        }
        items::AreaState &Area = *To->State;
        if (g.Pointing) return LabelKey(*To, Key);
        if (g.Point.Code != 0 && KeyChordMatches(&g.Point, &Key)) {
            ShowLabels(Area);
            return true;
        }
        const std::vector<std::uint32_t> Things = Reachable(Area);
        if (Area.Focused != ITEM_AREA && std::find(Things.begin(), Things.end(), Area.Focused) == Things.end())
            Focus(Area, ITEM_AREA, To->Tell);
        const items::Item *Of       = Area.Focused != ITEM_AREA ? Area.Items.Find(Area.Focused) : nullptr;
        const bool         Extended = (Key.mods & KB_MOD_EXTENDED) != 0;
        const bool         Chorded  = (Key.mods & (KB_MOD_CTRL | KB_MOD_ALT)) != 0;
        if (Of && Field(*Of) && FieldKey(Area, *Of, Key, To->Tell)) return true;
        if (!Extended && !Chorded && Key.cp == KEY_TAB && !Things.empty()) {
            /* The next in the order the program made them, or the one before. */
            const bool Back = (Key.mods & KB_MOD_SHIFT) != 0;
            auto       At   = std::find(Things.begin(), Things.end(), Area.Focused);
            std::size_t Next;
            if (At == Things.end()) Next = Back ? Things.size() - 1 : 0;
            else Next = (static_cast<std::size_t>(At - Things.begin()) + (Back ? Things.size() - 1 : 1)) % Things.size();
            Focus(Area, Things[Next], To->Tell);
            return true;
        }
        if (Extended && Of) {
            const int Dx = Key.scancode == KEY_LEFT ? -1 : Key.scancode == KEY_RIGHT ? 1 : 0;
            const int Dy = Key.scancode == KEY_UP ? -1 : Key.scancode == KEY_DOWN ? 1 : 0;
            if (Dx != 0 || Dy != 0) {
                if (const std::uint32_t Next = Nearest(Area, *Of, Dx, Dy); Next != ITEM_AREA) Focus(Area, Next, To->Tell);
                return true;
            }
        }
        if (!Extended && !Chorded && Of && !Field(*Of) && (Key.cp == KEY_ENTER || Key.cp == ' ')) {
            Press(*Of, To->Tell);
            return true;
        }
        return false;
    } catch (...) {
        return false;
    }
}

void InputFocusAsked(items::AreaState &Area, std::uint32_t Item, const InputTeller &Tell) noexcept
{
    try {
        if (Item == ITEM_AREA) {
            Focus(Area, ITEM_AREA, Tell);
            return;
        }
        /* Only what the keyboard reaches takes the focus; what is not drawn
         * yet, as it is drawn. */
        const items::Item *Of = Area.Items.Find(Item);
        if (Of && items::FocusableOf(Of->Said, Of->Kind, Of->Role) && !items::DisabledOf(Of->Said)) Focus(Area, Item, Tell);
    } catch (...) {
    }
}

void InputAreaGone(const items::AreaState &Area) noexcept
{
    if (g.PointedIn == &Area) {
        try {
            HideLabels();
        } catch (...) {
            g.Pointing  = false;
            g.PointedIn = nullptr;
        }
    }
}

void InputFrame(std::uint64_t Now) noexcept
{
    if (!g.Over) return;
    try {
        gui::Changes &Say = g.Over->Say;
        const float   Gap = g.Look->FocusGap;
        /* The ring where what is reached lies now — gliding there from where
         * it was; gone with nothing reached. */
        const std::optional<InputArea> To = AreasKeyed();
        const items::Item *Of = To && To->State->Focused != ITEM_AREA ? To->State->Items.Find(To->State->Focused) : nullptr;
        const Rect         Want = Of && !items::HiddenOf(Of->Said) ? Of->Box : Rect{};
        if (Want != g.RingBox) {
            if (Want.Empty()) {
                Say.Animate(gui::Animation::Out(160ms), [&] { Say.Opacity(RingOf, 0); });
                g.RingShown = false;
            } else {
                const float  Round = static_cast<float>(items::CornersOf(Of->Said, Of->Role, *g.Look)[0]) * PerPoint + Gap;
                const PointF At    = PointsOf(static_cast<float>(Want.X), static_cast<float>(Want.Y));
                const PointF Size  = PointsOf(static_cast<float>(Want.Width), static_cast<float>(Want.Height));
                const auto   Lay   = [&] {
                    Say.Place(RingOf, {At.X - Gap, At.Y - Gap});
                    Say.Size(RingOf, {Size.X + 2 * Gap, Size.Y + 2 * Gap});
                    Say.Anchor(RingOf, {Size.X / 2 + Gap, Size.Y / 2 + Gap});
                    Say.Corners(RingOf, painter::core::Corners::All(Round));
                };
                if (!g.RingShown) {
                    Lay();
                    Say.Animate(gui::Animation::Out(180ms), [&] { Say.Opacity(RingOf, 1); });
                    g.RingShown = true;
                } else {
                    Say.Animate(gui::Animation::Spring(300ms, 0.15f), Lay);
                }
            }
            g.RingBox = Want;
        }
        /* A field's caret where it stands, blinking anew as it moves. */
        painter::core::RectF Caret{};
        if (Of && Field(*Of) && Of->Drawn && Of->Drawn->Laid) {
            const typecase::Caret Stands = Of->Drawn->Laid->CaretAt(std::min(To->State->Caret, Of->Drawn->Laid->Count()));
            const PointF          At     = PointsOf(static_cast<float>(Of->Box.X), static_cast<float>(Of->Box.Y));
            const float           Left   = At.X + static_cast<float>(Of->Drawn->Inset.X) * PerPoint + Stands.X;
            const float           Top    = At.Y + static_cast<float>(Of->Drawn->Inset.Y) * PerPoint + Stands.Top;
            Caret = painter::core::RectF{Left - 0.75f, Top, 1.5f, std::max(Stands.Bottom - Stands.Top, 8.0f)};
        }
        if (Caret.X != g.CaretBox.X || Caret.Y != g.CaretBox.Y || Caret.Height != g.CaretBox.Height || (Caret.Width > 0) != g.CaretShown) {
            if (Caret.Width > 0) {
                Say.Place(CaretOf, {Caret.X, Caret.Y});
                Say.Size(CaretOf, {Caret.Width, Caret.Height});
                Say.Opacity(CaretOf, 1);
                gui::Animation Blink = gui::Animation::Linear(530ms);
                Blink.Curve          = gui::Animation::Ease::Steps;
                Blink.Steps          = 1;
                Blink.Repeats        = gui::Animation::Forever;
                Blink.Back           = true;
                Say.Animate(Blink, [&] { Say.Opacity(CaretOf, 0); });
            } else {
                Say.Opacity(CaretOf, 0);
            }
            g.CaretBox   = Caret;
            g.CaretShown = Caret.Width > 0;
        }
        g.Over->Apply(Now);
        items::Animator &Moving = g.Over->Area().Moving;
        if (const std::uint64_t Due = Moving.Due(true); Due != 0 && Due <= Now)
            Moving.Advance(Now, *g.Look, [](std::uint32_t, const ItemAnimationEnd &) {});
    } catch (...) {
        // No room for the display's own drawing this frame: drawn at the next.
    }
}

std::uint64_t InputDue() noexcept { return g.Over ? g.Over->Area().Moving.Due(true) : 0; }

items::AreaState *InputOverlay() noexcept { return g.Over ? &g.Over->Area() : nullptr; }
