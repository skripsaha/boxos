/*
 * path.cpp — a shape's outline, its arcs as curves, and SVG's path data read
 * into one. See painter/core/path.h.
 */

#include "painter/core/path.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace painter::core {

namespace {

constexpr float Pi = 3.14159265358979f;

}  // namespace

void Path::Begin()
{
    if (Open) return;
    StepList.push_back(Step::Move);
    PointList.push_back(At);
    Start = At;
    Open  = true;
}

Path &Path::MoveTo(PointF To)
{
    StepList.push_back(Step::Move);
    PointList.push_back(To);
    Start = At = To;
    Open       = true;
    return *this;
}

Path &Path::LineTo(PointF To)
{
    Begin();
    StepList.push_back(Step::Line);
    PointList.push_back(To);
    At = To;
    return *this;
}

Path &Path::QuadTo(PointF Pull, PointF To)
{
    Begin();
    StepList.push_back(Step::Quad);
    PointList.insert(PointList.end(), {Pull, To});
    At = To;
    return *this;
}

Path &Path::CubicTo(PointF Pull1, PointF Pull2, PointF To)
{
    Begin();
    StepList.push_back(Step::Cubic);
    PointList.insert(PointList.end(), {Pull1, Pull2, To});
    At = To;
    return *this;
}

/* SVG's arc (SVG 1.1, F.6.5 and F.6.6): its ends to its middle and turns, the
 * radii grown when they cannot reach, then each quarter turn or less a cubic
 * whose pulls lie 4/3·tan(θ/4) of the radius along the tangents. */
Path &Path::ArcTo(float RadiusX, float RadiusY, float TurnDegrees, bool Large, bool Clockwise, PointF To)
{
    const PointF From = At;
    float        RX = std::fabs(RadiusX), RY = std::fabs(RadiusY);
    if ((From.X == To.X && From.Y == To.Y)) return *this;
    if (RX == 0.0f || RY == 0.0f) return LineTo(To);
    const float Phi = TurnDegrees * Pi / 180.0f, Cos = std::cos(Phi), Sin = std::sin(Phi);
    const float HX = (From.X - To.X) / 2.0f, HY = (From.Y - To.Y) / 2.0f;
    const float X1 = Cos * HX + Sin * HY, Y1 = -Sin * HX + Cos * HY;
    const float Reach = (X1 * X1) / (RX * RX) + (Y1 * Y1) / (RY * RY);
    if (Reach > 1.0f) {
        RX *= std::sqrt(Reach);
        RY *= std::sqrt(Reach);
    }
    const float Top  = RX * RX * RY * RY - RX * RX * Y1 * Y1 - RY * RY * X1 * X1;
    const float Down = RX * RX * Y1 * Y1 + RY * RY * X1 * X1;
    float       Root = Down > 0.0f ? std::sqrt(std::max(Top, 0.0f) / Down) : 0.0f;
    if (Large == Clockwise) Root = -Root;
    const float CX1 = Root * RX * Y1 / RY, CY1 = -Root * RY * X1 / RX;
    const float CX = Cos * CX1 - Sin * CY1 + (From.X + To.X) / 2.0f;
    const float CY = Sin * CX1 + Cos * CY1 + (From.Y + To.Y) / 2.0f;
    const auto  Angle = [](float UX, float UY, float VX, float VY) {
        return std::atan2(UX * VY - UY * VX, UX * VX + UY * VY);
    };
    const float Start = Angle(1.0f, 0.0f, (X1 - CX1) / RX, (Y1 - CY1) / RY);
    float       Sweep = Angle((X1 - CX1) / RX, (Y1 - CY1) / RY, (-X1 - CX1) / RX, (-Y1 - CY1) / RY);
    if (!Clockwise && Sweep > 0.0f) Sweep -= 2.0f * Pi;
    if (Clockwise && Sweep < 0.0f) Sweep += 2.0f * Pi;

    const int   Pieces = std::max(1, static_cast<int>(std::ceil(std::fabs(Sweep) / (Pi / 2.0f) - 1.0e-4f)));
    const float Each   = Sweep / static_cast<float>(Pieces);
    const float Pull   = 4.0f / 3.0f * std::tan(Each / 4.0f);
    const auto  On     = [&](float T) {
        const float X = RX * std::cos(T), Y = RY * std::sin(T);
        return PointF{Cos * X - Sin * Y + CX, Sin * X + Cos * Y + CY};
    };
    const auto Tangent = [&](float T) {
        const float X = -RX * std::sin(T), Y = RY * std::cos(T);
        return PointF{Cos * X - Sin * Y, Sin * X + Cos * Y};
    };
    float T = Start;
    for (int I = 0; I < Pieces; I++) {
        const float  Next = T + Each;
        const PointF A = On(T), B = I + 1 == Pieces ? To : On(Next);
        const PointF TA = Tangent(T), TB = Tangent(Next);
        CubicTo({A.X + Pull * TA.X, A.Y + Pull * TA.Y}, {B.X - Pull * TB.X, B.Y - Pull * TB.Y}, B);
        T = Next;
    }
    return *this;
}

