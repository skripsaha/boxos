#ifndef PAINTER_GUI_ITEMS_H
#define PAINTER_GUI_ITEMS_H

// A program's items as objects: made in an area, a group or a frame, told what
// they show with Set..., and gone when the object goes.
//
//     gui::Text Time = Screen->Text("12:46", gui::Role::Heading);
//     Time.SetPlace({16, 8});
//     Screen->Present();
//     Time.SetText("12:47");     // only this goes to the display
//     Screen->Present();
//
// What an item does not say is its role's, as the theme draws it. A property
// said again as it was goes nowhere (painter/gui/changes.h), so an item may be
// told all of itself every time. The numbers the language names items by are
// painter's to choose; a program never sees them.
//
// An item kept — made in an area, or in a group kept there — is the object's
// own: moved, it goes with the object; destroyed or Remove()d, it is gone from
// the area. An item described in a frame (Area::Draw) is the frame's: its
// object only says what it is in this frame, and the frame decides what
// changed. An object whose area is gone does nothing.

#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "box/items.h"
#include "painter/core/canvas.h"
#include "painter/core/geometry.h"
#include "painter/core/path.h"
#include "painter/gui/blocks.h"
#include "painter/gui/changes.h"

namespace painter::gui {

class Area;
class Frame;
class Text;
class Shape;
class Group;
class Picture;
class Canvas;
class Cells;

namespace detail {

// The numbers an area's items are named by: taken as an item is made, given
// back as it goes — past the numbers a program names items by itself. Of a
// kept item it knows the group it lies in, so an item whose group went first
// — and the display took it with the group — is known gone, and its object
// says nothing of it as it goes.
class Numbers {
public:
    std::uint32_t Take();
    // A kept item made under Parent.
    void Made(std::uint32_t Number, std::uint32_t Parent);
    // Gone with a group of its, already.
    bool Gone(std::uint32_t Number) const noexcept { return Went.contains(Number); }
    // Given back as its item goes: what lies in it gone with it.
    void Give(std::uint32_t Number) noexcept;

private:
    std::uint32_t                                                Next = 1u << 20;
    std::vector<std::uint32_t>                                   Free;
    std::unordered_map<std::uint32_t, std::uint32_t>             ParentOf;
    std::unordered_map<std::uint32_t, std::vector<std::uint32_t>> In;     // a group's kept items
    std::unordered_set<std::uint32_t>                            Went;
};

// Where an item's words go and its number comes from — the changes of the
// area it is kept in, and that area for what only an area does (a canvas's
// pages) — or the frame it is described in. The area points it anew when it
// moves and lets go of it when it goes; a frame lets go of it when it is drawn.
struct Anchor {
    Changes *Into       = nullptr;
    Numbers *Pool       = nullptr;
    Area    *Owner      = nullptr;
    Frame   *Describing = nullptr;
};

// An item of Kind made under Parent (ITEM_AREA, or a group's number): its
// number, and whether the object made for it keeps it.
struct Made {
    std::uint32_t Number = 0;
    bool          Owns   = false;
};
Made Make(const std::shared_ptr<Anchor> &Where, std::uint32_t Parent, gui::Kind Kind, gui::Role Role);
// A canvas of Pixels made under Parent, with its pages: none in a frame, nor
// when the display gives no block.
gui::Canvas MakeCanvas(const std::shared_ptr<Anchor> &Where, std::uint32_t Parent, core::Size Pixels);

}  // namespace detail

// What every item is: its number in its area, and where it is kept or
// described.
class Item {
public:
    Item() = default;
    Item(Item &&Other) noexcept;
    Item &operator=(Item &&Other) noexcept;
    Item(const Item &)            = delete;
    Item &operator=(const Item &) = delete;
    ~Item();

    // Made, and its area still open.
    explicit operator bool() const noexcept;
    // Gone from the area now; the object holds nothing after.
    void Remove();

protected:
    Item(std::shared_ptr<detail::Anchor> Where, detail::Made Made) noexcept;
    // Where its words go: the area's changes, a frame's; none when it is gone.
    Changes *Into() const noexcept;
    void     KeyAs(std::uint64_t Key) noexcept;

