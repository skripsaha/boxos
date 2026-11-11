/* room.cpp — the room: painted once, lit every frame. */

#include "room.h"

#include <algorithm>

#include "room_parts.h"

namespace wizard {

namespace {

// What the room is lit with when nothing burns: dim and a little cool.
constexpr Tint Dim{74, 70, 80};

Color Dimmed(Color Of, int Percent)
{
    return Scaled(Of, 256 * Percent / 100, 256 * Percent / 100, 256 * Percent / 100);
}

}  // namespace

void Room::Build(const Scene &Where, const Look &Shown)
{
    Place = Where;
    Albedo = Canvas(Where.Width, Where.Height);
    PaintWall(Albedo, Where, Shown.Room);
    PaintFloor(Albedo, Where, Shown.Floor);
    PaintRug(Albedo, Where);
    PaintBeam(Albedo, Where);
    PaintBooks(Albedo, Where);
    PaintWindowFrame(Albedo, Where, Shown.Room);
    PaintJars(Albedo, Where);
    PaintStands(Albedo, Where);

    const float Unit = Where.Unit;
    std::vector<Lamp> Lamps(LampCount);
    Lamps[LampFire] = Lamp{LampKind::Round, static_cast<float>(Where.CauldronX), static_cast<float>(Where.FeetY) - Unit * 8.0f,
                           static_cast<float>(Where.Width) * 0.46f, Shown.Fire, 0.0f, 0.0f};
    for (int Which = 0; Which < 3; Which++)
        Lamps[LampFloorCandle + static_cast<std::size_t>(Which)] =
            Lamp{LampKind::Round, static_cast<float>(Where.CandleX[Which]), static_cast<float>(FlameRow(Where, Which)),
                 Unit * (Which == 0 ? 52.0f : 40.0f), Shown.Candle, 0.0f, 0.0f};
    const Box W = Where.Window;
    Lamps[LampMoon] = Lamp{LampKind::Shaft, static_cast<float>(W.X + W.Width / 2), static_cast<float>(W.Y + W.Height / 2),
                           static_cast<float>(Where.FeetY - W.Y - W.Height / 2), Dimmed(Shown.Moon, 60), -0.55f, static_cast<float>(W.Width) * 0.9f};
    Lamps[LampCrystal] = Lamp{LampKind::Round, WizardPixelX(Where, -38.0f), WizardPixelY(Where, 6.0f), Unit * 58.0f, Dimmed(Shown.Glow, 70), 0.0f, 0.0f};
    Lights.Build(Where.Width, Where.Height, Lamps, Dim);
}

void Room::Draw(Canvas &Out, const Show &Now)
{
    Lights.Set(LampFire, Flicker(Now.Ms, 0, 250, 90) + static_cast<int>(Now.Excite * 40.0f));
    for (std::size_t Which = 0; Which < 3; Which++) Lights.Set(LampFloorCandle + Which, Flicker(Now.Ms, static_cast<std::uint32_t>(Which) + 1, 230, 70));
    Lights.Set(LampMoon, 256);
    Lights.Set(LampCrystal, 40 + static_cast<int>(Now.Excite * 380.0f) + static_cast<int>(Beat(Now.Ms, 2300) * 40.0f));
    Lights.Apply(Albedo, Out);

    DrawSky(Out, Place, Now);
    DrawCircle(Out, Place, Now);
    DrawHerbs(Out, Place, Now, Lights);
    DrawFlames(Out, Place, Now);
    if (!Now.Critters) return;
    DrawOwl(Out, Place, Now, Lights);
    DrawCat(Out, Place, Now, Lights);
}

}  // namespace wizard