Path &Path::Close()
{
    if (!Open) return *this;
    StepList.push_back(Step::Close);
    At   = Start;
    Open = false;
    return *this;
}

Path &Path::AddRect(RectF Area)
{
    StepList.push_back(Step::Rect);
    PointList.insert(PointList.end(), {{Area.X, Area.Y}, {Area.Width, Area.Height}});
    Open = false;
    At = Start = {Area.X, Area.Y};
    return *this;
}

Path &Path::AddRoundedRect(RectF Area, Corners Round)
{
    StepList.push_back(Step::Rounded);
    PointList.insert(PointList.end(), {{Area.X, Area.Y},
                                       {Area.Width, Area.Height},
                                       {Round.TopLeft, Round.TopRight},
                                       {Round.BottomRight, Round.BottomLeft}});
    Open = false;
    At = Start = {Area.X, Area.Y};
    return *this;
}

Path &Path::AddEllipse(PointF Middle, float RadiusX, float RadiusY)
{
    StepList.push_back(Step::Ellipse);
    PointList.insert(PointList.end(), {Middle, {RadiusX, RadiusY}});
    Open = false;
    At = Start = Middle;
    return *this;
}

RectF Path::Bounds() const noexcept
{
    float       Left = std::numeric_limits<float>::max(), Top = Left;
    float       Right = std::numeric_limits<float>::lowest(), Bottom = Right;
    const auto  Hold  = [&](float X, float Y) {
        Left = std::min(Left, X), Right = std::max(Right, X);
        Top = std::min(Top, Y), Bottom = std::max(Bottom, Y);
    };
    std::size_t Next = 0;
    for (Step Each : StepList) {
        switch (Each) {
        case Step::Move:
        case Step::Line: Hold(PointList[Next].X, PointList[Next].Y); Next += 1; break;
        case Step::Quad:
            for (int I = 0; I < 2; I++) Hold(PointList[Next + I].X, PointList[Next + I].Y);
            Next += 2;
            break;
        case Step::Cubic:
            for (int I = 0; I < 3; I++) Hold(PointList[Next + I].X, PointList[Next + I].Y);
            Next += 3;
            break;
        case Step::Close: break;
        case Step::Rect:
        case Step::Rounded:
            Hold(PointList[Next].X, PointList[Next].Y);
            Hold(PointList[Next].X + PointList[Next + 1].X, PointList[Next].Y + PointList[Next + 1].Y);
            Next += Each == Step::Rect ? 2 : 4;
            break;
        case Step::Ellipse: {
            const PointF M = PointList[Next], R = PointList[Next + 1];
            Hold(M.X - std::fabs(R.X), M.Y - std::fabs(R.Y));
            Hold(M.X + std::fabs(R.X), M.Y + std::fabs(R.Y));
            Next += 2;
            break;
        }
        }
    }
    if (Left > Right) return {};
    return {Left, Top, Right - Left, Bottom - Top};
}

/* ---------------------------------------------------------------------------
 * SVG's path data
 * ------------------------------------------------------------------------- */

namespace {

class SvgReader {
public:
    explicit SvgReader(std::string_view Data) : Text(Data) {}

    void Skip()
    {
        while (At < Text.size() && (Text[At] == ' ' || Text[At] == '\t' || Text[At] == '\n' || Text[At] == '\r' ||
                                    Text[At] == '\f' || Text[At] == ','))
            At++;
    }
    bool Done()
    {
        Skip();
        return At >= Text.size();
    }
    /* A command letter next, or none. */
    char Command()
    {
        Skip();
        if (At < Text.size() && std::string_view("MmLlHhVvCcSsQqTtAaZz").find(Text[At]) != std::string_view::npos)
            return Text[At++];
        return 0;
    }
    /* Whether a number comes next. */
    bool NumberNext()
    {
        Skip();
        if (At >= Text.size()) return false;
        const char C = Text[At];
        return (C >= '0' && C <= '9') || C == '-' || C == '+' || C == '.';
    }
    /* A number as SVG writes one: a sign, digits with a point among them or
     * before them, and an exponent — the digits past the nineteenth only
     * counted, a float's worth being far fewer. */
    bool Number(float &Value)
    {
        Skip();
        bool Negative = false;
        if (At < Text.size() && (Text[At] == '+' || Text[At] == '-')) Negative = Text[At++] == '-';
        std::uint64_t Digits = 0;
        int           Scale = 0, Seen = 0, Kept = 0;
        const auto    Take  = [&](bool Fraction) {
            while (At < Text.size() && Text[At] >= '0' && Text[At] <= '9') {
                if (Kept < 19) {
                    Digits = Digits * 10u + static_cast<std::uint64_t>(Text[At] - '0');
                    if (Digits) Kept++;
                    if (Fraction) Scale--;
                } else if (!Fraction) {
                    Scale++;
                }
                Seen++;
                At++;
            }
        };
        Take(false);
        if (At < Text.size() && Text[At] == '.') {
            At++;
            Take(true);
        }
        if (!Seen) return false;
        if (At < Text.size() && (Text[At] == 'e' || Text[At] == 'E')) {
            std::size_t Back = At++;
            bool        Down = false;
            if (At < Text.size() && (Text[At] == '+' || Text[At] == '-')) Down = Text[At++] == '-';
            int Exponent = 0, Count = 0;
            while (At < Text.size() && Text[At] >= '0' && Text[At] <= '9') {
                if (Exponent < 10000) Exponent = Exponent * 10 + (Text[At] - '0');
                Count++;
                At++;
            }
            if (!Count) At = Back;
            else        Scale += Down ? -Exponent : Exponent;
        }
        const double Read = static_cast<double>(Digits) * std::pow(10.0, Scale);
        Value             = static_cast<float>(Negative ? -Read : Read);
        return std::isfinite(Value);
    }
    /* An arc's flag: one digit, 0 or 1, needing no space after it. */
    bool Flag(bool &Value)
    {
        Skip();
        if (At >= Text.size() || (Text[At] != '0' && Text[At] != '1')) return false;
        Value = Text[At++] == '1';
        return true;
    }

private:
    std::string_view Text;
    std::size_t      At = 0;
};

}  // namespace

