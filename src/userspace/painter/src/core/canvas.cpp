/*
 * canvas.cpp — pixels in memory, drawn on. See painter/core/canvas.h.
 */

#include "painter/core/canvas.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <new>
#include <vector>

#include "blur.h"
#include "dither_noise.h"
#include "lanes.h"
#include "light.h"
#include "paint.h"
#include "shape_room.h"

namespace painter::core {

namespace {

/* The first of Count coverages that is not none — Count when all are —
 * read eight at a time where eight are none. */
std::int32_t FirstCovered(const std::uint8_t *Line, std::int32_t Count) noexcept
{
    std::int32_t I = 0;
    for (; I + 8 <= Count; I += 8) {
        std::uint64_t Eight;
        std::memcpy(&Eight, Line + I, sizeof Eight);
        if (Eight) break;
    }
    while (I < Count && !Line[I]) I++;
    return I;
}

/* Shares read four at a time: whether any of four is none, or whole. */
constexpr std::uint64_t Ones = 0x0001000100010001ull, Highs = 0x8000800080008000ull;
constexpr bool AnyNone(std::uint64_t Four) noexcept { return ((Four - Ones) & ~Four & Highs) != 0; }
std::uint64_t FourAt(const std::uint16_t *Share) noexcept
{
    std::uint64_t Four;
    std::memcpy(&Four, Share, sizeof Four);
    return Four;
}

/* The first of Share[I] up to End that is not none; End when none is. */
std::int32_t NextCovered(const std::uint16_t *Share, std::int32_t I, std::int32_t End) noexcept
{
    while (I + 4 <= End && !FourAt(Share + I)) I += 4;
    while (I < End && !Share[I]) I++;
    return I;
}
/* … that is none. */
std::int32_t NextUncovered(const std::uint16_t *Share, std::int32_t I, std::int32_t End) noexcept
{
    while (I + 4 <= End && !AnyNone(FourAt(Share + I))) I += 4;
    while (I < End && Share[I]) I++;
    return I;
}
/* … that is whole. */
std::int32_t NextWhole(const std::uint16_t *Share, std::int32_t I, std::int32_t End) noexcept
{
    while (I + 4 <= End && !AnyNone(~FourAt(Share + I))) I += 4;
    while (I < End && Share[I] != light::Whole) I++;
    return I;
}
/* … that is not whole. */
std::int32_t NextPart(const std::uint16_t *Share, std::int32_t I, std::int32_t End) noexcept
{
    while (I + 4 <= End && FourAt(Share + I) == ~0ull) I += 4;
    while (I < End && Share[I] == light::Whole) I++;
    return I;
}

std::uint32_t ShareOf(float Opacity) noexcept
{
    if (!(Opacity > 0.0f)) return 0;
    if (Opacity >= 1.0f) return light::Whole;
    return static_cast<std::uint32_t>(std::lround(Opacity * static_cast<float>(light::Whole)));
}

/* What a stroke is kept to against its shape's own fill. */
enum class Against : std::uint8_t { Nothing, Within, Without };

}  // namespace

struct Canvas::Work {
    struct ClipLevel {
        Rect                        Area;
        std::shared_ptr<const Mask> Cover;   /* none: the whole of the area */
    };

    engine::ShapeRoom             Shape, Edge;
    std::vector<std::int32_t>     Cells, EdgeCells;
    std::vector<std::uint16_t>    Shares;
    std::vector<Pixel>            Paint;
    std::vector<std::uint8_t>     Thresholds;
    std::vector<ClipLevel>        Clips;
    std::vector<engine::Prepared> Fills;

    /* A stretch of a row whose shares are not none, Begin to End. */
    struct Run {
        std::int32_t Begin, End;
    };
    std::vector<Run> Runs;

    const ClipLevel &Top() const noexcept { return Clips.back(); }

    bool Prepare(std::span<const core::Fill> Given, const Transform &Place, float Opacity)
    {
        Fills.clear();
        for (const core::Fill &Each : Given) {
            std::optional<engine::Prepared> Made = engine::Prepared::Of(Each, Place, Opacity);
            if (!Made) return false;
            Fills.push_back(std::move(*Made));
        }
        return true;
    }