    std::shared_ptr<detail::Anchor> Where;
    std::uint32_t                   Number = 0;
    bool                            Owns   = false;

    friend class Area;
    friend class Frame;
    template <typename Self>
    friend class Makes;
};

// The words every item takes, each giving the item back so they chain.
template <typename Self>
class Words : public Item {
public:
    Self &SetPlace(core::PointF At) { return Say([&](Changes &To) { To.Place(Number, At); }); }
    // Either side ByContent: the content decides it.
    Self &SetSize(core::PointF Size) { return Say([&](Changes &To) { To.Size(Number, Size); }); }
    Self &SetOpacity(float Opacity) { return Say([&](Changes &To) { To.Opacity(Number, Opacity); }); }
    Self &Show() { return Say([&](Changes &To) { To.Hidden(Number, false); }); }
    Self &Hide() { return Say([&](Changes &To) { To.Hidden(Number, true); }); }
    Self &SetTurn(float Degrees) { return Say([&](Changes &To) { To.Turn(Number, Degrees); }); }
    Self &SetScale(core::PointF Scale) { return Say([&](Changes &To) { To.Scale(Number, Scale); }); }
    // The point it turns and scales about, from its corner, in points.
    Self &SetAnchor(core::PointF At) { return Say([&](Changes &To) { To.Anchor(Number, At); }); }
    Self &SetRole(gui::Role Role) { return Say([&](Changes &To) { To.Role(Number, Role); }); }
    // What it is in words, where it cannot be seen: a text page, a reader.
    Self &SetCaption(std::string_view Utf8) { return Say([&](Changes &To) { To.Caption(Number, Utf8); }); }
    // In a frame: which of the last frame's items this is, so an item of a
    // list stays itself when the list changes order. A kept item has none.
    Self &Key(std::uint64_t Key)
    {
        KeyAs(Key);
        return static_cast<Self &>(*this);
    }

protected:
    using Item::Item;
    template <typename Saying>
    Self &Say(Saying &&Into2)
    {
        if (Changes *To = Into()) Into2(*To);
        return static_cast<Self &>(*this);
    }
};

// The words of an item that is painted: a shape, a text.
template <typename Self>
class PaintWords : public Words<Self> {
public:
    Self &SetFill(const Fill &One) { return this->Say([&](Changes &To) { To.Fills(this->Number, std::span(&One, 1)); }); }
    // Fills laid one over another, the first lowest.
    Self &SetFills(std::span<const Fill> Fills) { return this->Say([&](Changes &To) { To.Fills(this->Number, Fills); }); }
    Self &SetStroke(const Stroke &Line) { return this->Say([&](Changes &To) { To.Stroke(this->Number, Line); }); }
    Self &SetShadow(const Shadow &One) { return this->Say([&](Changes &To) { To.Shadows(this->Number, std::span(&One, 1)); }); }
    Self &SetShadows(std::span<const Shadow> Shadows)
    {
        return this->Say([&](Changes &To) { To.Shadows(this->Number, Shadows); });
    }

protected:
    using Words<Self>::Words;
};

class Shape : public PaintWords<Shape> {
public:
    Shape() = default;
    Shape &SetRectangle() { return Say([&](Changes &To) { To.Rectangle(Number); }); }
    Shape &SetEllipse() { return Say([&](Changes &To) { To.Ellipse(Number); }); }
    // Any outline, in the shape's own points; EvenOdd: its holes by the
    // even-odd rule, else by the winding.
    Shape &SetOutline(const core::Path &Outline, bool EvenOdd = false)
    {
        return Say([&](Changes &To) { To.Outline(Number, Outline, EvenOdd); });
    }
    Shape &SetCorners(const core::Corners &Round) { return Say([&](Changes &To) { To.Corners(Number, Round); }); }

private:
    using PaintWords::PaintWords;
    template <typename Self>
    friend class Makes;
};

class Text : public PaintWords<Text> {
public:
    Text() = default;
    Text &SetText(std::string_view Utf8) { return Say([&](Changes &To) { To.Text(Number, Utf8); }); }
    // Its style: only what it says over its role's.
    Text &SetStyle(const TextStyle &Style) { return Say([&](Changes &To) { To.Style(Number, Style); }); }
    // Stretches of it styled otherwise, each saying only what it changes.
    Text &SetSpans(std::span<const Span> Spans) { return Say([&](Changes &To) { To.Spans(Number, Spans); }); }
    // How it lies in its size: aligned, wrapped, cut.
    Text &SetBox(const TextBox &Box) { return Say([&](Changes &To) { To.Box(Number, Box); }); }
    // Shown as stars, never said aloud nor copied.
    Text &SetSecret(bool Secret) { return Say([&](Changes &To) { To.Secret(Number, Secret); }); }

private:
    using PaintWords::PaintWords;
    template <typename Self>
    friend class Makes;
};

class Picture : public Words<Picture> {
public:
    Picture() = default;
    // The picture block it shows, fitted to its size (ITEM_FIT_*); Sharp: its
    // pixels kept square, as pixel art is.
    Picture &SetPicture(const PictureBlock &Block, std::uint8_t Fit = ITEM_FIT_STRETCH, bool Sharp = false)
    {
        return Say([&](Changes &To) { To.Picture(Number, Block.Id, Fit, Sharp); });
    }

private:
    using Words::Words;
    template <typename Self>
    friend class Makes;
};

// A canvas of the program's own pixels — real pixels of the screen, as many as
// it was made with — drawn with Painter's engine and shown whole.
class Canvas : public Words<Canvas> {
public:
    Canvas() = default;
    Canvas(Canvas &&Other) noexcept;
    Canvas &operator=(Canvas &&Other) noexcept;
    ~Canvas();