std::optional<Path> Path::FromSvg(std::string_view Data)
{
    SvgReader Reader(Data);
    Path      Made;
    PointF    Pen{}, Begun{}, LastPull{};
    char      Last = 0;
    while (!Reader.Done()) {
        const char Command = Reader.Command();
        if (!Command) return std::nullopt;
        const bool Relative = Command >= 'a' && Command <= 'z';
        const char Upper    = Relative ? static_cast<char>(Command - 'a' + 'A') : Command;
        const auto Place    = [&](float X, float Y) { return Relative ? PointF{Pen.X + X, Pen.Y + Y} : PointF{X, Y}; };
        if (Upper == 'Z') {
            Made.Close();
            Pen  = Begun;
            Last = 'Z';
            continue;
        }
        bool First = true;
        do {
            float N[7];
            const auto Read = [&](int Count) {
                for (int I = 0; I < Count; I++)
                    if (!Reader.Number(N[I])) return false;
                return true;
            };
            switch (Upper) {
            case 'M':
                if (!Read(2)) return std::nullopt;
                Pen = Place(N[0], N[1]);
                if (First) {
                    Made.MoveTo(Pen);
                    Begun = Pen;
                } else {
                    Made.LineTo(Pen);
                }
                break;
            case 'L':
                if (!Read(2)) return std::nullopt;
                Pen = Place(N[0], N[1]);
                Made.LineTo(Pen);
                break;
            case 'H':
                if (!Read(1)) return std::nullopt;
                Pen.X = Relative ? Pen.X + N[0] : N[0];
                Made.LineTo(Pen);
                break;
            case 'V':
                if (!Read(1)) return std::nullopt;
                Pen.Y = Relative ? Pen.Y + N[0] : N[0];
                Made.LineTo(Pen);
                break;
            case 'C': {
                if (!Read(6)) return std::nullopt;
                const PointF P1 = Place(N[0], N[1]), P2 = Place(N[2], N[3]), To = Place(N[4], N[5]);
                Made.CubicTo(P1, P2, To);
                LastPull = P2;
                Pen      = To;
                break;
            }
            case 'S': {
                if (!Read(4)) return std::nullopt;
                const bool   Follows = Last == 'C' || Last == 'S';
                const PointF P1 = Follows ? PointF{2 * Pen.X - LastPull.X, 2 * Pen.Y - LastPull.Y} : Pen;
                const PointF P2 = Place(N[0], N[1]), To = Place(N[2], N[3]);
                Made.CubicTo(P1, P2, To);
                LastPull = P2;
                Pen      = To;
                break;
            }
            case 'Q': {
                if (!Read(4)) return std::nullopt;
                const PointF P = Place(N[0], N[1]), To = Place(N[2], N[3]);
                Made.QuadTo(P, To);
                LastPull = P;
                Pen      = To;
                break;
            }
            case 'T': {
                if (!Read(2)) return std::nullopt;
                const bool   Follows = Last == 'Q' || Last == 'T';
                const PointF P  = Follows ? PointF{2 * Pen.X - LastPull.X, 2 * Pen.Y - LastPull.Y} : Pen;
                const PointF To = Place(N[0], N[1]);
                Made.QuadTo(P, To);
                LastPull = P;
                Pen      = To;
                break;
            }
            case 'A': {
                bool Large, Clockwise;
                if (!Read(3) || !Reader.Flag(Large) || !Reader.Flag(Clockwise) || !Reader.Number(N[3]) ||
                    !Reader.Number(N[4]))
                    return std::nullopt;
                const PointF To = Place(N[3], N[4]);
                Made.ArcTo(N[0], N[1], N[2], Large, Clockwise, To);
                Pen = To;
                break;
            }
            default: return std::nullopt;
            }
            Last  = Upper == 'M' && !First ? 'L' : Upper;
            First = false;
        } while (Reader.NumberNext());
    }
    return Made;
}

}  // namespace painter::core
