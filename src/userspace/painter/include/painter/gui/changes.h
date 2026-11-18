#ifndef PAINTER_GUI_CHANGES_H
#define PAINTER_GUI_CHANGES_H

// Changes to an area's items, written in the items' language (box/items.h):
// each made a record as it is said, and put into the area's ring when the
// program presents a step — as many as the ring has room for, the step's end
// after the last. What does not fit waits here, and a property said again
// while it waits replaces what it said before: the last word wins, nothing is
// lost but what was said over, and the program never waits for the display.
//
// What was last said of each property of each item is kept too: a property
// said again as it was goes nowhere, so a program may say all of an item each
// time and only what differs is written; a change the display refused is
// forgotten, so saying it again writes it again.
//
// A cells item's cells are kept here as last said, each cell a word of its
// own: only the cells that differ from what was said before are marked, and
// each row's marked stretch goes into the ring after every other change.
//
// Values are given in points (floats), colours as Painter's; they are written
// to 1/256 of a point and sixteen bits a channel.

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "box/items.h"
#include "painter/core/color.h"
#include "painter/core/geometry.h"
#include "painter/core/path.h"

namespace painter::gui {

enum class Kind : std::uint8_t {
    Group   = ITEM_GROUP,
    Text    = ITEM_TEXT,
    Shape   = ITEM_SHAPE,
    Picture = ITEM_PICTURE,
    Canvas  = ITEM_CANVAS,
    Cells   = ITEM_CELLS,
};

enum class Role : std::uint16_t {
    None      = ITEM_ROLE_NONE,
    Page      = ITEM_ROLE_PAGE,
    Panel     = ITEM_ROLE_PANEL,
    Body      = ITEM_ROLE_BODY,
    Heading   = ITEM_ROLE_HEADING,
    Code      = ITEM_ROLE_CODE,
    Hint      = ITEM_ROLE_HINT,
    Label     = ITEM_ROLE_LABEL,
    Button    = ITEM_ROLE_BUTTON,
    Field     = ITEM_ROLE_FIELD,
    Icon      = ITEM_ROLE_ICON,
    Separator = ITEM_ROLE_SEPARATOR,
    Selection = ITEM_ROLE_SELECTION,
};

// A colour of the theme's: what follows the person's choice of look.
enum class ThemeColor : std::uint16_t {
    Text   = ITEM_THEME_TEXT,
    Page   = ITEM_THEME_PAGE,
    Panel  = ITEM_THEME_PANEL,
    Accent = ITEM_THEME_ACCENT,
    Quiet  = ITEM_THEME_QUIET,
};

struct Stop {
    float       Offset = 0;
    core::Color Color;
};

// One of an item's fills.
struct Fill {
    enum class Kind : std::uint8_t { Color, Theme, Linear, Radial, Conic, Picture };
    Kind              What    = Kind::Color;
    core::Color       Color;
    gui::ThemeColor   Theme   = gui::ThemeColor::Text;
    core::PointF      From{}, To{};          // linear ends; radial and conic middle is From
    float             Radius  = 0;
    core::PointF      Focus{};
    float             StartDegrees = 0;      // conic, 0 up
    std::vector<Stop> Stops;
    std::uint8_t      Extend  = ITEM_EXTEND_PAD;
    std::uint32_t     Block   = 0;           // a picture block
    core::PointF      At{}, Size{};          // where the picture lies in the item, and how large
    bool              Sharp   = false;
    float             Opacity = 1;

    static Fill Of(core::Color Color) { Fill Made; Made.Color = Color; return Made; }
    static Fill OfTheme(gui::ThemeColor Which) { Fill Made; Made.What = Kind::Theme; Made.Theme = Which; return Made; }
};

struct Stroke {
    float              Width      = 1;
    std::uint8_t       Cap        = ITEM_CAP_BUTT;
    std::uint8_t       Join       = ITEM_JOIN_MITER;
    std::uint8_t       Align      = ITEM_ALIGN_CENTER;
    float              MiterLimit = 4;
    std::vector<float> Dashes;
    float              DashOffset = 0;
    gui::Fill          With;
};

struct Shadow {
    core::PointF Offset{};
    float        Blur   = 0;
    float        Spread = 0;
    core::Color  Color  = core::Color::Rgba(0, 0, 0, 96);
    bool         Inner  = false;
};

// How a text, or a stretch of it, is set: only what it says.
struct TextStyle {
    std::optional<std::string_view> Family;
    std::optional<float>            Size;
    std::optional<std::uint16_t>    Weight;
    std::optional<bool>             Italic;
    std::optional<float>            LetterSpacing;
    std::optional<float>            LineHeight;
    std::optional<bool>             Underline;
    std::optional<bool>             Strike;
    std::optional<core::Color>      Color;
};

struct Span {
    std::uint32_t From = 0, To = 0;
    TextStyle     Style;
};

struct TextBox {
    std::uint8_t  Align    = ITEM_TEXT_LEFT;
    std::uint8_t  Overflow = ITEM_OVERFLOW_GROW;
    bool          Wrap     = true;
    std::uint32_t MaxLines = 0;
};

// A size the item's content decides.
constexpr float ByContent = -1.0f;

class Changes {
public:
    void Create(std::uint32_t Item, gui::Kind Kind, std::uint32_t Parent = ITEM_AREA, std::uint32_t Above = ITEM_TOP,
                gui::Role Role = gui::Role::None);
    void Remove(std::uint32_t Item);
    void Restack(std::uint32_t Item, std::uint32_t Parent, std::uint32_t Above);
    void Reset(std::uint32_t Item, std::uint16_t Property);
    // A block's pixels changed: Where in its own, a canvas's Page now shown.
    void BlockChanged(std::uint32_t Block, core::Rect Where, std::uint32_t Page = 0);