    /* Share I of row Y, from X0, at Opacity and within the clip. */
    std::uint32_t Kept(std::uint32_t Share, std::int32_t X, std::int32_t Y, std::uint32_t Opacity, const Mask *Cover)
    {
        if (Share && Opacity < light::Whole) Share = light::Times(Share, Opacity);
        if (Share && Cover) Share = light::Times(Share, light::Widened(Cover->At(X, Y)));
        return Share;
    }

    /* The runs of a row's kept shares — First to Last of them not none:
     * where the shares are none for a stretch nothing is worked, not even a
     * gradient's colours; a gap of a few is worked through, which costs less
     * than stopping. */
    void RunsOf(std::int32_t First, std::int32_t Last)
    {
        constexpr std::int32_t Gap = 16;
        const std::uint16_t   *Share = Shares.data();
        const std::int32_t     Past  = Last + 1;
        Runs.clear();
        for (std::int32_t I = NextCovered(Share, First, Past); I < Past; I = NextCovered(Share, I, Past)) {
            /* A run goes on over every stretch of none shorter than the gap. */
            std::int32_t End = I;
            for (;;) {
                const std::int32_t None = NextUncovered(Share, End, Past);
                const std::int32_t Next = NextCovered(Share, None, Past);
                if (Next >= Past || Next - None >= Gap) {
                    End = None;
                    break;
                }
                End = Next;
            }
            Runs.push_back({I, End});
            I = End;
        }
    }

    /* Row Y's kept shares, from X0, laid with every fill, run by run. A
     * colour that hides what it covers is put, not laid, where it covers
     * whole: the very pixels laying it would make. */
    void LayRuns(Canvas &On, std::int32_t Y, std::int32_t X0)
    {
        const std::uint16_t *Share = Shares.data();
        const lanes::Kit    &Kit   = lanes::Chosen();
        Pixel               *Row   = On.Row(static_cast<std::uint32_t>(Y)) + X0;
        for (const engine::Prepared &Fill : Fills)
            for (const Run &Each : Runs) {
                const std::uint32_t Count = static_cast<std::uint32_t>(Each.End - Each.Begin);
                if (!Fill.Row(Y, X0 + Each.Begin, Count, Paint.data())) {
                    Kit.LayPaint(Row + Each.Begin, Paint.data(), Share + Each.Begin, Count);
                    continue;
                }
                const Pixel Color = Paint[0];
                if (Color.Alpha != light::Whole) {
                    Kit.LayColor(Row + Each.Begin, Share + Each.Begin, Count, Color);
                    continue;
                }
                /* An opaque colour wholly covering a stretch is put, not laid —
                 * a stretch long enough to be worth it: a glyph's few whole
                 * pixels between its edges are laid with them, as they are the
                 * same colour laid whole. */
                constexpr std::int32_t PutLeast = 16;
                for (std::int32_t I = Each.Begin; I < Each.End;) {
                    std::int32_t Whole = NextWhole(Share, I, Each.End), WholeEnd = Whole;
                    while (Whole < Each.End) {
                        WholeEnd = NextPart(Share, Whole, Each.End);
                        if (WholeEnd - Whole >= PutLeast) break;
                        Whole = NextWhole(Share, WholeEnd, Each.End);
                    }
                    if (Whole > I) Kit.LayColor(Row + I, Share + I, static_cast<std::uint32_t>(Whole - I), Color);
                    if (Whole >= Each.End) break;
                    std::fill(Row + Whole, Row + WholeEnd, Color);
                    I = WholeEnd;
                }
            }
    }

