/*
 * compositor.cpp — the areas put together onto the screen only where
 * something changed. See compositor.h.
 */

#include "compositor.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <utility>

#include "painter/core/path.h"

namespace items {

void DropDrawing::operator()(Drawing *Of) const noexcept
{
    if (Of && Of->Held) *Of->Held -= Of->Bytes;
    delete Of;
}

namespace {

using painter::core::Rect;
using painter::core::Transform;

constexpr float PerPoint = 1.0f / ITEM_POINT;

/* The rectangles damage is kept in at most: past them, the two that grow the
 * least by it are joined. */
constexpr std::size_t DamageMost = 24;

bool Holds(Rect Outer, Rect Inner) noexcept
{
    return Inner.X >= Outer.X && Inner.Y >= Outer.Y && Inner.X + Inner.Width <= Outer.X + Outer.Width &&
           Inner.Y + Inner.Height <= Outer.Y + Outer.Height;
}

std::int64_t AreaOf(Rect Of) noexcept { return Of.Empty() ? 0 : std::int64_t{Of.Width} * Of.Height; }

/* The whole pixels inside what Place lays Box of points on: an opaque item's
 * pixels it surely covers whole. */
Rect Inside(const painter::core::RectF &Box, const Transform &Place) noexcept
{
    const painter::core::PointF A = Place.Apply({Box.X, Box.Y}), B = Place.Apply({Box.X + Box.Width, Box.Y + Box.Height});
    const float Left = std::min(A.X, B.X), Right = std::max(A.X, B.X), Top = std::min(A.Y, B.Y), Bottom = std::max(A.Y, B.Y);
    const float Reach = 1 << 24;
    if (!(Left > -Reach && Right < Reach && Top > -Reach && Bottom < Reach)) return {};
    const auto X = static_cast<std::int32_t>(std::ceil(Left)), Y = static_cast<std::int32_t>(std::ceil(Top));
    return Rect{X, Y, static_cast<std::int32_t>(std::floor(Right)) - X, static_cast<std::int32_t>(std::floor(Bottom)) - Y};
}

/* The pixel a place falls nearest, a half always up (item_draw.cpp). */
std::int32_t Nearest(float Place) noexcept { return static_cast<std::int32_t>(std::floor(Place + 0.5f)); }

/* A cells item laid as Place lays it — on the console's grid where its own
 * place is unsaid. */
Transform CellsPlace(const Item &Of, const Transform &Place, const CellsLook &Look) noexcept
{
    if (Of.Said.Place) return Place;
    const painter::core::PointF Corner = Place.Apply({0, 0});
    const painter::core::Point  Grid   = OnTheGrid(Look, painter::core::Point{Nearest(Corner.X), Nearest(Corner.Y)});
    return Place.Then(Transform::Translate(static_cast<float>(Grid.X) - Corner.X, static_cast<float>(Grid.Y) - Corner.Y));
}

/* What changed in a cells item's grid since a frame last took it: damage
 * where its cells lie — only the cells that changed where it lies upright,
 * all of it elsewhere — or nothing where it is drawn whole anyway. */
template <typename Damaging>
void CellsChanged(CellGrid &Grid, const CellsLook &Look, const Transform &Place, float Scale, Rect Now, bool Whole,
                  Damaging &&Damage)
{
    (void)Grid.TakeScrolled();
    if (Whole || !Grid.Changed()) {
        Grid.TakeMarks([](std::uint32_t, std::uint32_t, std::uint32_t) {});
        return;
    }
    if (!Upright(Place, Scale)) {
        Grid.TakeMarks([](std::uint32_t, std::uint32_t, std::uint32_t) {});
        Damage(Now);
        return;
    }
    const painter::core::PointF Corner = Place.Apply({0, 0});
    const std::int32_t          X = Nearest(Corner.X), Y = Nearest(Corner.Y);
    Grid.TakeMarks([&](std::uint32_t Row, std::uint32_t From, std::uint32_t To) {
        Damage(Rect{X + static_cast<std::int32_t>(From) * Look.Width, Y + static_cast<std::int32_t>(Row) * Look.Height,
                    static_cast<std::int32_t>(To - From) * Look.Width, Look.Height}
                   .Intersect(Now));
    });
}

}  // namespace

Transform LocalOf(const Item &Of) noexcept
{
    const ItemPoint Place = Of.Said.Place ? *Of.Said.Place : ItemPoint{0, 0};
    const float     X = Place.X * PerPoint, Y = Place.Y * PerPoint;
    const bool      Turned = Of.Said.Turn && *Of.Said.Turn % ITEM_TURN != 0;
    const bool      Scaled = Of.Said.Scale && (Of.Said.Scale->X != ITEM_ONE || Of.Said.Scale->Y != ITEM_ONE);
    if (!Turned && !Scaled) return Transform::Translate(X, Y);
    const ItemPoint Anchor = Of.Said.Anchor ? *Of.Said.Anchor : ItemPoint{0, 0};
    const float     AX = Anchor.X * PerPoint, AY = Anchor.Y * PerPoint;
    Transform       Made = Transform::Translate(-AX, -AY);
    if (Scaled) Made = Made.Then(Transform::Scale(static_cast<float>(Of.Said.Scale->X) / ITEM_ONE,
                                                  static_cast<float>(Of.Said.Scale->Y) / ITEM_ONE));
    if (Turned) Made = Made.Then(Transform::Rotate(static_cast<float>(*Of.Said.Turn) * 360.0f / ITEM_TURN));
    return Made.Then(Transform::Translate(AX + X, AY + Y));
}

namespace {

/* Area noted in rectangles reserved beforehand: in none that holds it
 * already; past them, joined to the one it grows the least. */
void Note(std::vector<Rect> &Into, Rect Area) noexcept
{
    for (const Rect &Each : Into)
        if (Holds(Each, Area)) return;
    if (Into.size() < Into.capacity()) {
        Into.push_back(Area);
        return;
    }
    std::size_t  Best  = 0;
    std::int64_t Least = -1;
    for (std::size_t I = 0; I < Into.size(); I++) {
        const std::int64_t Grows = AreaOf(Into[I].Join(Area)) - AreaOf(Into[I]);
        if (Least < 0 || Grows < Least) Least = Grows, Best = I;
    }
    if (Into.empty()) return;
    Into[Best] = Into[Best].Join(Area);
}

/* Overlapping rectangles joined, again until none overlaps. */
void JoinOverlapping(std::vector<Rect> &Made)
{
    for (bool Joined = true; Joined;) {
        Joined = false;
        for (std::size_t I = 0; I < Made.size() && !Joined; I++)
            for (std::size_t J = I + 1; J < Made.size() && !Joined; J++)
                if (!Made[I].Intersect(Made[J]).Empty() || Holds(Made[I], Made[J]) || Holds(Made[J], Made[I])) {
                    Made[I] = Made[I].Join(Made[J]);
                    Made.erase(Made.begin() + static_cast<std::ptrdiff_t>(J));
                    Joined = true;
                }
    }
}

}  // namespace

void Compositor::Damage(Rect Area) noexcept
{
    Area = Area.Intersect(Screen);
    if (Area.Empty()) return;
    Note(Waiting, Area);
    if (Owning) Note(Own, Area);
}

void Compositor::Vanish(AreaState &Area, const std::vector<std::uint32_t> &Items)
{
    for (std::uint32_t Id : Items) {
        Item *Of = Area.Items.Find(Id);
        if (!Of) continue;
        Damage(Of->Shown);
        Of->Shown  = {};
        if (Of->Grid) {
            /* What changed in cells not seen is drawn with them when they are. */
            (void)Of->Grid->TakeScrolled();
            Of->Grid->TakeMarks([](std::uint32_t, std::uint32_t, std::uint32_t) {});
        }
        Of->Redraw = Of->Moved || Of->Redraw;   /* drawn again when it comes back */
        Of->Moved  = true;
        Of->Within = false;
        Vanish(Area, Of->Children);
    }
}

void Compositor::Walk(AreaState &Area, const std::vector<std::uint32_t> &Items, const Transform &Parent, float Opacity,
                      bool Moved, bool Plain)
{
    ProgramState &Program = Area.Program;
    for (std::uint32_t Id : Items) {
        Item *Of = Area.Items.Find(Id);
        if (!Of) continue;
        if (!Of->Drawn) {
            Of->Drawn.reset(new Drawing);
            Of->Drawn->Held = &Program.KeptBytes;
        }
        Drawing &Drawn = *Of->Drawn;
        if (Of->Redraw) {
            Program.KeptBytes -= Drawn.Bytes;
            Drawn.Bytes = 0;
            const bool Sized = Prepare(*Of, Program.From, Drawn, Program.KeptBytes < KeptMost);
            if (Drawn.Kept)
                Drawn.Bytes = static_cast<std::size_t>(Drawn.Kept->Width()) * Drawn.Kept->Height() * sizeof(painter::core::Pixel);
            Program.KeptBytes += Drawn.Bytes;
            if (Sized && Area.Sized) Area.Sized(Id, Drawn.Measured);
        }
        const bool      Cellular = Of->Kind == ITEM_CELLS && Of->Grid && Program.From.Cells;
        const Transform Place    = Cellular ? CellsPlace(*Of, LocalOf(*Of).Then(Parent), *Program.From.Cells)
                                            : LocalOf(*Of).Then(Parent);
        const bool      Hidden   = HiddenOf(Of->Said) || !(Of->Seen > 0.0f);
        const bool      Shifted  = Moved || Of->Moved;
        const Leaving  *Fade     = Of->Fading && Of->Fading->Was.Drawn ? Of->Fading.get() : nullptr;
        Rect            Now      = Hidden ? Rect{} : DeviceRect(Drawn.Own, Place).Intersect(Area.Place);
        if (!Hidden && Fade) Now = Now.Join(DeviceRect(Fade->Was.Drawn->Own, Place).Intersect(Area.Place));
        const bool      Whole    = Of->Redraw || Shifted;
        if (Whole) {
            Damage(Of->Shown);
            Damage(Now);
            Of->Shown = Now;
        } else if (Of->Changed && !Hidden) {
            /* Only part of a canvas changed: where it lies on the glass, its
             * page laid pixel for pixel — else all of it. */
            const std::optional<painter::core::Point> Corner = CanvasCorner(*Of, Drawn, Program.From, Place);
            Damage(Corner ? Rect{Corner->X + Of->Changed->X, Corner->Y + Of->Changed->Y, Of->Changed->Width, Of->Changed->Height}.Intersect(Now)
                          : Now);
        }
        Of->Changed.reset();
        if (Cellular)
            CellsChanged(*Of->Grid, *Program.From.Cells, Place, Program.From.Scale, Now, Whole || Hidden,
                         [this](Rect Where) { Damage(Where); });
        Of->Redraw = Of->Moved = Of->Within = false;
        if (Hidden) {
            if (Shifted) Vanish(Area, Of->Children);
            continue;
        }
        const float Own = static_cast<float>(OpacityOf(Of->Said)) / ITEM_OPAQUE * std::min(Of->Seen, 1.0f);
        if (Of->Kind != ITEM_GROUP) {
            Entry Made{Entry::Kind::Draw, Of, Place, Opacity * Own, Now, {}, Fade};
            if (Fade) {
                /* Fading from another look: it hides nothing beneath. */
            } else if (Plain && Drawn.Opaque && Opacity * Own >= 1.0f && Place.B == 0 && Place.C == 0) {
                const painter::core::PointF Size = SizeOf(*Of, Drawn);
                Made.Hides = Inside(painter::core::RectF{0, 0, Size.X, Size.Y}, Place).Intersect(Area.Place);
            } else if (Plain && Cellular && Of->Grid->Opaque() && Opacity * Own >= 1.0f && Upright(Place, Program.From.Scale)) {
                /* Cells whose papers all cover: their grid's pixels, whole. */
                const painter::core::PointF Corner = Place.Apply({0, 0});
                Made.Hides = Rect{Nearest(Corner.X), Nearest(Corner.Y),
                                  static_cast<std::int32_t>(Of->Grid->Cols()) * Program.From.Cells->Width,
                                  static_cast<std::int32_t>(Of->Grid->Rows()) * Program.From.Cells->Height}
                                 .Intersect(Area.Place);
            }
            Area.List.push_back(Made);
            continue;
        }
        /* A group: its items within its clip, and through a picture of their
         * own where it is not wholly opaque. */
        const bool        Layered = Own < 1.0f;
        const bool        Clipped = Of->Said.Clip && *Of->Said.Clip && !ClipOf(*Of).Empty();
        const std::size_t Begun   = Area.List.size();
        if (Layered) Area.List.push_back(Entry{Entry::Kind::LayerBegin, Of, Place, Opacity * Own, {}, {}});
        if (Clipped) Area.List.push_back(Entry{Entry::Kind::ClipBegin, Of, Place, 1.0f, {}, {}});
        Walk(Area, Of->Children, Place, Layered ? 1.0f : Opacity * Own, Shifted, Plain && !Layered && !Clipped);
        if (Clipped) Area.List.push_back(Entry{Entry::Kind::ClipEnd, Of, Place, 1.0f, {}, {}});
        if (Layered) {
            Area.List.push_back(Entry{Entry::Kind::LayerEnd, Of, Place, Opacity * Own, {}, {}});
            Rect Covers{};
            for (std::size_t I = Begun + 1; I + 1 < Area.List.size(); I++) Covers = Covers.Join(Area.List[I].Device);
            Area.List[Begun].Device = Covers;
        }
    }
}

void Compositor::Take(std::span<AreaState *const> Areas, const Theme &Look)
{
    for (AreaState *Area : Areas) {
        Own.clear();
        Owning = true;
        TakeOne(*Area, Look);
        Owning = false;
        /* What its own changes cost, whatever else the frame composes. */
        JoinOverlapping(Own);
        Area->Composed = 0;
        for (const Rect &Each : Own) Area->Composed += static_cast<std::uint64_t>(AreaOf(Each));
    }
}

void Compositor::TakeOne(AreaState &Area, const Theme &Look)
{
    const bool Placed     = Area.Place != Area.Was || Area.Shown != Area.WasShown;
    Area.Program.From.Look = &Look;
    for (const Rect &Gone : Area.Items.Vacated()) Damage(Gone);
    Area.Items.Vacated().clear();
    if (!Placed && !Area.Items.Marked()) return;
    if (Placed) {
        if (Area.WasShown) Damage(Area.Was);
        if (Area.Shown) Damage(Area.Place);
    }
    Area.List.clear();
    if (Area.Shown) {
        const Transform Root = Transform::Scale(Area.Program.From.Scale)
                                   .Then(Transform::Translate(static_cast<float>(Area.Place.X), static_cast<float>(Area.Place.Y)));
        try {
            Walk(Area, Area.Items.Top(), Root, 1.0f, Placed, true);
        } catch (...) {
            /* No room to compose it: the area shown as far as it was walked,
             * and walked again whole at the next frame. */
            Area.Items.RedrawAll();
            Damage(Area.Place);
            return;
        }
    } else {
        Vanish(Area, Area.Items.Top());
    }
    Area.Was      = Area.Place;
    Area.WasShown = Area.Shown;
    Area.Items.Taken();
}

std::vector<Rect> Compositor::Rects()
{
    std::vector<Rect> Made = Waiting;
    Waiting.clear();
    JoinOverlapping(Made);
    /* Too many: the two that grow the least joined. */
    while (Made.size() > DamageMost) {
        std::size_t  BestI = 0, BestJ = 1;
        std::int64_t Least = -1;
        for (std::size_t I = 0; I < Made.size(); I++)
            for (std::size_t J = I + 1; J < Made.size(); J++) {
                const std::int64_t Grows = AreaOf(Made[I].Join(Made[J])) - AreaOf(Made[I]) - AreaOf(Made[J]);
                if (Least < 0 || Grows < Least) Least = Grows, BestI = I, BestJ = J;
            }
        Made[BestI] = Made[BestI].Join(Made[BestJ]);
        Made.erase(Made.begin() + static_cast<std::ptrdiff_t>(BestJ));
    }
    return Made;
}

bool Compositor::Hidden(Rect Area, std::span<AreaState *const> Areas) const noexcept
{
    for (std::size_t A = Areas.size(); A-- > 0;) {
        const AreaState &Each = *Areas[A];
        if (!Each.Shown) continue;
        if (Each.Paged && Holds(Each.Place, Area)) return true;
        for (const Entry &One : Each.List)
            if (One.What == Entry::Kind::Draw && Holds(One.Hides, Area)) return true;
    }
    return false;
}

void Compositor::Compose(painter::core::Canvas &Target, painter::core::Point Corner, Rect Area,
                         std::span<AreaState *const> Areas, const Theme &Look)
{
    /* The topmost opaque item covering the rectangle whole: nothing beneath
     * it is drawn. */
    std::size_t StartArea = 0, StartEntry = 0;
    bool        Found     = false;
    for (std::size_t A = Areas.size(); A-- > 0 && !Found;) {
        const AreaState &Each = *Areas[A];
        if (!Each.Shown) continue;
        for (std::size_t E = Each.List.size(); E-- > 0;)
            if (Each.List[E].What == Entry::Kind::Draw && Holds(Each.List[E].Hides, Area)) {
                StartArea = A, StartEntry = E, Found = true;
                break;
            }
        if (!Found && Each.Paged && Holds(Each.Place, Area)) StartArea = A, StartEntry = 0, Found = true;
    }

    /* Every canvas drawn on lies somewhere on the screen: Target at Corner,
     * a group's own picture at Area's corner. */
    const auto Into = [](Rect Of, painter::core::Point At) { return Rect{Of.X - At.X, Of.Y - At.Y, Of.Width, Of.Height}; };
    const auto Moved = [](const Transform &Place, painter::core::Point At) {
        return At.X == 0 && At.Y == 0 ? Place
                                      : Place.Then(Transform::Translate(-static_cast<float>(At.X), -static_cast<float>(At.Y)));
    };
    const painter::core::Point Picture{Area.X, Area.Y};
    Target.PushClip(Into(Area, Corner));
    for (std::size_t A = StartArea; A < Areas.size(); A++) {
        AreaState &Each = *Areas[A];
        if (!Each.Shown || Each.Place.Intersect(Area).Empty()) continue;
        Target.PushClip(Into(Each.Place, Corner));
        if (Each.Paged && (A > StartArea || StartEntry == 0))
            Target.Clear(Into(Each.Place.Intersect(Area), Corner), Look.Page);
        /* Groups that see through: their items onto a picture of their own,
         * laid at the group's end. */
        struct Layer {
            std::optional<painter::core::Canvas> Picture;
            float                                Opacity;
        };
        std::vector<Layer> Layers;
        const auto Drawn = [&]() -> Layer * {
            for (std::size_t L = Layers.size(); L-- > 0;)
                if (Layers[L].Picture) return &Layers[L];
            return nullptr;
        };
        const auto Current = [&]() -> painter::core::Canvas & {
            Layer *On = Drawn();
            return On ? *On->Picture : Target;
        };
        const auto At = [&] { return Drawn() ? Picture : Corner; };
        for (std::size_t E = A == StartArea ? StartEntry : 0; E < Each.List.size(); E++) {
            const Entry &One = Each.List[E];
            switch (One.What) {
            case Entry::Kind::Draw: {
                const Rect Part = One.Device.Intersect(Area);
                if (Part.Empty()) break;
                std::optional<painter::core::Canvas> Old, New;
                if (One.Fade) {
                    Old = painter::core::Canvas::Make(static_cast<std::uint32_t>(Part.Width), static_cast<std::uint32_t>(Part.Height));
                    New = painter::core::Canvas::Make(static_cast<std::uint32_t>(Part.Width), static_cast<std::uint32_t>(Part.Height));
                }
                if (!Old || !New) {
                    Paint(*One.Of, *One.Of->Drawn, Each.Program.From, Current(), Moved(One.Place, At()), One.Opacity);
                    break;
                }
                /* Fading from another look: the two looks drawn apart, the
                 * old one walked toward the new as far as it has faded, laid
                 * as one. */
                const painter::core::Point Here{Part.X, Part.Y};
                Paint(One.Fade->Was, *One.Fade->Was.Drawn, Each.Program.From, *Old, Moved(One.Place, Here), 1.0f);
                Paint(*One.Of, *One.Of->Drawn, Each.Program.From, *New, Moved(One.Place, Here), 1.0f);
                Old->FadeTo(*New, One.Fade->Share);
                const painter::core::Point Under = At();
                Current().Draw(*Old, painter::core::Point{Part.X - Under.X, Part.Y - Under.Y}, One.Opacity);
                break;
            }
            case Entry::Kind::ClipBegin:
                (void)Current().PushClip(ClipOf(*One.Of), Moved(One.Place, At()));
                break;
            case Entry::Kind::ClipEnd:
                Current().PopClip();
                break;
            case Entry::Kind::LayerBegin:
                Layers.push_back(Layer{One.Device.Intersect(Area).Empty()
                                           ? std::nullopt
                                           : painter::core::Canvas::Make(static_cast<std::uint32_t>(Area.Width),
                                                                         static_cast<std::uint32_t>(Area.Height)),
                                       One.Opacity});
                break;
            case Entry::Kind::LayerEnd: {
                if (Layers.empty()) break;
                Layer Done = std::move(Layers.back());
                Layers.pop_back();
                if (Done.Picture) {
                    const painter::core::Point Under = At();
                    Current().Draw(*Done.Picture, painter::core::Point{Picture.X - Under.X, Picture.Y - Under.Y}, Done.Opacity);
                }
                break;
            }
            }
        }
        Target.PopClip();
    }
    Target.PopClip();
}

}  // namespace items
