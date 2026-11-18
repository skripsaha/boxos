/*
 * frame.cpp — what the glass shows, put together only where it changed. See
 * frame.h.
 */

#include "frame.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

#include "typecase/text.h"
#include "typesetter.h"

namespace {

using painter::core::Rect;

/* A text page's cells as pixels, for an area's place: a point is a pixel of
 * an eighth of a cell across and a sixteenth down. */
constexpr std::int32_t PageCellWide = 8, PageCellTall = 16;

/* A side of the canvas a frame composes on, tile by tile. */
constexpr std::int32_t TileSide = 256;

/* The console's one item in its area. */
constexpr std::uint32_t ConsoleItem = 1;

items::Theme                         g_theme;
CellsLook                            g_cells;
Rect                                 g_screen{};
bool                                 g_text = false;
std::shared_ptr<CellGrid>            g_console;
items::ProgramState                  g_own;            // the display's: what the console is drawn from
std::unique_ptr<items::AreaState>    g_console_area;
std::optional<items::Compositor>     g_compositor;
std::optional<painter::core::Canvas> g_tile;
std::vector<items::AreaState *>      g_order;
std::vector<ItemCell>                g_composed;     // a text page's cells
bool                                 g_everything = true;

/* Where the glass is to be composed again, said before a frame — where an
 * area went: the glass as it was before the console moved under it. */
std::vector<Rect> g_before;

/* What the mirror shows of each of the console's cells, as last drawn —
 * Unseen where that is not known: where it lies under something, or was
 * drawn only in part — and where the console's caret was drawn. */
std::vector<ItemCell> g_seen;
std::uint64_t         g_seen_caret = ~std::uint64_t{0};
constexpr ItemCell    Unseen{0xFFFFFFFFu, 0, 0, 0, 0xFF, {0, 0, 0}};

/* The covers kept still over the console when it scrolls, at most so many:
 * past them, what lies over it moves with it and is drawn again. */
constexpr std::size_t KeptMost = 32;

/* The caret the glass shows: whose grid, in which area, by which item. */
std::weak_ptr<CellGrid> g_holder;
items::AreaState       *g_holder_area = nullptr;
std::uint32_t           g_holder_item = 0;

ItemCell Blank() noexcept { return ItemCell{U' ', COLOR_DEFAULT, COLOR_DEFAULT, 0, ITEM_MARK_NONE, {0, 0, 0}}; }

/* The topmost cells item, in Order from the top, whose caret is shown: the
 * one whose caret the glass shows — with its area — or the console's. Drawn
 * is how the items are found: in the compositor's list on a framebuffer, by
 * walking each tree on a text page. */
struct Holding {
    std::shared_ptr<CellGrid> Grid;
    items::AreaState         *Area = nullptr;
    std::uint32_t             Item = 0;
};

const items::Item *TopCaret(const items::Tree &Items, const std::vector<std::uint32_t> &Level)
{
    for (auto It = Level.rbegin(); It != Level.rend(); ++It) {
        const items::Item *Of = Items.Find(*It);
        if (!Of || items::HiddenOf(Of->Said)) continue;
        if (const items::Item *Inner = TopCaret(Items, Of->Children)) return Inner;
        if (Of->Kind == ITEM_CELLS && Of->Grid && Of->Grid->CaretShown()) return Of;
    }
    return nullptr;
}

Holding Holder(std::span<items::AreaState *const> Programs)
{
    for (auto Area = Programs.rbegin(); Area != Programs.rend(); ++Area) {
        items::AreaState &Each = **Area;
        if (!Each.Shown) continue;
        if (!g_text) {
            for (auto E = Each.List.rbegin(); E != Each.List.rend(); ++E)
                if (E->What == items::Entry::Kind::Draw && E->Of->Kind == ITEM_CELLS && E->Of->Grid && E->Of->Grid->CaretShown())
                    return Holding{E->Of->Grid, &Each, E->Of->Id};
        } else if (const items::Item *Of = TopCaret(Each.Items, Each.Items.Top())) {
            return Holding{Of->Grid, &Each, Of->Id};
        }
    }
    return Holding{g_console, g_console_area.get(), ConsoleItem};
}

/* The caret given to the grid that now holds it, and taken from the one that
 * did — the cells each leaves and comes to marked. True when it changed. */
bool HoldCaret(std::span<items::AreaState *const> Programs)
{
    const Holding                   Now = Holder(Programs);
    const std::shared_ptr<CellGrid> Was = g_holder.lock();
    if (Was == Now.Grid) return false;
    if (Was) {
        Was->HoldCaret(false);
        g_holder_area->Items.CellsChanged(g_holder_item);
    }
    Now.Grid->HoldCaret(true);
    Now.Area->Items.CellsChanged(Now.Item);
    g_holder      = Now.Grid;
    g_holder_area = Now.Area;
    g_holder_item = Now.Item;
    return true;
}

/* Where what an area drew lies on the glass, told to Each: every item's
 * pixels as the compositor last laid them — of cells, only those that show
 * something, and the caret the glass shows, for a cell that shows nothing
 * shows what lies beneath it. */
template <typename Telling> void Drawn(const items::AreaState &Area, Telling &&Each)
{
    for (const items::Entry &One : Area.List) {
        if (One.What != items::Entry::Kind::Draw) continue;
        const items::Item &Of = *One.Of;
        if (Of.Kind != ITEM_CELLS || !Of.Grid || !items::Upright(One.Place, Area.Program.From.Scale)) {
            Each(One.Device);
            continue;
        }
        const painter::core::PointF Corner = One.Place.Apply({0, 0});
        const auto                  X      = static_cast<std::int32_t>(std::floor(Corner.X + 0.5f));
        const auto                  Y      = static_cast<std::int32_t>(std::floor(Corner.Y + 0.5f));
        const auto                  Pixels = [&](Rect Cells) {
            return Rect{X + Cells.X * g_cells.Width, Y + Cells.Y * g_cells.Height, Cells.Width * g_cells.Width,
                        Cells.Height * g_cells.Height}
                .Intersect(One.Device);
        };
        if (const Rect Cells = Of.Grid->Showing(); !Cells.Empty()) Each(Pixels(Cells));
        if (Of.Grid->Holds() && Of.Grid->CaretShown()) {
            const ItemCaret Caret = Of.Grid->Caret();
            Each(Pixels(Rect{static_cast<std::int32_t>(Caret.Col), static_cast<std::int32_t>(Caret.Row), 1, 1}));
        }
    }
}

bool SameCell(const ItemCell &A, const ItemCell &B) noexcept { return std::memcmp(&A, &B, sizeof(ItemCell)) == 0; }

/* The console's cells within Pixels of the glass: Each(Index, Whole) for
 * every cell that touches it, Whole when it lies in it entirely. */
template <typename Telling> void CellsIn(Rect Pixels, Telling &&Each)
{
    const Rect In = Pixels.Intersect(g_cells.Grid);
    if (In.Empty()) return;
    const std::int32_t W = g_cells.Width, H = g_cells.Height, X0 = g_cells.Grid.X, Y0 = g_cells.Grid.Y;
    const std::int32_t FirstCol = (In.X - X0) / W, LastCol = (In.X + In.Width - 1 - X0) / W;
    const std::int32_t FirstRow = (In.Y - Y0) / H, LastRow = (In.Y + In.Height - 1 - Y0) / H;
    for (std::int32_t Row = FirstRow; Row <= LastRow && Row < static_cast<std::int32_t>(g_console->Rows()); Row++)
        for (std::int32_t Col = FirstCol; Col <= LastCol && Col < static_cast<std::int32_t>(g_console->Cols()); Col++) {
            const Rect Cell{X0 + Col * W, Y0 + Row * H, W, H};
            const bool Whole = Cell.Intersect(Pixels) == Cell;
            Each(static_cast<std::size_t>(Row) * g_console->Cols() + static_cast<std::size_t>(Col), Whole);
        }
}

/* The least rectangle of Tile holding all the areas draw there — cells that
 * show nothing draw nothing — and the page of an area of the whole screen:
 * the console alone shows round it. */
Rect Lying(Rect Tile, std::span<items::AreaState *const> Programs) noexcept
{
    Rect       Lies{};
    const auto Add = [&](Rect Where) {
        Where = Where.Intersect(Tile);
        if (!Where.Empty()) Lies = Lies.Empty() ? Where : Lies.Join(Where);
    };
    for (const items::AreaState *Each : Programs) {
        if (!Each->Shown) continue;
        if (Each->Paged) Add(Each->Place);
        Drawn(*Each, Add);
    }
    return Lies;
}

/* Tile as the console alone shows it, written into the mirror as triples —
 * the page round its cells, and its cells (cells_draw.h LayCells), each
 * covering its pixels whole (console_model.h): the very pixels composing it
 * would put there, the canvas and its light passed by. */
void LayConsole(Mirror &Into, Rect Tile)
{
    const Rect          Grid = g_cells.Grid;
    const std::uint32_t Page = g_cells.Page;
    Into.Paint(Tile, [&](std::uint32_t *Pixels, std::size_t Stride, Rect Area) {
        const std::int32_t Left  = std::clamp(Grid.X, Area.X, Area.X + Area.Width);
        const std::int32_t Right = std::clamp(Grid.X + Grid.Width, Area.X, Area.X + Area.Width);
        for (std::int32_t Y = Area.Y; Y < Area.Y + Area.Height; Y++) {
            std::uint32_t *Line   = Pixels + static_cast<std::size_t>(Y - Area.Y) * Stride - Area.X;
            const bool     Across = Y < Grid.Y || Y >= Grid.Y + Grid.Height;
            std::fill(Line + Area.X, Line + (Across ? Area.X + Area.Width : Left), Page);
            if (!Across) std::fill(Line + Right, Line + Area.X + Area.Width, Page);
        }
        LayCells(*g_console, g_cells, Pixels, Stride, Area, painter::core::Point{Grid.X, Grid.Y});
        if (g_seen.size() == g_console->Count())
            CellsIn(Area, [](std::size_t Index, bool Whole) { g_seen[Index] = Whole ? g_console->At(Index) : Unseen; });
    });
}

/* What the areas lay opaque over the console — their items that cover
 * their rectangles whole (compositor.h Entry::Hides), as last composed:
 * what lies still when the console scrolls under it. An area that moves —
 * one in the console's flow, going up with it — lies still nowhere: it is
 * composed again where it was and where it is. */
void Covers(std::span<items::AreaState *const> Programs, std::vector<Rect> &Into)
{
    Into.clear();
    for (const items::AreaState *Each : Programs) {
        if (!Each->WasShown || !Each->Shown || Each->Place != Each->Was) continue;
        for (const items::Entry &One : Each->List) {
            const Rect Still = One.Hides.Intersect(g_cells.Grid);
            if (One.What != items::Entry::Kind::Draw || Still.Empty() || Into.size() == KeptMost) continue;
            Into.push_back(Still);
        }
    }
}

bool Within(Rect Inner, std::span<const Rect> Outers) noexcept
{
    for (const Rect &Outer : Outers)
        if (Inner.Intersect(Outer) == Inner) return true;
    return false;
}

/* What the console changed, onto the mirror: of each changed stretch only
 * the cells that differ from what the mirror shows of them — and the caret's
 * cells, where it was drawn and where it stands now — none of those under a
 * cover kept still, which are not seen; where nothing an area draws lies
 * over them, written straight, each run as it is; under what an area draws,
 * left to the compositor to compose with it. */
void ConsoleChanged(Mirror &Into, std::span<items::AreaState *const> Programs, std::span<const Rect> Still)
{
    const std::uint32_t Cols  = g_console->Cols();
    const auto          Caret = [&]() -> std::uint64_t {
        if (!g_console->Holds() || !g_console->CaretShown()) return ~std::uint64_t{0};
        return static_cast<std::uint64_t>(g_console->Caret().Row) * Cols + g_console->Caret().Col;
    }();
    const auto Damage = [&](std::uint32_t Row, std::uint32_t From, std::uint32_t To) {
        Rect Run{g_cells.Grid.X + static_cast<std::int32_t>(From) * g_cells.Width, g_cells.Grid.Y + static_cast<std::int32_t>(Row) * g_cells.Height,
                 static_cast<std::int32_t>(To - From) * g_cells.Width, g_cells.Height};
        /* What of it lies under a cover kept still is not seen: its cells
         * not known to the mirror, the rest composed. */
        std::vector<Rect> Parts{Run}, Next;
        for (const Rect &Cover : Still) {
            Next.clear();
            for (const Rect &Part : Parts) {
                const Rect Hid = Part.Intersect(Cover);
                if (Hid.Empty()) {
                    Next.push_back(Part);
                    continue;
                }
                CellsIn(Hid, [](std::size_t Index, bool) { g_seen[Index] = Unseen; });
                const std::int32_t R = Part.X + Part.Width, B = Part.Y + Part.Height;
                for (const Rect &Piece : {Rect{Part.X, Part.Y, Part.Width, Hid.Y - Part.Y}, Rect{Part.X, Hid.Y + Hid.Height, Part.Width, B - Hid.Y - Hid.Height},
                                          Rect{Part.X, Hid.Y, Hid.X - Part.X, Hid.Height}, Rect{Hid.X + Hid.Width, Hid.Y, R - Hid.X - Hid.Width, Hid.Height}})
                    if (!Piece.Empty()) Next.push_back(Piece);
            }
            Parts.swap(Next);
        }
        for (const Rect &Part : Parts) {
            if (Lying(Part, Programs).Empty()) LayConsole(Into, Part);
            else g_compositor->Damage(Part);
        }
    };
    g_console->TakeMarks([&](std::uint32_t Row, std::uint32_t From, std::uint32_t To) {
        std::uint32_t Begun = To;
        for (std::uint32_t Col = From; Col < To; Col++) {
            const std::size_t Index  = static_cast<std::size_t>(Row) * Cols + Col;
            const bool        Differs = !SameCell(g_seen[Index], g_console->At(Col, Row)) || Index == Caret || Index == g_seen_caret;
            if (Differs && Begun == To) Begun = Col;
            if (!Differs && Begun != To) {
                Damage(Row, Begun, Col);
                Begun = To;
            }
        }
        if (Begun != To) Damage(Row, Begun, To);
    });
    g_seen_caret = Caret;
}

/* ---- A text page --------------------------------------------------------- */

/* Characters written into a text page's cells from a cell on, up to the
 * page's edge, in Ink. */
void Write(std::vector<ItemCell> &Cells, std::uint32_t Cols, std::uint32_t Rows, std::int32_t Col, std::int32_t Row,
           std::u32string_view Characters, std::uint32_t Ink) noexcept
{
    if (Row < 0 || Row >= static_cast<std::int32_t>(Rows)) return;
    for (std::size_t I = 0; I < Characters.size(); I++) {
        const std::int32_t At = Col + static_cast<std::int32_t>(I);
        if (At < 0 || At >= static_cast<std::int32_t>(Cols)) continue;
        ItemCell &Into = Cells[static_cast<std::size_t>(Row) * Cols + static_cast<std::size_t>(At)];
        Into.Character = Characters[I];
        Into.Ink       = Ink;
    }
}

/* A text item's ink on a text page: its first fill's colour. Throws
 * std::bad_alloc only. */
std::uint32_t InkOf(const items::Item &Of)
{
    const std::vector<items::Fill> Fills = items::FillsOf(Of.Said, Of.Kind, Of.Role);
    if (Fills.empty()) return COLOR_DEFAULT;
    const painter::core::Color C =
        Fills[0].Kind == ITEM_FILL_THEME ? items::ThemeColor(g_theme, Fills[0].Theme) : items::ColorOf(Fills[0].Color);
    return static_cast<std::uint32_t>(C.Red()) << 16 | static_cast<std::uint32_t>(C.Green()) << 8 | C.Blue();
}

/* One area's items into the page's cells: a page of the whole screen's
 * cleared first; then each item where it lies — cells laid over the cells
 * beneath, a text written there, a secret as stars, and each other item but
 * a group by its caption. Where a cells item lies is found as the frame
 * finds it on pixels: its place, or the console's grid; the caret of Caret's
 * cells, where it stands on the page, into CaretX, CaretY. Throws
 * std::bad_alloc only. */
void CellsOf(const items::AreaState &Area, std::vector<ItemCell> &Cells, std::uint32_t Cols, std::uint32_t Rows,
             const CellGrid *Caret, std::uint32_t &CaretX, std::uint32_t &CaretY)
{
    const Rect Place = Area.Place;
    if (Area.Paged) {
        const std::int32_t Right = std::min<std::int32_t>((Place.X + Place.Width) / PageCellWide, static_cast<std::int32_t>(Cols));
        const std::int32_t Foot  = std::min<std::int32_t>((Place.Y + Place.Height) / PageCellTall, static_cast<std::int32_t>(Rows));
        for (std::int32_t Row = std::max(Place.Y / PageCellTall, 0); Row < Foot; Row++)
            for (std::int32_t Col = std::max(Place.X / PageCellWide, 0); Col < Right; Col++)
                Cells[static_cast<std::size_t>(Row) * Cols + static_cast<std::size_t>(Col)] = Blank();
    }
    /* Every item, with where its groups put it, in points. */
    std::vector<std::tuple<std::uint32_t, std::int64_t, std::int64_t>> Walk;
    for (auto It = Area.Items.Top().rbegin(); It != Area.Items.Top().rend(); ++It) Walk.emplace_back(*It, 0, 0);
    while (!Walk.empty()) {
        const auto [Id, X0, Y0] = Walk.back();
        Walk.pop_back();
        const items::Item *Of = Area.Items.Find(Id);
        if (!Of || items::HiddenOf(Of->Said)) continue;
        const ItemPoint    Own = Of->Said.Place ? *Of->Said.Place : ItemPoint{0, 0};
        const std::int64_t X = X0 + Own.X, Y = Y0 + Own.Y;
        for (auto Child = Of->Children.rbegin(); Child != Of->Children.rend(); ++Child) Walk.emplace_back(*Child, X, Y);
        const auto Col = static_cast<std::int32_t>((Place.X + X / ITEM_POINT) / PageCellWide);
        const auto Row = static_cast<std::int32_t>((Place.Y + Y / ITEM_POINT) / PageCellTall);
        if (Of->Kind == ITEM_CELLS && Of->Grid) {
            painter::core::Point At{static_cast<std::int32_t>(Place.X + X / ITEM_POINT), static_cast<std::int32_t>(Place.Y + Y / ITEM_POINT)};
            if (!Of->Said.Place) At = OnTheGrid(g_cells, At);
            const std::int32_t PageCol = At.X / PageCellWide, PageRow = At.Y / PageCellTall;
            PageCells(*Of->Grid, PageCol, PageRow, Cells, Cols, Rows, g_cells.Page);
            if (Of->Grid.get() == Caret) {
                CaretX = static_cast<std::uint32_t>(std::max(PageCol + static_cast<std::int32_t>(Of->Grid->Caret().Col), 0));
                CaretY = static_cast<std::uint32_t>(std::max(PageRow + static_cast<std::int32_t>(Of->Grid->Caret().Row), 0));
            }
        } else if (Of->Kind == ITEM_TEXT && Of->Said.Text) {
            std::u32string Characters = typecase::CharactersOf(*Of->Said.Text);
            if (Of->Said.Secret && *Of->Said.Secret)
                std::replace_if(Characters.begin(), Characters.end(), [](char32_t C) { return C != U'\n'; }, U'*');
            const std::uint32_t Ink  = InkOf(*Of);
            std::u32string_view Rest = Characters;
            for (std::int32_t Line = 0; !Rest.empty(); Line++) {
                const std::size_t End = Rest.find(U'\n');
                Write(Cells, Cols, Rows, Col, Row + Line, Rest.substr(0, End), Ink);
                Rest = End == std::u32string_view::npos ? std::u32string_view() : Rest.substr(End + 1);
            }
        } else if (Of->Kind != ITEM_GROUP && Of->Said.Caption) {
            Write(Cells, Cols, Rows, Col, Row, typecase::CharactersOf(*Of->Said.Caption), COLOR_DEFAULT);
        }
    }
}

}  // namespace

