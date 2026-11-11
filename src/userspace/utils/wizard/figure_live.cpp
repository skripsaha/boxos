/* figure_live.cpp — the wizard every frame: his sprites blitted with the sway
 * of beard, hem and hat, lit by the room's lamps, and over them what moves
 * on its own — his eyes and brows, his mouth, the staff and its crystal, his
 * arms with the ladle and the vial. */

#include <algorithm>
#include <cmath>

#include "figure.h"
#include "figure_art.h"
#include "shapes.h"

namespace wizard {

namespace {

constexpr int Boost = 540;    // what the sprites' mid-tones are lifted by before the room's light (a 256th)

Color Lit(Color Ink, const Tint &Light)
{
    return Scaled(Ink, Light.Red * Boost >> 8, Light.Green * Boost >> 8, Light.Blue * Boost >> 8);
}

struct Joint {
    float X, Y;
};

// Where the elbow bends when a hand at H is held by a shoulder at S: below the straight line.
Joint ElbowOf(Joint S, Joint H, float Upper, float Fore)
{
    const float Dx = H.X - S.X, Dy = H.Y - S.Y;
    const float Far = std::min(std::hypot(Dx, Dy), (Upper + Fore) * 0.985f);
    const float Reach = std::max(0.01f, std::hypot(Dx, Dy));
    const float Along = (Far * Far + Upper * Upper - Fore * Fore) / (2.0f * Far);
    const float Out = std::sqrt(std::max(0.0f, Upper * Upper - Along * Along));
    const float Ux = Dx / Reach, Uy = Dy / Reach;
    const Joint One{S.X + Ux * Along - Uy * Out, S.Y + Uy * Along + Ux * Out};
    const Joint Two{S.X + Ux * Along + Uy * Out, S.Y + Uy * Along - Ux * Out};
    return One.Y > Two.Y ? One : Two;
}

float TurnOf(const Pose &Now)
{
    return Now.Look * 1.4f;
}

// What the blit does to the row of the sprite at Down (his units): sideways in pixels.
float FrontShift(float Down, const Pose &Now, std::uint64_t Ms, float Unit)
{
    const float Time = static_cast<float>(Ms);
    float Wu = 0.0f;
    if (Down < 20.0f) Wu += 2.6f * std::pow((20.0f - Down) / 20.0f, 2.0f) * Wave(Time / 3300.0f + 0.2f);
    if (Down > 44.0f) Wu += 2.6f * std::pow((Down - 44.0f) / 50.0f, 1.6f) * Wave(Time / 2300.0f + Down * 0.011f);
    if (Down < 44.0f) Wu += TurnOf(Now);
    else if (Down < 58.0f) Wu += TurnOf(Now) * (58.0f - Down) / 14.0f;
    return Wu * Unit;
}

float BackShift(float Down, std::uint64_t Ms, float Unit)
{
    if (Down < 86.0f) return 0.0f;
    return 1.5f * std::pow((Down - 86.0f) / 16.0f, 1.5f) * Wave(static_cast<float>(Ms) / 2900.0f + 0.3f) * Unit;
}

template <class Rows>
void Blit(Canvas &Out, const Scene &Where, const Sprite &Of, const Grid &On, const Lighting &Lights, int Rise, Rows Shift)
{
    const int Left = Where.WizardX - static_cast<int>(std::lround(On.X(0.0f)));
    const int Top = Where.FeetY - static_cast<int>(std::lround(On.Y(100.0f)));
    for (int Row = 0; Row < Of.Height; Row++) {
        const float Down = On.Down(static_cast<float>(Row) + 0.5f);
        const int Aside = static_cast<int>(std::lround(Shift(Down)));
        const int Lift = Down < 50.0f ? Rise : 0;
        for (int Column = 0; Column < Of.Width; Column++) {
            const int Opacity = Of.Opacity(Column, Row);
            if (Opacity == 0) continue;
            const int X = Left + Column + Aside, Y = Top + Row + Lift;
            Out.Cover(X, Y, Lit(Of.Read(Column, Row), Lights.At(X, Y)), static_cast<float>(Opacity) / 255.0f);
        }
    }
}

struct Eyes {
    float Open;       // how tall the eye is, 1 as drawn
    float Raise;      // the brows' lift, in his units
    float Slant;      // how far the brows' inner ends are lowered
    float Pupil;      // the pupil's size, 1 as drawn
};

Eyes EyesOf(const Pose &Now)
{
    Eyes Out{1.0f, 0.0f, 0.0f, 1.0f};
    const float W = Now.FaceWeight;
    switch (Now.Face) {
    case Mood::Pleased:   Out.Open = 1.0f - 0.18f * W; Out.Raise = -0.4f * W; break;
    case Mood::Surprised: Out.Open = 1.0f + 0.40f * W; Out.Raise = -2.0f * W; Out.Pupil = 0.8f; break;
    case Mood::Dizzy:     Out.Open = 1.0f + 0.1f * W; Out.Raise = -0.8f * W; break;
    case Mood::Spooked:   Out.Open = 1.0f + 0.30f * W; Out.Raise = -1.2f * W; Out.Slant = 1.4f * W; Out.Pupil = 0.55f; break;
    case Mood::Squinting: Out.Open = 1.0f - 0.55f * W; Out.Raise = 0.9f * W; Out.Slant = -0.8f * W; break;
    }
    return Out;
}

void DrawEyes(Canvas &Out, const Scene &Where, const Pose &Now, const Tint &Light, float HeadX, float HeadY)
{
    const float Unit = Where.Unit;
    const Eyes Style = EyesOf(Now);
    const float Rx = std::max(0.75f, 2.5f * Unit), Ry = std::max(0.62f, 1.9f * Unit * Style.Open);
    for (const float Side : {-1.0f, 1.0f}) {
        const float X = WizardPixelX(Where, Side * 5.0f) + HeadX, Y = WizardPixelY(Where, 38.4f) + HeadY;
        const float Brow = WizardPixelY(Where, 35.2f + Style.Raise) + HeadY;
        const float Inner = Style.Slant * Unit, Reach = 4.0f * Unit;
        FillStroke(Out, X - Side * Reach * 0.95f, Brow - Inner * -0.2f, X + Side * Reach * 0.9f, Brow + Inner - 0.2f * Unit, std::max(0.8f, 1.15f * Unit), std::max(0.7f, 0.9f * Unit),
                   [&](float Along, float Across, float) { return Lit(OnRamp(BeardTones, 0.75f + 0.12f * Along - 0.2f * Across), Light); });
        if (Now.Blink > 0.62f) {
            FillStroke(Out, X - Rx, Y + 0.3f, X + Rx, Y + 0.3f, 0.5f, 0.5f, [&](float, float, float) { return Lit(SkinTones[0], Light); });
            continue;
        }
        const float Lid = Now.Blink > 0.25f ? 0.55f : 1.0f;
        FillEllipse(Out, X, Y, Rx, Ry * Lid, [&](float, float Ny, float) { return Mixed(Lit(Rgb(0xF4, 0xEE, 0xE2), Light), Lit(SkinTones[1], Light), Ny < -0.5f ? 0.4f : 0.0f); });
        float Px = X + Now.Look * Rx * 0.45f, Py = Y + Now.LookUp * Ry * -0.35f;
        if (Now.Face == Mood::Dizzy) {
            Px = X + Wave(Now.Spin * 3.0f + Side * 0.2f) * Rx * 0.55f;
            Py = Y + Wave(Now.Spin * 3.0f + Side * 0.2f + 0.25f) * Ry * 0.4f;
        }
        const float Iris = std::max(0.62f, 1.25f * Unit * std::min(1.0f, Style.Open));
        FillEllipse(Out, Px, Py, Iris, Iris * Lid, [&](float Nx, float Ny, float) {
            const float Core = std::hypot(Nx, Ny);
            if (Core < 0.45f * Style.Pupil + 0.1f) return Lit(Rgb(0x10, 0x10, 0x18), Light);
            return Lit(Mixed(Rgb(0x28, 0x58, 0x98), Rgb(0x6A, 0xA8, 0xE0), Clamped(0.5f - Ny * 0.5f)), Light);
        });
        Out.Cover(static_cast<int>(Px - Iris * 0.4f), static_cast<int>(Py - Iris * 0.4f), Rgb(0xFF, 0xFF, 0xFF), 0.8f);
    }
}

void DrawMouth(Canvas &Out, const Scene &Where, const Pose &Now, const Tint &Light, float HeadX, float HeadY)
{
    float Open = Now.Speak;
    if (Now.Face == Mood::Surprised) Open = std::max(Open, 0.5f * Now.FaceWeight);
    if (Open < 0.06f) return;
    const float Unit = Where.Unit;
    FillEllipse(Out, WizardPixelX(Where, 0.0f) + HeadX, WizardPixelY(Where, 51.0f) + HeadY, std::max(0.9f, 3.0f * Unit), std::max(0.5f, (0.7f + 2.6f * Open) * Unit),
                [&](float, float Ny, float) { return Ny > 0.25f ? Lit(Rgb(0xC0, 0x50, 0x58), Light) : Rgb(0x34, 0x0C, 0x10); });
}

void DrawStaff(Canvas &Out, const Scene &Where, const Pose &Now, const Show &Shown, const Tint &Light, float Lift)
{
    const float Unit = Where.Unit;
    const float Top = 14.0f - Lift;
    const float X = -38.0f + 1.2f * Now.Cast * Wave(static_cast<float>(Shown.Ms % 700u) / 700.0f);
    const float Thick = std::max(0.85f, 1.9f * Unit);
    FillStroke(Out, WizardPixelX(Where, X), WizardPixelY(Where, Top), WizardPixelX(Where, X - 0.6f), WizardPixelY(Where, 103.0f), Thick, Thick * 1.1f,
               [&](float Along, float Across, float Inside) {
                   float Level = 0.55f - 0.38f * Across + 0.10f * Wave(Along * 16.0f + Across * 0.4f) + 0.06f * Grain(static_cast<int>(Along * 80.0f), static_cast<int>(Across * 3.0f + 5.0f), 231);
                   Color Here = OnRamp(WoodTones, Level);
                   return Inside < 0.8f ? Toned(Lit(Here, Light), 0.8f) : Lit(Here, Light);
               });
    // Three prongs of root hold the crystal.
    for (const float Side : {-1.0f, 0.0f, 1.0f}) {
        FillStroke(Out, WizardPixelX(Where, X), WizardPixelY(Where, Top + 4.0f), WizardPixelX(Where, X + Side * 4.2f), WizardPixelY(Where, Top - 5.0f + std::fabs(Side) * 2.0f),
                   std::max(0.7f, 1.5f * Unit), std::max(0.45f, 0.7f * Unit), [&](float, float Across, float) { return Lit(OnRamp(WoodTones, 0.5f - 0.3f * Across), Light); });
    }
}

// The crystal, lit from within; the stronger the spell, the brighter it burns.
void DrawCrystal(Canvas &Out, const Scene &Where, const Show &Shown, float Lift)
{
    const float Unit = Where.Unit;
    const float X = WizardPixelX(Where, -38.0f), Y = WizardPixelY(Where, 14.0f - Lift - 6.0f);
    const float Pulse = 0.55f + 0.2f * Beat(Shown.Ms, 2300) + 0.6f * Shown.Excite;
    const Color Dark = Mixed(Shown.Glow, Rgb(0x10, 0x08, 0x30), 0.55f), Light = Mixed(Shown.Glow, Rgb(0xFF, 0xFF, 0xFF), 0.35f + 0.4f * Clamped(Pulse - 0.5f));
    Out.Glow(X, Y, Unit * 24.0f * (0.7f + 0.7f * Shown.Excite), Shown.Glow, static_cast<int>(70.0f * Pulse));
    const float Half = std::max(1.2f, 3.8f * Unit), Tall = std::max(2.4f, 8.0f * Unit);
    const auto Facet = [&](float Along, float Across, float Inside) {
        Color Here = Across < -0.25f ? Light : Across > 0.3f ? Dark : Mixed(Shown.Glow, Light, 0.4f);
        Here = Mixed(Here, Rgb(0xFF, 0xFF, 0xFF), Clamped(0.35f - Along * 0.2f) * Pulse * 0.5f);
        return Inside < 0.7f ? Toned(Here, 0.7f) : Here;
    };
    FillStroke(Out, X, Y - Tall, X, Y, 0.45f, Half, Facet);
    FillStroke(Out, X, Y, X, Y + Tall * 0.75f, Half, 0.4f, [&](float Along, float Across, float Inside) { return Toned(Facet(1.0f - Along, Across, Inside), 0.8f); });
    Out.Cover(static_cast<int>(X - Half * 0.4f), static_cast<int>(Y - Tall * 0.35f), Rgb(0xFF, 0xFF, 0xFF), 0.9f);
    if (Shown.Excite > 0.12f) {
        const float Ray = Unit * 18.0f * Shown.Excite;
        const Color Spark = Mixed(Shown.Glow, Rgb(0xFF, 0xFF, 0xFF), 0.6f);
        FillStroke(Out, X - Ray, Y, X + Ray, Y, 0.45f, 0.45f, [&](float Along, float, float) { return Mixed(Spark, Shown.Glow, std::fabs(Along - 0.5f)); });
        FillStroke(Out, X, Y - Ray, X, Y + Ray, 0.45f, 0.45f, [&](float Along, float, float) { return Mixed(Spark, Shown.Glow, std::fabs(Along - 0.5f)); });
    }
}

void DrawHand(Canvas &Out, const Scene &Where, Joint Wrist, float Direction, bool Open, const Tint &Light)
{
    const float Unit = Where.Unit;
    const float Reach = 3.6f;
    const Joint Palm{Wrist.X + std::cos(Direction) * Reach, Wrist.Y + std::sin(Direction) * Reach};
    const auto  Skin = [&](float Nx, float Ny, float Inside) {
        Color Here = OnRamp(SkinTones, 0.62f - 0.20f * Nx - 0.12f * Ny);
        return Lit(Inside < 0.8f ? Toned(Here, 0.75f) : Here, Light);
    };
    if (Open) {
        for (int Finger = -2; Finger <= 1; Finger++) {
            const float Angle = Direction + (static_cast<float>(Finger) + 0.5f) * 0.28f;
            FillStroke(Out, WizardPixelX(Where, Palm.X), WizardPixelY(Where, Palm.Y), WizardPixelX(Where, Palm.X + std::cos(Angle) * 5.2f),
                       WizardPixelY(Where, Palm.Y + std::sin(Angle) * 5.2f), std::max(0.55f, 1.05f * Unit), std::max(0.45f, 0.8f * Unit),
                       [&](float, float Across, float) { return Lit(OnRamp(SkinTones, 0.6f - 0.2f * Across), Light); });
        }
    }
    FillEllipse(Out, WizardPixelX(Where, Palm.X), WizardPixelY(Where, Palm.Y), std::max(0.9f, 2.6f * Unit), std::max(0.9f, 2.9f * Unit), Skin);
}

// One sleeve from the shoulder to the wrist, wide at the cuff, trimmed in gold.
void DrawSleeve(Canvas &Out, const Scene &Where, Joint Shoulder, Joint Elbow, Joint Wrist, const Tint &Light)
{
    const float Unit = Where.Unit;
    const auto  Cloth = [&](float Along, float Across, float Inside) {
        float Level = 0.50f - 0.26f * Across + 0.12f * Wave(Along * 3.0f + Across * 0.3f) + 0.05f * Grain(static_cast<int>(Along * 40.0f), static_cast<int>(Across * 4.0f + 8.0f), 241);
        Color Here = OnRamp(RobeTones, Level);
        return Lit(Inside < 1.0f ? Toned(Here, 0.62f + 0.2f * Inside) : Here, Light);
    };
    const auto P = [&](Joint At, float &X, float &Y) {
        X = WizardPixelX(Where, At.X);
        Y = WizardPixelY(Where, At.Y);
    };
    float Sx, Sy, Ex, Ey, Wx, Wy;
    P(Shoulder, Sx, Sy);
    P(Elbow, Ex, Ey);
    P(Wrist, Wx, Wy);
    FillStroke(Out, Sx, Sy, Ex, Ey, std::max(1.2f, 6.8f * Unit), std::max(1.1f, 5.8f * Unit), Cloth);
    FillStroke(Out, Ex, Ey, Wx, Wy, std::max(1.1f, 5.8f * Unit), std::max(1.3f, 7.6f * Unit), Cloth);
    const float Dx = Wx - Ex, Dy = Wy - Ey, Length = std::max(0.01f, std::hypot(Dx, Dy));
    const float Back = std::max(1.0f, 2.8f * Unit) / Length;
    FillStroke(Out, Wx - Dx * Back, Wy - Dy * Back, Wx, Wy, std::max(1.4f, 7.7f * Unit), std::max(1.4f, 7.7f * Unit), [&](float Along, float Across, float Inside) {
        Color Here = Along > 0.75f ? OnRamp(GoldTones, 0.55f - 0.2f * Across) : OnRamp(RobeTones, 0.62f - 0.3f * Across);
        return Lit(Inside < 0.8f ? Toned(Here, 0.75f) : Here, Light);
    });
}

}  // namespace

void Figure::Draw(Canvas &Out, const Scene &Where, const Pose &Now, const Show &Shown, const Lighting &Lights)
{
    const Grid On{Where.Unit};
    const float Unit = Where.Unit;
    const Tint Body = Lights.At(Where.WizardX, Where.FeetY - static_cast<int>(Unit * 50.0f));
    const int Rise = Unit * 0.8f >= 0.5f ? -static_cast<int>(std::lround(Unit * 0.9f * Now.Breath)) : 0;
    const float HeadX = TurnOf(Now) * Unit, HeadY = static_cast<float>(Rise);
    const float Lift = 11.0f * Now.Cast;

    // A soft shadow under his feet.
    FillEllipse(Out, static_cast<float>(Where.WizardX), static_cast<float>(Where.FeetY) + 1.0f, Unit * 46.0f, std::max(1.6f, Unit * 5.0f),
                [](float, float, float) { return Rgb(0x04, 0x03, 0x06); });

    Blit(Out, Where, Back, On, Lights, 0, [&](float Down) { return BackShift(Down, Shown.Ms, Unit); });
    DrawStaff(Out, Where, Now, Shown, Body, Lift);

    // The arm that holds the staff: its hand on the staff, lifting it when he casts.
    const Joint LeftShoulder{-23.0f, 57.0f}, LeftHand{-38.0f + 1.0f, 66.0f - Lift};
    const Joint LeftElbow = ElbowOf(LeftShoulder, LeftHand, 17.0f, 17.0f);
    DrawSleeve(Out, Where, LeftShoulder, LeftElbow, LeftHand, Body);
    DrawHand(Out, Where, LeftHand, 0.4f, false, Body);

    Blit(Out, Where, Front, On, Lights, Rise, [&](float Down) { return FrontShift(Down, Now, Shown.Ms, Unit); });
    DrawEyes(Out, Where, Now, Body, HeadX, HeadY);
    DrawMouth(Out, Where, Now, Body, HeadX, HeadY);
    DrawCrystal(Out, Where, Shown, Lift);

    // The other arm: at rest, stirring, casting or drinking, whichever he does most.
    const float Total = std::max(1.0f, Now.Stir + Now.Cast + Now.Drink);
    const float Rest = 1.0f - Clamped(Now.Stir + Now.Cast + Now.Drink);
    const Joint StirHand{40.0f + 3.0f * Wave(Now.Spin), 69.0f + 2.0f * Wave(Now.Spin + 0.25f)};
    const Joint CastHand{46.0f, 30.0f + 2.0f * Wave(static_cast<float>(Shown.Ms % 900u) / 900.0f)};
    const Joint DrinkHand{15.0f, 50.0f};
    const Joint Idle{34.0f + 0.6f * Wave(static_cast<float>(Shown.Ms % 4100u) / 4100.0f), 79.0f - 0.8f * Now.Breath};
    const Joint RightShoulder{23.0f, 57.0f};
    const Joint RightHand{(Idle.X * Rest + StirHand.X * Now.Stir / Total + CastHand.X * Now.Cast / Total + DrinkHand.X * Now.Drink / Total),
                          Idle.Y * Rest + StirHand.Y * Now.Stir / Total + CastHand.Y * Now.Cast / Total + DrinkHand.Y * Now.Drink / Total};
    const Joint RightElbow = ElbowOf(RightShoulder, RightHand, 17.0f, 17.0f);
    if (Now.Stir > 0.05f) {
        const Spot Pot = SurfaceOf(Where);
        const float Tip = Pot.Y + std::max(1.0f, Unit * 2.0f);
        FillStroke(Out, WizardPixelX(Where, RightHand.X), WizardPixelY(Where, RightHand.Y), Pot.X + 2.5f * Wave(Now.Spin), Tip, std::max(0.8f, Unit * 1.1f), std::max(0.7f, Unit * 0.9f),
                   [&](float, float Across, float) { return Lit(OnRamp(WoodTones, 0.6f - 0.3f * Across), Body); });
    }
    DrawSleeve(Out, Where, RightShoulder, RightElbow, RightHand, Body);
    DrawHand(Out, Where, RightHand, Now.Cast > 0.4f ? -1.2f : 0.5f, Now.Cast > 0.4f, Body);
    if (Now.Drink > 0.05f) {
        const float X = WizardPixelX(Where, RightHand.X + 3.0f), Y = WizardPixelY(Where, RightHand.Y - 3.0f);
        const Color Brew = Color::FromRaw(Shown.Brew == 0 ? 0x70B090u : Shown.Brew);
        FillStroke(Out, X, Y, X + 1.5f * Unit, Y - 9.0f * Unit, std::max(0.9f, 2.4f * Unit), std::max(0.9f, 2.2f * Unit), [&](float Along, float Across, float Inside) {
            Color Here = Along > 0.85f ? Rgb(0x90, 0x60, 0x30) : Mixed(Rgb(0xB8, 0xD8, 0xE8), Brew, 0.78f);
            if (Across < -0.4f) Here = Lighter(Here, 80);
            return Lit(Inside < 0.7f ? Toned(Here, 0.7f) : Here, Body);
        });
    }
}

}  // namespace wizard