    void Place(std::uint32_t Item, core::PointF At);
    void Size(std::uint32_t Item, core::PointF Size);   // either ByContent
    void Opacity(std::uint32_t Item, float Opacity);
    void Hidden(std::uint32_t Item, bool Hidden);
    void Turn(std::uint32_t Item, float Degrees);
    void Scale(std::uint32_t Item, core::PointF Scale);
    void Anchor(std::uint32_t Item, core::PointF At);
    void Clip(std::uint32_t Item, bool Clip);
    void Corners(std::uint32_t Item, const core::Corners &Round);
    void Rectangle(std::uint32_t Item);
    void Ellipse(std::uint32_t Item);
    void Outline(std::uint32_t Item, const core::Path &Shape, bool EvenOdd = false);
    void Fills(std::uint32_t Item, std::span<const gui::Fill> Fills);
    void Stroke(std::uint32_t Item, const gui::Stroke &Line);
    void Shadows(std::uint32_t Item, std::span<const gui::Shadow> Shadows);
    void Text(std::uint32_t Item, std::string_view Utf8);
    void Style(std::uint32_t Item, const TextStyle &Style);
    void Spans(std::uint32_t Item, std::span<const gui::Span> Spans);
    void Box(std::uint32_t Item, const TextBox &Box);
    void Secret(std::uint32_t Item, bool Secret);
    void Caption(std::uint32_t Item, std::string_view Utf8);
    void Picture(std::uint32_t Item, std::uint32_t Block, std::uint8_t Fit = ITEM_FIT_STRETCH, bool Sharp = false);
    void Canvas(std::uint32_t Item, std::uint32_t Block);
    void Role(std::uint32_t Item, gui::Role Role);

    // A cells item's grid, every cell of it showing nothing; its cells from
    // Col of Row, as far as the grid goes; and its caret, none for none.
    void Grid(std::uint32_t Item, std::uint32_t Cols, std::uint32_t Rows);
    void Cells(std::uint32_t Item, std::uint32_t Row, std::uint32_t Col, std::span<const ItemCell> Cells);
    void Caret(std::uint32_t Item, std::optional<core::Point> At);

    // Into a ring of Bytes after Write, the reader at Read: as many records as
    // fit, then Step's end when all of them did — or when what is said is more
    // than the ring holds at once: then what fit goes as a step of its own,
    // the rest in the next. True when nothing waits.
    bool Flush(std::uint8_t *Ring, std::uint32_t Bytes, std::uint64_t &Write, std::uint64_t Read, std::uint64_t Step);

    bool        Empty() const noexcept { return Ordered.empty() && Properties.empty() && CellsMarked == 0; }
    std::size_t Waiting() const noexcept { return Ordered.size() + Properties.size() + CellsMarked; }
    // The bytes of records waiting, and those ever written.
    std::size_t Bytes() const noexcept;
    std::uint64_t Written() const noexcept { return WrittenBytes; }
    // A change refused here, before it was written: a value too large for a
    // ring to hold. Counted, never written.
    std::uint64_t Refused() const noexcept { return RefusedCount; }

    // What was last said of Item's Property, as the language writes it; none
    // when it is unsaid. The properties Item says, in their order.
    const std::vector<std::uint8_t> *Said(std::uint32_t Item, std::uint16_t Property) const noexcept;
    std::vector<std::uint16_t>       SaidOf(std::uint32_t Item) const;
    // Item's Property no longer known as said: the display refused it, and
    // what it holds is what was said before.
    void Forget(std::uint32_t Item, std::uint16_t Property);
    // Item's Property said as the language writes it — a value Said gave
    // back, of another item — written unless it is what was said.
    void Say(std::uint32_t Item, std::uint16_t Property, std::span<const std::uint8_t> Value)
    {
        Put(Item, Property, Record(Value.begin(), Value.end()));
    }

protected:
    using Record = std::vector<std::uint8_t>;
    // Item's Property said as Value — written unless it is what was said.
    void Put(std::uint32_t Item, std::uint16_t Property, Record Value);

private:
    static Record Make(std::uint16_t Op, std::uint32_t Item, std::span<const std::uint8_t> Body);

    // A cells item's cells as last said, and each row's stretch [From, To)
    // said since it was put in the ring.
    struct KeptCells {
        std::uint32_t                                       Cols = 0, Rows = 0;
        std::vector<ItemCell>                               Cells;
        std::vector<std::pair<std::uint32_t, std::uint32_t>> Marked;
    };
    bool FlushCells(const auto &Into, std::uint32_t Most);

    std::vector<std::pair<std::uint32_t, Record>>               Ordered;      // the item, and its record
    std::map<std::pair<std::uint32_t, std::uint16_t>, Record>   Properties;   // each property's last word, waiting
    std::map<std::pair<std::uint32_t, std::uint16_t>, Record>   Values;       // each property as last said
    std::map<std::uint32_t, KeptCells>                          Grids;        // each cells item's
    std::size_t                                                 CellsMarked = 0;   // rows with a stretch marked
    static constexpr std::uint64_t                              NoStep = ~std::uint64_t{0};
    std::uint64_t                                               Open   = NoStep;   // where the step being written began
    bool          StepDue      = false;
    std::uint64_t WrittenBytes = 0;
    std::uint64_t RefusedCount = 0;
};

}  // namespace painter::gui

#endif  // PAINTER_GUI_CHANGES_H