std::shared_ptr<CellGrid> FrameStart(const FrameGlass &Glass)
{
    g_text        = Glass.Text;
    g_theme       = items::ThemeOf(Glass.TextColour, Glass.PageColour);
    g_cells.Text  = Glass.TextColour;
    g_cells.Page  = Glass.PageColour;
    std::uint32_t Cols = 0, Rows = 0;
    if (g_text) {
        Cols           = Glass.Cols;
        Rows           = Glass.Rows;
        g_screen       = Rect{0, 0, static_cast<std::int32_t>(Cols) * PageCellWide, static_cast<std::int32_t>(Rows) * PageCellTall};
        g_cells.Type   = nullptr;
        g_cells.Width  = PageCellWide;
        g_cells.Height = PageCellTall;
        g_cells.Grid   = g_screen;
        g_composed.reserve(static_cast<std::size_t>(Cols) * Rows);
    } else {
        /* As many cells as the glass holds, at least one, in its middle — a
         * cell past the glass's edge drawn as far as the glass goes. */
        const std::uint32_t Wide = Glass.Width, High = Glass.Height;
        Typesetter         *Type = Glass.Type;
        const auto          CellWide = static_cast<std::uint32_t>(Type->Cell().Width);
        const auto          CellHigh = static_cast<std::uint32_t>(Type->Cell().Height);
        Cols                         = std::max<std::uint32_t>(1, Wide / CellWide);
        Rows                         = std::max<std::uint32_t>(1, High / CellHigh);
        const std::uint32_t X        = (Wide - std::min(Wide, Cols * CellWide)) / 2;
        const std::uint32_t Y        = (High - std::min(High, Rows * CellHigh)) / 2;
        g_screen                     = Rect{0, 0, static_cast<std::int32_t>(Wide), static_cast<std::int32_t>(High)};
        g_cells.Type                 = Type;
        g_cells.Width                = static_cast<std::int32_t>(CellWide);
        g_cells.Height               = static_cast<std::int32_t>(CellHigh);
        g_cells.Grid = Rect{static_cast<std::int32_t>(X), static_cast<std::int32_t>(Y), static_cast<std::int32_t>(Cols * CellWide),
                            static_cast<std::int32_t>(Rows * CellHigh)};
        g_tile = painter::core::Canvas::Make(TileSide, TileSide);
        if (!g_tile) throw std::bad_alloc();
        g_compositor.emplace(g_screen);
        g_order.reserve(ITEM_AREAS_MOST * 4u);
    }
    g_console          = std::make_shared<CellGrid>(Cols, Rows, Blank());
    g_own.From.Look    = &g_theme;
    g_own.From.Scale   = 1.0f;
    g_own.From.Cells   = &g_cells;
    g_console_area     = std::make_unique<items::AreaState>(g_own);
    g_console_area->Place = g_screen;
    g_console_area->Shown = true;
    g_console_area->Paged = true;
    if (g_console_area->Items.Create(ConsoleItem, ItemCreate{ITEM_CELLS, 0, ITEM_ROLE_CODE, ITEM_AREA, ITEM_TOP, 0}) != OK ||
        g_console_area->Items.Show(ConsoleItem, g_console) != OK)
        throw std::bad_alloc();
    g_everything = true;
    return g_console;
}