    core::Size Size() const noexcept { return {static_cast<std::int32_t>(Block.Width), static_cast<std::int32_t>(Block.Height)}; }
    // Paint draws the next picture, which the display shows once it is drawn
    // and presented: the whole canvas, every pixel of it painted, unless
    // Changed says only part of it changed — then the rest is the picture
    // shown before. Waits, telling what the display says to the area, while
    // the display still shows both pages. False when the area is gone.
    template <typename Painting>
    bool Draw(Painting &&Paint, std::optional<core::Rect> Changed = std::nullopt)
    {
        std::optional<core::Canvas> Page = Begin(Changed);
        if (!Page) return false;
        Paint(*Page);
        return End(Changed);
    }

private:
    Canvas(std::shared_ptr<detail::Anchor> Where, detail::Made Made, CanvasBlock Pages) noexcept;
    // The item gone, and its pages with it.
    void                        Close() noexcept;
    std::optional<core::Canvas> Begin(std::optional<core::Rect> Changed);
    bool                        End(std::optional<core::Rect> Changed);

    CanvasBlock   Block;
    std::uint32_t Drawing = 0;           // the page being drawn
    std::int32_t  Shown   = -1;          // the page shown last, none yet
    template <typename Self>
    friend class Makes;
    friend Canvas detail::MakeCanvas(const std::shared_ptr<detail::Anchor> &, std::uint32_t, core::Size);
};

// Cells in the console's look — each a character in its ink on its paper,
// with a mark — and a caret of their own; only the cells that changed go.
class Cells : public Words<Cells> {
public:
    Cells() = default;
    Cells &SetGrid(std::uint32_t Cols, std::uint32_t Rows) { return Say([&](Changes &To) { To.Grid(Number, Cols, Rows); }); }
    Cells &SetCells(std::uint32_t Row, std::uint32_t Col, std::span<const ItemCell> Run)
    {
        return Say([&](Changes &To) { To.Cells(Number, Row, Col, Run); });
    }
    Cells &SetCaret(std::optional<core::Point> At) { return Say([&](Changes &To) { To.Caret(Number, At); }); }

private:
    using Words::Words;
    template <typename Self>
    friend class Makes;
};

// What makes items: an area, a group, a frame — each under itself. Self says
// where (Where()) and under what (Parent()).
template <typename Self>
class Makes {
public:
    gui::Text Text(std::string_view Utf8, gui::Role Role = gui::Role::Body)
    {
        gui::Text Made(Me().Where(), detail::Make(Me().Where(), Me().Parent(), gui::Kind::Text, Role));
        Made.SetText(Utf8);
        return Made;
    }
    gui::Shape Rectangle(gui::Role Role = gui::Role::None)
    {
        return gui::Shape(Me().Where(), detail::Make(Me().Where(), Me().Parent(), gui::Kind::Shape, Role));
    }
    gui::Shape Ellipse(gui::Role Role = gui::Role::None)
    {
        gui::Shape Made(Me().Where(), detail::Make(Me().Where(), Me().Parent(), gui::Kind::Shape, Role));
        Made.SetEllipse();
        return Made;
    }
    gui::Shape Outline(const core::Path &Outline, bool EvenOdd = false, gui::Role Role = gui::Role::None)
    {
        gui::Shape Made(Me().Where(), detail::Make(Me().Where(), Me().Parent(), gui::Kind::Shape, Role));
        Made.SetOutline(Outline, EvenOdd);
        return Made;
    }
    gui::Group Group(gui::Role Role = gui::Role::None);
    gui::Picture Picture(const PictureBlock &Block, std::uint8_t Fit = ITEM_FIT_STRETCH, bool Sharp = false)
    {
        gui::Picture Made(Me().Where(), detail::Make(Me().Where(), Me().Parent(), gui::Kind::Picture, gui::Role::None));
        Made.SetPicture(Block, Fit, Sharp);
        return Made;
    }
    // A canvas Pixels large, of real pixels; kept only — a frame makes none.
    gui::Canvas Canvas(core::Size Pixels);
    // Cols x Rows cells in the console's look; kept only.
    gui::Cells Cells(std::uint32_t Cols, std::uint32_t Rows);

private:
    Self &Me() noexcept { return static_cast<Self &>(*this); }
};

// A group of items: they lie in it, are seen through it as one, and are
// clipped by it when it clips. Items are made in it as in an area — and a
// group in it with Subgroup, for in C++ a group's own name is its class's.
class Group : public Words<Group>, public Makes<Group> {
public:
    Group() = default;
    gui::Group Subgroup(gui::Role Role = gui::Role::None) { return Makes<Group>::Group(Role); }
    Group &SetClip(bool Clip) { return Say([&](Changes &To) { To.Clip(Number, Clip); }); }
    Group &SetCorners(const core::Corners &Round) { return Say([&](Changes &To) { To.Corners(Number, Round); }); }

private:
    using Words::Words;
    const std::shared_ptr<detail::Anchor> &Where() const noexcept { return Item::Where; }
    std::uint32_t                          Parent() const noexcept { return Number; }
    template <typename Self>
    friend class Makes;
};

template <typename Self>
gui::Group Makes<Self>::Group(gui::Role Role)
{
    return gui::Group(Me().Where(), detail::Make(Me().Where(), Me().Parent(), gui::Kind::Group, Role));
}

namespace detail {

// What makes items into Changes of its own and numbers them from Numbers of
// its own: an area's way, for painter's own stand, which has no area.
class Holder : public Makes<Holder> {
public:
    Holder(Changes &Into, Numbers &Pool) : Anchored(std::make_shared<Anchor>(Anchor{&Into, &Pool, nullptr, nullptr})) {}
    Holder(const Holder &)            = delete;
    Holder &operator=(const Holder &) = delete;
    ~Holder() { Anchored->Into = nullptr, Anchored->Pool = nullptr; }
    const std::shared_ptr<Anchor> &Where() const noexcept { return Anchored; }
    std::uint32_t                  Parent() const noexcept { return ITEM_AREA; }

private:
    std::shared_ptr<Anchor> Anchored;
};

}  // namespace detail

template <typename Self>
gui::Canvas Makes<Self>::Canvas(core::Size Pixels)
{
    return detail::MakeCanvas(Me().Where(), Me().Parent(), Pixels);
}

template <typename Self>
gui::Cells Makes<Self>::Cells(std::uint32_t Cols, std::uint32_t Rows)
{
    if (Me().Where() && Me().Where()->Describing) return gui::Cells();
    gui::Cells Made(Me().Where(), detail::Make(Me().Where(), Me().Parent(), gui::Kind::Cells, gui::Role::None));
    Made.SetGrid(Cols, Rows);
    return Made;
}

}  // namespace painter::gui

#endif  // PAINTER_GUI_ITEMS_H