    /* The shape swept and laid with the prepared fills — kept within, or
     * without, Edge's fill when a stroke is aligned to its shape. */
    void Lay(Canvas &On, BoxShape &S, BoxShape *EdgeShape, Against How, std::uint32_t Opacity)
    {
        const Rect    Within = Top().Area;
        BoxRect       Held   = BoxShapeReach(&S, BoxRect{Within.X, Within.Y, Within.Width, Within.Height});
        if (How == Against::Within) Held = BoxRectMeet(Held, BoxShapeReach(EdgeShape, Held));
        if (Held.Width <= 0 || Held.Height <= 0 || !Opacity) return;
        const std::size_t Wide = static_cast<std::size_t>(Held.Width);
        Cells.resize(Wide + 2u);
        Shares.resize(Wide);
        Paint.resize(Wide);
        BoxShapeSweep Sweep, EdgeSweep{};
        BoxShapeSweepBegin(&Sweep, &S, Cells.data(), Held.X, Held.X + Held.Width);
        if (How != Against::Nothing) {
            EdgeCells.resize(Wide + 2u);
            BoxShapeSweepBegin(&EdgeSweep, EdgeShape, EdgeCells.data(), Held.X, Held.X + Held.Width);
        }
        const Mask *Clip = Top().Cover.get();
        for (std::int32_t Y = Held.Y; Y < Held.Y + Held.Height; Y++) {
            const bool SameCells = BoxShapeSweepRow(&Sweep, Y) != 0;
            const bool SameEdge  = How == Against::Nothing || BoxShapeSweepRow(&EdgeSweep, Y) != 0;
            /* Cells as the row before's are its shares and runs too — but
             * where a clip's own coverage weighs each pixel. */
            if (SameCells && SameEdge && !Clip) {
                LayRuns(On, Y, Held.X);
                continue;
            }
            std::int64_t Run = Cells[0], EdgeRun = How != Against::Nothing ? EdgeCells[0] : 0;
            std::int32_t First = Held.Width, Last = -1;
            if (How == Against::Nothing && !Clip) {
                /* Where a row's cells are none its coverage holds: each run
                 * of them one share, worked once. */
                for (std::size_t I = 0; I < Wide;) {
                    Run += Cells[I + 1u];
                    std::size_t End = I + 1u;
                    while (End < Wide && Cells[End + 1u] == 0) End++;
                    const std::uint32_t Share = Kept(light::Widened(BoxShapeCoverage(Run, S.Rule)), 0, Y, Opacity, nullptr);
                    std::fill(Shares.begin() + static_cast<std::ptrdiff_t>(I), Shares.begin() + static_cast<std::ptrdiff_t>(End),
                              static_cast<std::uint16_t>(Share));
                    if (Share) {
                        if (First > static_cast<std::int32_t>(I)) First = static_cast<std::int32_t>(I);
                        Last = static_cast<std::int32_t>(End) - 1;
                    }
                    I = End;
                }
                RunsOf(First, Last);
                LayRuns(On, Y, Held.X);
                continue;
            }
            for (std::size_t I = 0; I < Wide; I++) {
                Run += Cells[I + 1u];
                std::uint32_t Cover = BoxShapeCoverage(Run, S.Rule);
                if (How != Against::Nothing) {
                    EdgeRun += EdgeCells[I + 1u];
                    const std::uint32_t Fill = BoxShapeCoverage(EdgeRun, EdgeShape->Rule);
                    Cover = (Cover * (How == Against::Within ? Fill : 255u - Fill) + 127u) / 255u;
                }
                const std::uint32_t Share = Kept(light::Widened(Cover), Held.X + static_cast<std::int32_t>(I), Y, Opacity, Clip);
                Shares[I]                 = static_cast<std::uint16_t>(Share);
                if (Share) {
                    if (First > static_cast<std::int32_t>(I)) First = static_cast<std::int32_t>(I);
                    Last = static_cast<std::int32_t>(I);
                }
            }
            RunsOf(First, Last);
            LayRuns(On, Y, Held.X);
        }
        On.Touch(Rect{Held.X, Held.Y, Held.Width, Held.Height});
    }
};

Canvas::Canvas(Pixel *Memory, std::unique_ptr<Pixel[]> Own, std::uint32_t Width, std::uint32_t Height,
               std::size_t Stride)
    : Pixels(Memory), Owned(std::move(Own)), Wide(Width), High(Height), Stride(Stride), Room(new Work)
{
    Room->Clips.push_back(Work::ClipLevel{Rect{0, 0, static_cast<std::int32_t>(Width), static_cast<std::int32_t>(Height)}, nullptr});
}

Canvas::Canvas(Canvas &&) noexcept            = default;
Canvas &Canvas::operator=(Canvas &&) noexcept = default;
Canvas::~Canvas()                             = default;

std::optional<Canvas> Canvas::Make(std::uint32_t Width, std::uint32_t Height)
{
    if (!Width || !Height || Width > static_cast<std::uint32_t>(BOX_SHAPE_REACH) ||
        Height > static_cast<std::uint32_t>(BOX_SHAPE_REACH))
        return std::nullopt;
    std::unique_ptr<Pixel[]> Own(new (std::nothrow) Pixel[static_cast<std::size_t>(Width) * Height]());
    if (!Own) return std::nullopt;
    Pixel *Memory = Own.get();
    return Canvas(Memory, std::move(Own), Width, Height, Width);
}

std::optional<Canvas> Canvas::Over(Pixel *Memory, std::uint32_t Width, std::uint32_t Height, std::size_t Stride)
{
    if (!Memory || !Width || !Height || Width > static_cast<std::uint32_t>(BOX_SHAPE_REACH) ||
        Height > static_cast<std::uint32_t>(BOX_SHAPE_REACH) || Stride < Width)
        return std::nullopt;
    return Canvas(Memory, nullptr, Width, Height, Stride);
}

void Canvas::Touch(Rect Area) noexcept { Touched = Touched.Join(Area); }

Rect Canvas::TakeChanged() noexcept
{
    const Rect Was = Touched;
    Touched        = Rect{};
    return Was;
}

Rect Canvas::Clip() const noexcept { return Room->Top().Area; }

void Canvas::Clear(Color To)
{
    Clear(Rect{0, 0, static_cast<std::int32_t>(Wide), static_cast<std::int32_t>(High)}, To);
}

void Canvas::Clear(Rect Area, Color To)
{
    const Rect  At    = Area.Intersect(Size{static_cast<std::int32_t>(Wide), static_cast<std::int32_t>(High)});
    const Pixel Plain = Pixel::Of(To);
    for (std::int32_t Y = At.Y; Y < At.Y + At.Height; Y++) {
        Pixel *Line = Row(static_cast<std::uint32_t>(Y)) + At.X;
        std::fill(Line, Line + At.Width, Plain);
    }
    Touch(At);
}

bool Canvas::Fill(const Path &Shape, std::span<const core::Fill> Fills, const Transform &Place, FillRule Rule,
                  float Opacity)
{
    if (!Room->Shape.Fill(Shape, Place, Rule) || !Room->Prepare(Fills, Place, 1.0f)) return false;
    Room->Lay(*this, Room->Shape.Built(), nullptr, Against::Nothing, ShareOf(Opacity));
    return true;
}

bool Canvas::Fill(const Path &Shape, const core::Fill &With, const Transform &Place, FillRule Rule, float Opacity)
{
    return Fill(Shape, std::span<const core::Fill>(&With, 1), Place, Rule, Opacity);
}

bool Canvas::Stroke(const Path &Shape, const core::Stroke &Line, std::span<const core::Fill> Fills,
                    const Transform &Place, float Opacity)
{
    /* Kept within or without the shape's own fill, the line is drawn twice as
     * wide, its half across the edge cut away. */
    const Against How = Line.Align == StrokeAlign::Inside    ? Against::Within
                        : Line.Align == StrokeAlign::Outside ? Against::Without
                                                             : Against::Nothing;
    if (!Room->Shape.Stroke(Shape, Line, Place, How == Against::Nothing ? 1.0f : 2.0f)) return false;
    if (How != Against::Nothing && !Room->Edge.Fill(Shape, Place, FillRule::NonZero)) return false;
    if (!Room->Prepare(Fills, Place, 1.0f)) return false;
    Room->Lay(*this, Room->Shape.Built(), How == Against::Nothing ? nullptr : &Room->Edge.Built(), How,
              ShareOf(Opacity));
    return true;
}

bool Canvas::Stroke(const Path &Shape, const core::Stroke &Line, const core::Fill &With, const Transform &Place,
                    float Opacity)
{
    return Stroke(Shape, Line, std::span<const core::Fill>(&With, 1), Place, Opacity);
}

bool Canvas::FillMask(const Mask &Cover, std::span<const core::Fill> Fills, float Opacity)
{
    return LayMask(Cover, Point{}, Fills, Transform::Identity(), Opacity);
}

bool Canvas::LayMask(const Mask &Cover, Point By, std::span<const core::Fill> Fills, const Transform &Place, float Opacity)
{
    if (!Room->Prepare(Fills, Place, 1.0f)) return false;
    if (!Cover.Empty())
        LayCover(Cover.Row(Cover.Area().Y), static_cast<std::size_t>(Cover.Area().Width),
                 Rect{Cover.Area().X + By.X, Cover.Area().Y + By.Y, Cover.Area().Width, Cover.Area().Height}, Opacity);
    return true;
}

bool Canvas::PrepareFills(std::span<const core::Fill> Fills, const Transform &Place)
{
    return Room->Prepare(Fills, Place, 1.0f);
}

void Canvas::LayCover(const std::uint8_t *Coverage, std::size_t Stride, Rect Lies, float Opacity)
{
    const Rect          At    = Lies.Intersect(Room->Top().Area);
    const std::uint32_t Share = ShareOf(Opacity);
    if (At.Empty() || !Share) return;
    Room->Shares.resize(static_cast<std::size_t>(At.Width));
    Room->Paint.resize(static_cast<std::size_t>(At.Width));
    const Mask *Clip  = Room->Top().Cover.get();
    const bool  Plain = !Clip && Share == light::Whole;
    /* One colour — a glyph's, a shadow's — laid from a row's first pixel
     * covered to its last at once: the very pixels its runs would make. */
    const bool        Solid = Room->Fills.size() == 1 && Room->Fills[0].Solid();
    const lanes::Kit &Kit   = lanes::Chosen();
    for (std::int32_t Y = At.Y; Y < At.Y + At.Height; Y++) {
        const std::uint8_t *Line = Coverage + static_cast<std::size_t>(Y - Lies.Y) * Stride + static_cast<std::size_t>(At.X - Lies.X);
        /* A row's coverage from its first pixel covered to its last: what
         * lies outside covers nothing and is passed over unread. */
        std::int32_t First = FirstCovered(Line, At.Width);
        if (First == At.Width) continue;
        std::int32_t Last = At.Width - 1;
        while (!Line[Last]) Last--;
        std::uint16_t *Shares = Room->Shares.data();
        if (Plain) {
            for (std::int32_t I = First; I <= Last; I++) Shares[I] = static_cast<std::uint16_t>(light::Widened(Line[I]));
        } else {
            for (std::int32_t I = First; I <= Last; I++)
                Shares[I] = static_cast<std::uint16_t>(Room->Kept(light::Widened(Line[I]), At.X + I, Y, Share, Clip));
        }
        if (Solid) {
            Kit.LayColor(Row(static_cast<std::uint32_t>(Y)) + At.X + First, Shares + First,
                         static_cast<std::uint32_t>(Last - First + 1), Room->Fills[0].Color());
            continue;
        }
        Room->RunsOf(First, Last);
        Room->LayRuns(*this, Y, At.X);
    }
    Touch(At);
}

bool Canvas::FillMask(const Mask &Cover, const core::Fill &With, float Opacity)
{
    return FillMask(Cover, std::span<const core::Fill>(&With, 1), Opacity);
}

void Canvas::DrawShadow(const Mask &Kept, const Shadow &Cast, float Opacity, Point By)
{
    const core::Fill Color(Cast.Color);
    LayMask(Kept, By, std::span<const core::Fill>(&Color, 1), Transform::Identity(), Opacity);
}

void Canvas::Draw(const Canvas &Picture, Point At, float Opacity)
{
    const Rect          Into  = Rect{At.X, At.Y, static_cast<std::int32_t>(Picture.Wide), static_cast<std::int32_t>(Picture.High)}
                                   .Intersect(Room->Top().Area);
    const std::uint32_t Share = ShareOf(Opacity);
    if (Into.Empty() || !Share) return;
    const lanes::Kit &Kit   = lanes::Chosen();
    const Mask       *Cover = Room->Top().Cover.get();
    Room->Shares.resize(static_cast<std::size_t>(Into.Width));
    for (std::int32_t Y = Into.Y; Y < Into.Y + Into.Height; Y++) {
        Pixel       *To   = Row(static_cast<std::uint32_t>(Y)) + Into.X;
        const Pixel *From = Picture.Row(static_cast<std::uint32_t>(Y - At.Y)) + (Into.X - At.X);
        if (!Cover) {
            Kit.LayRow(To, From, static_cast<std::uint32_t>(Into.Width), Share);
            continue;
        }
        for (std::int32_t I = 0; I < Into.Width; I++)
            Room->Shares[static_cast<std::size_t>(I)] =
                static_cast<std::uint16_t>(light::Times(Share, light::Widened(Cover->At(Into.X + I, Y))));
        Kit.LayPaint(To, From, Room->Shares.data(), static_cast<std::uint32_t>(Into.Width));
    }
    Touch(Into);
}

void Canvas::FadeTo(const Canvas &Toward, float Share)
{
    const std::uint32_t Across = std::min(Wide, Toward.Wide), Down = std::min(High, Toward.High);
    const std::int64_t  Way  = std::clamp<std::int64_t>(std::lround(Share * 65536.0f), 0, 65536);
    const auto Walk = [Way](std::uint16_t From, std::uint16_t To) {
        return static_cast<std::uint16_t>(From + ((static_cast<std::int64_t>(To) - From) * Way + 32768) / 65536);
    };
    for (std::uint32_t Y = 0; Y < Down; Y++) {
        Pixel       *Into = Row(Y);
        const Pixel *From = Toward.Row(Y);
        for (std::uint32_t X = 0; X < Across; X++)
            Into[X] = Pixel{Walk(Into[X].Red, From[X].Red), Walk(Into[X].Green, From[X].Green), Walk(Into[X].Blue, From[X].Blue),
                            Walk(Into[X].Alpha, From[X].Alpha)};
    }
    Touch(Rect{0, 0, static_cast<std::int32_t>(Across), static_cast<std::int32_t>(Down)});
}

void Canvas::Blur(Rect Area, float Radius)
{
    const engine::BlurBoxes Boxes = engine::BoxesFor(Radius / 2.0f);
    const Rect              At    = Area.Intersect(Room->Top().Area);
    if (At.Empty() || Boxes.Reach == 0) return;
    std::vector<std::uint32_t> Plane(static_cast<std::size_t>(At.Width) * static_cast<std::size_t>(At.Height) * 4u);
    for (std::int32_t Y = 0; Y < At.Height; Y++) {
        const Pixel *Line = Row(static_cast<std::uint32_t>(At.Y + Y)) + At.X;
        for (std::int32_t X = 0; X < At.Width; X++) {
            std::uint32_t *P = &Plane[(static_cast<std::size_t>(Y) * static_cast<std::size_t>(At.Width) + static_cast<std::size_t>(X)) * 4u];
            P[0] = Line[X].Red, P[1] = Line[X].Green, P[2] = Line[X].Blue, P[3] = Line[X].Alpha;
        }
    }
    engine::BlurPixels(Plane.data(), At.Width, At.Height, Boxes);
    const Mask *Cover = Room->Top().Cover.get();
    for (std::int32_t Y = 0; Y < At.Height; Y++) {
        Pixel *Line = Row(static_cast<std::uint32_t>(At.Y + Y)) + At.X;
        for (std::int32_t X = 0; X < At.Width; X++) {
            const std::uint32_t *P = &Plane[(static_cast<std::size_t>(Y) * static_cast<std::size_t>(At.Width) + static_cast<std::size_t>(X)) * 4u];
            const Pixel Blurred{static_cast<std::uint16_t>(P[0]), static_cast<std::uint16_t>(P[1]),
                                static_cast<std::uint16_t>(P[2]), static_cast<std::uint16_t>(P[3])};
            if (!Cover) {
                Line[X] = Blurred;
                continue;
            }
            /* Within a soft clip the blur shows as much as the clip covers:
             * the sharp pixel walked toward the blurred one by it. */
            const std::uint32_t Part = light::Widened(Cover->At(At.X + X, At.Y + Y)), Rest = light::Whole - Part;
            const Pixel         Was  = Line[X];
            Line[X] = Pixel{static_cast<std::uint16_t>(light::Times(Was.Red, Rest) + light::Times(Blurred.Red, Part)),
                            static_cast<std::uint16_t>(light::Times(Was.Green, Rest) + light::Times(Blurred.Green, Part)),
                            static_cast<std::uint16_t>(light::Times(Was.Blue, Rest) + light::Times(Blurred.Blue, Part)),
                            static_cast<std::uint16_t>(light::Times(Was.Alpha, Rest) + light::Times(Blurred.Alpha, Part))};
        }
    }
    Touch(At);
}

void Canvas::PushClip(Rect Area)
{
    const Work::ClipLevel &Was = Room->Top();
    Room->Clips.push_back(Work::ClipLevel{Area.Intersect(Was.Area), Was.Cover});
}

bool Canvas::PushClip(const Path &Shape, const Transform &Place, FillRule Rule)
{
    const Work::ClipLevel &Was  = Room->Top();
    std::optional<Mask>    Made = Mask::Of(Shape, Place, Rule, Was.Area);
    if (!Made) return false;
    if (Was.Cover)
        for (std::int32_t Y = Made->Area().Y; Y < Made->Area().Y + Made->Area().Height; Y++) {
            std::uint8_t *Line = Made->Row(Y);
            for (std::int32_t I = 0; I < Made->Area().Width; I++)
                Line[I] = static_cast<std::uint8_t>((Line[I] * Was.Cover->At(Made->Area().X + I, Y) + 127u) / 255u);
        }
    const Rect Area = Made->Area();
    Room->Clips.push_back(Work::ClipLevel{Area, std::make_shared<const Mask>(std::move(*Made))});
    return true;
}

void Canvas::PopClip()
{
    if (Room->Clips.size() > 1) Room->Clips.pop_back();
}

void Canvas::CopyOut(Rect Area, std::uint32_t *Glass, std::size_t GlassStride, std::uint32_t Format,
                     core::Dither Mode, Point Corner) const
{
    const Rect        At  = Area.Intersect(Size{static_cast<std::int32_t>(Wide), static_cast<std::int32_t>(High)});
    const lanes::Kit &Kit = lanes::Chosen();
    if (At.Empty()) return;
    Room->Thresholds.resize(static_cast<std::size_t>(At.Width));
    for (std::int32_t Y = At.Y; Y < At.Y + At.Height; Y++) {
        if (Mode == core::Dither::On) {
            /* The noise's row, from where the row begins in it, over and
             * over: it repeats every 64 pixels. */
            const std::uint8_t *Noise = light::DitherNoise[(Corner.Y + Y) & 63];
            std::uint32_t       Phase = static_cast<std::uint32_t>(Corner.X + At.X) & 63u;
            for (std::uint32_t Done = 0, Width = static_cast<std::uint32_t>(At.Width); Done < Width; Phase = 0) {
                const std::uint32_t Part = std::min(64u - Phase, Width - Done);
                std::memcpy(Room->Thresholds.data() + Done, Noise + Phase, Part);
                Done += Part;
            }
        }
        Kit.Show(Glass + static_cast<std::size_t>(Y) * GlassStride + static_cast<std::size_t>(At.X),
                 Row(static_cast<std::uint32_t>(Y)) + At.X, static_cast<std::uint32_t>(At.Width), Format,
                 Mode == core::Dither::On ? Room->Thresholds.data() : nullptr);
    }
}

void Canvas::TakePixels(Rect Area, const Pixel *From, std::size_t FromStride)
{
    const Rect        At  = Area.Intersect(Size{static_cast<std::int32_t>(Wide), static_cast<std::int32_t>(High)});
    const lanes::Kit &Kit = lanes::Chosen();
    for (std::int32_t Y = At.Y; Y < At.Y + At.Height; Y++)
        Kit.Held(Row(static_cast<std::uint32_t>(Y)) + At.X, From + static_cast<std::size_t>(Y) * FromStride + static_cast<std::size_t>(At.X),
                 static_cast<std::uint32_t>(At.Width));
    Touch(At);
}

void Canvas::CopyIn(Rect Area, const std::uint32_t *From, std::size_t FromStride)
{
    const Rect At = Area.Intersect(Size{static_cast<std::int32_t>(Wide), static_cast<std::int32_t>(High)});
    for (std::int32_t Y = At.Y; Y < At.Y + At.Height; Y++) {
        Pixel               *Line = Row(static_cast<std::uint32_t>(Y));
        const std::uint32_t *Read = From + static_cast<std::size_t>(Y) * FromStride;
        for (std::int32_t X = At.X; X < At.X + At.Width; X++) Line[X] = light::OfArgb(0xFF000000u | Read[X]);
    }
    Touch(At);
}

}  // namespace painter::core