const items::Theme  &FrameTheme() noexcept { return g_theme; }
const CellsLook     &FrameCells() noexcept { return g_cells; }
painter::core::Rect  FrameScreen() noexcept { return g_screen; }

void FrameColours(std::uint32_t Text, std::uint32_t Page)
{
    g_theme      = items::ThemeOf(Text, Page);
    g_cells.Text = Text;
    g_cells.Page = Page;
    if (g_console_area) g_console_area->Items.RedrawAll();
}

void FrameVacated(const Rect &Where) noexcept
{
    try {
        g_before.push_back(Where);
    } catch (...) {
        g_everything = true;   // no room to keep where: all of the glass composed again
    }
}

void FrameEverything() noexcept { g_everything = true; }

bool FrameDue(const ConsoleModel &Console, bool Areas) noexcept
{
    if (!g_console) return false;
    g_console->SetCaret(Console.CursorX(), Console.CursorY(), true);
    return g_everything || g_console->Changed() || Areas || !g_before.empty() || (g_compositor && g_compositor->Damaged());
}

std::span<const ItemCell> FramePage(std::span<items::AreaState *const> Programs, Caret &At) noexcept
{
    if (!g_console || !g_text) return {};
    const std::uint32_t Cols = g_console->Cols(), Rows = g_console->Rows();
    (void)HoldCaret(Programs);
    const std::shared_ptr<CellGrid> Holds = g_holder.lock();
    At = Caret{g_console->Caret().Col, g_console->Caret().Row, Holds == g_console};
    try {
        g_composed.resize(g_console->Count());
        for (std::size_t Index = 0; Index < g_composed.size(); Index++) g_composed[Index] = g_console->At(Index);
        for (items::AreaState *Each : Programs) {
            std::uint32_t   X = 0, Y = 0;
            const CellGrid *Mine = Holds && Holds != g_console && g_holder_area == Each ? Holds.get() : nullptr;
            if (Each->Shown) CellsOf(*Each, g_composed, Cols, Rows, Mine, X, Y);
            if (Mine) At = Caret{X, Y, Holds->CaretShown()};
        }
    } catch (...) {
        /* No room to compose: the page shows what it showed, and all of it
         * is composed again at the next frame. */
        g_everything = true;
        return {};
    }
    for (items::AreaState *Each : Programs) {
        Each->Items.Taken();
        Each->Items.Vacated().clear();
        Each->Was      = Each->Place;
        Each->WasShown = Each->Shown;
    }
    g_console->TakeMarks([](std::uint32_t, std::uint32_t, std::uint32_t) {});
    (void)g_console->TakeScrolled();
    g_console_area->Items.Taken();
    g_everything = false;
    return g_composed;
}

