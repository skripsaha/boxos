/*
 * frame.cpp — what the glass shows, put together only where it changed. See
 * frame.h.
 */

#include "frame.h"

#include <algorithm>
#include <chrono>
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
    if (g_compositor) g_compositor->Damage(Where);
}

void FrameEverything() noexcept { g_everything = true; }

bool FrameDue(const ConsoleModel &Console, bool Areas) noexcept
{
    if (!g_console) return false;
    g_console->SetCaret(Console.CursorX(), Console.CursorY(), true);
    return g_everything || g_console->Changed() || Areas || (g_compositor && g_compositor->Damaged());
}

std::span<const ItemCell> FramePage(std::span<items::AreaState *const> Programs, Caret &At) noexcept
{
    if (!g_console || !g_text) return {};
    const std::uint32_t Cols = g_console->Cols(), Rows = g_console->Rows();
    (void)HoldCaret(Programs);
    const std::shared_ptr<CellGrid> Holds = g_holder.lock();
    At = Caret{g_console->Caret().Col, g_console->Caret().Row, Holds == g_console};
    try {
        g_composed.assign(g_console->Cells().begin(), g_console->Cells().end());
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

    /* The console scrolled: what the glass shows of it moves with it, and
     * only what came in is drawn — and the areas over it, where they lay and
     * where the text under them went. Under an area that hides it whole,
     * nothing of the console is seen, nor moved. */
    const std::uint32_t Scrolled = g_console->TakeScrolled();
    if (g_compositor->Hidden(g_cells.Grid, Programs)) {
        g_console->TakeMarks([](std::uint32_t, std::uint32_t, std::uint32_t) {});
    } else {
        if (Scrolled != 0 && Scrolled < g_console->Rows()) {
            const std::int32_t Up = static_cast<std::int32_t>(Scrolled) * g_cells.Height;
            Into.Shift(g_cells.Grid, Up);
            for (items::AreaState *Each : Programs) {
                if (!Each->WasShown) continue;
                const Rect Was = Each->Was.Intersect(g_cells.Grid);
                if (Was.Empty()) continue;
                g_compositor->Damage(Was);
                g_compositor->Damage(Rect{Was.X, Was.Y - Up, Was.Width, Was.Height}.Intersect(g_cells.Grid));
            }
        }
        if (g_console->Changed()) g_console_area->Items.CellsChanged(ConsoleItem);
    }

    std::vector<Rect> Damaged;
    try {
        g_order.clear();
        g_order.push_back(g_console_area.get());
        g_order.insert(g_order.end(), Programs.begin(), Programs.end());
        g_compositor->Take(g_order, g_theme);
        if (HoldCaret(Programs)) g_compositor->Take(g_order, g_theme);
        Damaged = g_compositor->Rects();
        for (const Rect &Each : Damaged)
            for (std::int32_t Y = Each.Y; Y < Each.Y + Each.Height; Y += TileSide)
                for (std::int32_t X = Each.X; X < Each.X + Each.Width; X += TileSide) {
                    const Rect Tile{X, Y, std::min(TileSide, Each.X + Each.Width - X), std::min(TileSide, Each.Y + Each.Height - Y)};
                    g_compositor->Compose(*g_tile, painter::core::Point{Tile.X, Tile.Y}, Tile, g_order, g_theme);
                    Into.Put(*g_tile, Tile);
                    Pixels += static_cast<std::uint64_t>(Tile.Width) * static_cast<std::uint64_t>(Tile.Height);
                }
    } catch (...) {
        /* No room to compose: what the mirror holds stays, and all of this
         * frame's damage is composed again at the next. */
        for (const Rect &Each : Damaged) g_compositor->Damage(Each);
    }

    const auto Micros = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - Start).count();
    return FrameCost{static_cast<std::uint32_t>(std::clamp<std::int64_t>(Micros, 0, UINT32_MAX)), Pixels};
}