FrameCost FrameCompose(Mirror &Into, std::span<items::AreaState *const> Programs) noexcept
{
    if (!g_console || g_text || !g_compositor || !g_tile) return {};
    const auto    Start  = std::chrono::steady_clock::now();
    std::uint64_t Pixels = 0;
    if (g_everything) {
        g_compositor->Damage(g_screen);
        g_everything = false;
    }

    /* The console scrolled: what the glass shows of it moves with it — all
     * but what the areas lay opaque over it, which stays still — and only
     * what came in is drawn, and what the areas drew over it, where it lay
     * and where the text under it went, and the stripe above what stayed
     * still, which the text came from under. Of what changed only the cells
     * the mirror does not show already are drawn. Under an area that hides
     * it whole, nothing of the console is seen, nor moved. */
    const std::uint32_t Scrolled = g_console->TakeScrolled();
    std::int32_t        Up       = 0;
    std::vector<Rect>   Still;
    try {
        g_seen.resize(g_console->Count(), Unseen);
        if (g_compositor->Hidden(g_cells.Grid, Programs)) {
            g_console->TakeMarks([](std::uint32_t, std::uint32_t, std::uint32_t) {});
            std::fill(g_seen.begin(), g_seen.end(), Unseen);
        } else {
            Still.reserve(KeptMost);
            Covers(Programs, Still);
            if (Scrolled != 0 && Scrolled < g_console->Rows()) {
                Up = static_cast<std::int32_t>(Scrolled) * g_cells.Height;
                Into.Shift(g_cells.Grid, Up, Still);
                const std::size_t Gone = static_cast<std::size_t>(Scrolled) * g_console->Cols();
                std::move(g_seen.begin() + static_cast<std::ptrdiff_t>(Gone), g_seen.end(), g_seen.begin());
                std::fill(g_seen.end() - static_cast<std::ptrdiff_t>(Gone), g_seen.end(), Unseen);
                /* The caret drawn went up with the text, its picture where
                 * its cell now is. */
                if (g_seen_caret != ~std::uint64_t{0})
                    g_seen_caret = g_seen_caret >= Gone ? g_seen_caret - Gone : ~std::uint64_t{0};
                for (items::AreaState *Each : Programs) {
                    if (!Each->WasShown) continue;
                    Drawn(*Each, [&](Rect Where) {
                        Where = Where.Intersect(g_cells.Grid);
                        if (Where.Empty() || Within(Where, Still)) return;
                        g_compositor->Damage(Where);
                        g_compositor->Damage(Rect{Where.X, Where.Y - Up, Where.Width, Where.Height}.Intersect(g_cells.Grid));
                    });
                }
                for (const Rect &Cover : Still) g_compositor->Damage(Rect{Cover.X, Cover.Y - Up, Cover.Width, Up}.Intersect(g_cells.Grid));
            }
            ConsoleChanged(Into, Programs, Still);
        }
    } catch (...) {
        /* No room to know what the mirror shows: all of the console drawn
         * again, nothing kept still. */
        g_console->TakeMarks([](std::uint32_t, std::uint32_t, std::uint32_t) {});
        g_seen.clear();
        g_compositor->Damage(g_cells.Grid);
    }

    /* What was to be composed again before the console moved: there, and
     * where the move took what the glass showed there. */
    for (const Rect &Each : g_before) {
        g_compositor->Damage(Each);
        if (Up == 0) continue;
        const Rect Moved = Each.Intersect(g_cells.Grid);
        if (!Moved.Empty()) g_compositor->Damage(Rect{Moved.X, Moved.Y - Up, Moved.Width, Moved.Height}.Intersect(g_cells.Grid));
    }
    g_before.clear();

    try {
        g_order.clear();
        g_order.push_back(g_console_area.get());
        g_order.insert(g_order.end(), Programs.begin(), Programs.end());
        g_compositor->Take(g_order, g_theme);
        if (HoldCaret(Programs)) g_compositor->Take(g_order, g_theme);
        const std::vector<Rect> Damaged = g_compositor->Rects();
        for (const Rect &Each : Damaged)
            for (std::int32_t Y = Each.Y; Y < Each.Y + Each.Height; Y += TileSide)
                for (std::int32_t X = Each.X; X < Each.X + Each.Width; X += TileSide) {
                    const Rect Tile{X, Y, std::min(TileSide, Each.X + Each.Width - X), std::min(TileSide, Each.Y + Each.Height - Y)};
                    /* What the areas draw composed; round it, the console
                     * alone written straight. */
                    const Rect Lies = Lying(Tile, Programs);
                    if (!Lies.Empty()) {
                        g_compositor->Compose(*g_tile, painter::core::Point{Lies.X, Lies.Y}, Lies, g_order, g_theme);
                        Into.Put(*g_tile, Lies);
                        if (g_seen.size() == g_console->Count())
                            CellsIn(Lies, [](std::size_t Index, bool) { g_seen[Index] = Unseen; });
                    }
                    const std::int32_t Right = Tile.X + Tile.Width, Bottom = Tile.Y + Tile.Height;
                    if (Lies.Empty()) {
                        LayConsole(Into, Tile);
                    } else {
                        LayConsole(Into, Rect{Tile.X, Tile.Y, Tile.Width, Lies.Y - Tile.Y});
                        LayConsole(Into, Rect{Tile.X, Lies.Y + Lies.Height, Tile.Width, Bottom - Lies.Y - Lies.Height});
                        LayConsole(Into, Rect{Tile.X, Lies.Y, Lies.X - Tile.X, Lies.Height});
                        LayConsole(Into, Rect{Lies.X + Lies.Width, Lies.Y, Right - Lies.X - Lies.Width, Lies.Height});
                    }
                    Pixels += static_cast<std::uint64_t>(Tile.Width) * static_cast<std::uint64_t>(Tile.Height);
                }
    } catch (...) {
        /* No room to compose: what the mirror holds stays, and all of the
         * glass is composed again at the next — wherever the console moves
         * meanwhile, and whatever of this frame's damage was taken. */
        g_everything = true;
    }

    const auto Micros = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - Start).count();
    return FrameCost{static_cast<std::uint32_t>(std::clamp<std::int64_t>(Micros, 0, UINT32_MAX)), Pixels};
}
