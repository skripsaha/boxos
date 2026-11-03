/* aquarium_paint.cpp — the tank through the glass.
 *
 * The window is painted back to front: the water and the light falling into
 * it, the far hills (half as fast as the tank, so they read as far), the sand,
 * the stones and plants, the chest, the food, what lives there, the bubbles.
 * Everything takes the light of its depth — the deep is darker, the night
 * darker still — but what glows: at night the jellyfish and the lanternfish
 * shine with their own light and colour the water round them. */

#include <algorithm>

#include "box/vga.h"

#include "aquarium.h"

namespace {

constexpr char UpperHalfBlock = static_cast<char>(0xDF);

const Color WaterTop    = COLOR_RGB(0x2A, 0x78, 0xC0);
const Color WaterDeep   = COLOR_RGB(0x06, 0x20, 0x48);
const Color Air         = COLOR_RGB(0x14, 0x1C, 0x26);
const Color RayInk      = COLOR_RGB(0x8C, 0xD0, 0xFF);
const Color SurfaceInk  = COLOR_RGB(0x5A, 0xA8, 0xE0);
const Color SparkleInk  = COLOR_RGB(0xC8, 0xEC, 0xFF);
const Color HillInk     = COLOR_RGB(0x0A, 0x2C, 0x4C);
const Color SandLight   = COLOR_RGB(0xC8, 0xA4, 0x64);
const Color SandDark    = COLOR_RGB(0x9A, 0x7A, 0x44);
const Color SandLit     = COLOR_RGB(0xF0, 0xD8, 0x98);
const Color BubbleInk   = COLOR_RGB(0xC8, 0xF0, 0xFF);
const Color White       = COLOR_RGB(0xF0, 0xF0, 0xF0);
const Color NightInk    = COLOR_RGB(0x00, 0x06, 0x18);
const Color WoodInk     = COLOR_RGB(0x7A, 0x4A, 0x1A);
const Color WoodDark    = COLOR_RGB(0x4A, 0x2A, 0x0C);
const Color GoldInk     = COLOR_RGB(0xE8, 0xB8, 0x30);
const Color TreasureInk = COLOR_RGB(0xFF, 0xE0, 0x50);
const Color StoneInk    = COLOR_RGB(0x6A, 0x6A, 0x72);
const Color AnemoneFoot = COLOR_RGB(0xA0, 0x40, 0x80);
const Color AnemoneInk  = COLOR_RGB(0xFF, 0x70, 0xB0);
const Color AnemoneTip  = COLOR_RGB(0xFF, 0xD0, 0xE8);
const Color CaveInk     = COLOR_RGB(0x0A, 0x0A, 0x12);
const Color MossInk     = COLOR_RGB(0x4A, 0x7A, 0x3A);

std::uint32_t Hash(int X, int Y)
{
    std::uint32_t Value = static_cast<std::uint32_t>(X) * 73856093u ^ static_cast<std::uint32_t>(Y) * 19349663u;
    Value ^= Value >> 13;
    return Value * 0x5BD1E995u;
}

/* The picture being made: the window's pixels and what lights them. */
class Glass {
public:
    Glass(const AquaTank &Tank, const AquaView &View, const AquaDaylight &Daylight, std::vector<Color> &Pixels)
        : Tank(Tank), Daylight(Daylight), Width(View.Width), Height(View.Height), Left(static_cast<int>(View.X)),
          Top(static_cast<int>(View.Y)), Pixels(Pixels), Lights(static_cast<std::size_t>(Tank.Height))
    {
        Pixels.assign(static_cast<std::size_t>(Width) * Height, COLOR_BLACK);
        /* The sun reaches every depth less; the night leaves a little, enough
         * to see by. */
        int Sun   = 96 + Daylight.Sun * 160 / 256;
        int Water = std::max(1, Tank.Height - Tank.Surface);
        for (int Y = 0; Y < Tank.Height; Y++) {
            int Depth = std::clamp(Y - Tank.Surface, 0, Water);
            Lights[static_cast<std::size_t>(Y)] = Sun * (256 - 100 * Depth / Water) / 256;
        }
    }

    const AquaTank     &Tank;
    const AquaDaylight &Daylight;

    /* Lit by the depth it stands at; a glow keeps its own light. */
    Color Lit(Color Ink, int Y) const
    {
        int   Light = Lights[static_cast<std::size_t>(std::clamp(Y, 0, Tank.Height - 1))];
        Color Shown = AquaShade(Ink, Light);
        return Daylight.Night ? color_mix(Shown, NightInk, 60) : Shown;
    }

    bool Inside(int X, int Y) const
    {
        return X >= Left && X < Left + Width && Y >= Top && Y < Top + Height;
    }

    Color &At(int X, int Y) { return Pixels[static_cast<std::size_t>(Y - Top) * Width + (X - Left)]; }

    void Put(int X, int Y, Color Ink)
    {
        if (Inside(X, Y)) At(X, Y) = Ink;
    }

    void PutLit(int X, int Y, Color Ink) { Put(X, Y, Lit(Ink, Y)); }

    /* `Weight` 256ths of Ink over what is there. */
    void Blend(int X, int Y, Color Ink, int Weight)
    {
        if (Inside(X, Y)) At(X, Y) = color_mix(At(X, Y), Ink, static_cast<std::uint8_t>(std::clamp(Weight, 0, 255)));
    }

    int Width, Height, Left, Top;

private:
    std::vector<Color> &Pixels;
    std::vector<int>    Lights;
};

/* ---- the water ------------------------------------------------------------ */

/* The water at a pixel: its colour by depth, the sun's rays slanting down
 * through it, the far hills behind. */
Color Water(const Glass &View, int X, int Y, int FarTop)
{
    const AquaTank &Tank = View.Tank;
    if (Y < Tank.Surface) return Air;

    float Depth = static_cast<float>(Y - Tank.Surface) / static_cast<float>(Tank.Height - Tank.Surface);
    Color Ink   = color_mix(WaterTop, WaterDeep, static_cast<std::uint8_t>(std::min(255.0f, Depth * 255.0f)));
    if (Y >= FarTop) Ink = color_mix(Ink, HillInk, 150);

    if (Y == Tank.Surface)
        Ink = AquaWave(X / 9.0f + Tank.Time * 0.4f) > 0.55f ? SparkleInk : SurfaceInk;

    float Fade = 1.0f - Depth * 1.7f;
    if (Fade > 0 && View.Daylight.Sun > 0) {
        float Slant = static_cast<float>(X) + static_cast<float>(Y) * 0.45f;
        float Ray   = 0.5f * AquaWave(Slant / 48.0f + Tank.Time * 0.02f) +
                      0.5f * AquaWave(Slant / 23.0f - Tank.Time * 0.035f) - 0.35f;
        if (Ray > 0) Ink = color_mix(Ink, RayInk, static_cast<std::uint8_t>(Ray / 0.65f * Fade * 110.0f * View.Daylight.Sun / 256));
    }
    return View.Lit(Ink, Y);
}

void PaintWater(Glass &View)
{
    const AquaTank &Tank = View.Tank;
    int FarBase = Tank.Height - AquaSandDepth(Tank);
    for (int Col = 0; Col < View.Width; Col++) {
        int X      = View.Left + Col;
        int FarCol = std::clamp(View.Left / 2 + Col, 0, static_cast<int>(Tank.Scene.Far.size()) - 1);
        int FarTop = FarBase - Tank.Scene.Far[static_cast<std::size_t>(FarCol)];
        for (int Row = 0; Row < View.Height; Row++) View.At(X, View.Top + Row) = Water(View, X, View.Top + Row, FarTop);
    }
}

/* ---- what stands still ------------------------------------------------------- */

void PaintSand(Glass &View)
{
    const AquaTank &Tank = View.Tank;
    for (int Col = 0; Col < View.Width; Col++) {
        int X     = View.Left + Col;
        int Floor = AquaFloor(Tank, X);
        for (int Y = std::max(Floor, View.Top); Y < View.Top + View.Height; Y++) {
            Color Grain = (Hash(X, Y) >> 29) == 0 ? SandDark : color_mix(SandLight, SandDark, static_cast<std::uint8_t>((Y - Floor) * 18));
            if (Y - Floor < 3 && View.Daylight.Sun > 0) {
                float Caustic = AquaWave(X / 7.0f + Tank.Time * 0.3f) * AquaWave(X / 4.3f - Tank.Time * 0.41f);
                if (Caustic > 0.45f) Grain = color_mix(Grain, SandLit, static_cast<std::uint8_t>(140 * View.Daylight.Sun / 256));
            }
            View.PutLit(X, Y, Grain);
        }
    }
    for (const AquaShell &Shell : Tank.Scene.Shells) {
        View.PutLit(Shell.X, Shell.Y, Shell.Ink);
        View.PutLit(Shell.X + 1, Shell.Y, color_mix(Shell.Ink, SandDark, 90));
    }
}

void PaintRock(Glass &View, const AquaRock &Rock)
{
    float Rx = static_cast<float>(Rock.Width) / 2.0f;
    float Ry = static_cast<float>(Rock.Height);
    float Cx = static_cast<float>(Rock.X) + Rx;
    float Cy = static_cast<float>(Rock.Y + Rock.Height);
    for (int Y = Rock.Y; Y < Rock.Y + Rock.Height; Y++) {
        for (int X = Rock.X; X < Rock.X + Rock.Width; X++) {
            float Dx = (static_cast<float>(X) + 0.5f - Cx) / Rx;
            float Dy = (static_cast<float>(Y) + 0.5f - Cy) / Ry;
            if (Dx * Dx + Dy * Dy > 1.0f) continue;
            Color Ink = Rock.Ink;
            if (Dy < -0.55f && Dx < 0.2f) Ink = color_mix(Ink, White, 50);
            if (Dx > 0.45f) Ink = color_mix(Ink, COLOR_BLACK, 70);
            if (Dy < -0.8f && (Hash(X, Y) >> 30) == 0) Ink = MossInk;
            if ((Hash(X, Y) >> 28) == 0) Ink = color_mix(Ink, COLOR_BLACK, 50);
            if (Rock.Cave) {
                float Hx = Dx / 0.42f, Hy = (Dy + 0.05f) / 0.55f;
                if (Hx * Hx + Hy * Hy < 1.0f && Dy > -0.6f) Ink = CaveInk;
            }
            View.PutLit(X, Y, Ink);
        }
    }
}

void PaintStalks(Glass &View)
{
    const AquaTank &Tank = View.Tank;
    for (const AquaStalk &Stalk : Tank.Scene.Stalks) {
        int   Floor = AquaFloor(Tank, Stalk.X);
        float Reach = Stalk.Kelp ? 3.0f : 1.0f;
        for (int Rise = 0; Rise < Stalk.Height; Rise++) {
            float Up    = static_cast<float>(Rise) / static_cast<float>(Stalk.Height);
            float Sway  = AquaWave(Tank.Time * 0.18f + Stalk.Phase + Rise * 0.035f) * Reach * Up;
            int   X     = Stalk.X + static_cast<int>(Sway + (Sway < 0 ? -0.5f : 0.5f));
            int   Y     = Floor - Rise;
            Color Ink   = color_mix(Stalk.Ink, COLOR_BLACK, static_cast<std::uint8_t>(90 - 90 * Up));
            View.PutLit(X, Y, Ink);
            if (Stalk.Kelp && Rise % 3 == 1 && Rise > 2)
                View.PutLit(X + ((Rise / 3) % 2 ? 1 : -1), Y, color_mix(Ink, White, 40));
        }
    }
}

void PaintCorals(Glass &View)
{
    for (const AquaCoral &Coral : View.Tank.Scene.Corals) {
        for (int Y = 0; Y < Coral.Height; Y++) {
            for (int X = 0; X < Coral.Width; X++) {
                std::uint8_t Pixel = Coral.Pixels[static_cast<std::size_t>(Y * Coral.Width + X)];
                if (Pixel) View.PutLit(Coral.X + X, Coral.Y + Y, Pixel == 2 ? Coral.Tip : Coral.Ink);
            }
        }
    }
}

void PaintAnemone(Glass &View)
{
    const AquaTank &Tank = View.Tank;
    int X0 = Tank.Scene.AnemoneX, Y0 = Tank.Scene.AnemoneY;
    for (int X = -3; X <= 3; X++) View.PutLit(X0 + X, Y0, AnemoneFoot);
    for (int Arm = -4; Arm <= 4; Arm++) {
        int Long = 4 + static_cast<int>(Hash(Arm, 7) >> 30);
        for (int Rise = 1; Rise <= Long; Rise++) {
            float Sway = AquaWave(Tank.Time * 0.3f + Arm * 0.11f + Rise * 0.07f) * Rise * 0.35f;
            int   X    = X0 + Arm + static_cast<int>(Sway + Arm * Rise * 0.15f);
            View.PutLit(X, Y0 - Rise, Rise == Long ? AnemoneTip : AnemoneInk);
        }
    }
}

/* The chest's lid lifts by how open it is, and the treasure shows. */
void PaintChest(Glass &View)
{
    const AquaChest &Chest = View.Tank.Scene.Chest;
    int X = Chest.X, Y = Chest.Y;
    for (int Row = 2; Row < 6; Row++) {
        for (int Col = 0; Col < 9; Col++) {
            Color Ink = WoodInk;
            if (Row == 5 || Col == 0 || Col == 8) Ink = GoldInk;
            if (Row == 3 && Col == 4) Ink = WoodDark;
            View.PutLit(X + Col, Y + Row, Ink);
        }
    }
    int Lift = static_cast<int>(Chest.Open * 3.0f + 0.5f);
    if (Lift > 0)
        for (int Col = 1; Col < 8; Col++) View.Put(X + Col, Y + 1, Col % 2 ? TreasureInk : GoldInk);
    for (int Col = 1; Col < 8; Col++) View.PutLit(X + Col, Y - Lift, WoodDark);
    for (int Col = 0; Col < 9; Col++) View.PutLit(X + Col, Y + 1 - Lift, Col == 4 ? GoldInk : WoodInk);

    const AquaScene &Scene = View.Tank.Scene;
    for (int Col = -1; Col <= 1; Col++) View.PutLit(Scene.BubblerX + Col, Scene.BubblerY - 1, StoneInk);
}

/* ---- what lives there ----------------------------------------------------------- */

Color InkOf(const AquaKind &Kind, char Letter)
{
    switch (Letter) {
    case 'b': return Kind.Look.Body;
    case 's': return Kind.Look.Second;
    case 'f': return Kind.Look.Fin;
    case 'e':
    case 'd': return Kind.Look.Dark;
    case 'w': return White;
    case 'g': return Kind.Look.Glow ? Kind.Look.Glow : Kind.Look.Second;
    default: return 0;
    }
}

void PaintCreature(Glass &View, const AquaCreature &Creature)
{
    const AquaKind   &Kind   = AquaKinds()[static_cast<std::size_t>(Creature.Kind)];
    const AquaSprite &Sprite = AquaFrameOf(Creature);
    bool Clear = Kind.Way == AquaWay::Drifts;      /* a jellyfish lets the water through */
    int  X0 = static_cast<int>(Creature.X + 0.5f), Y0 = static_cast<int>(Creature.Y + 0.5f);

    for (int Row = 0; Row < Sprite.Height; Row++) {
        const std::string &Line = Sprite.Rows[static_cast<std::size_t>(Row)];
        for (int Col = 0; Col < Sprite.Width; Col++) {
            int  At     = Creature.Left ? Sprite.Width - 1 - Col : Col;
            char Letter = At < static_cast<int>(Line.size()) ? Line[static_cast<std::size_t>(At)] : '.';
            if (Letter == '.') continue;
            int  X = X0 + Col, Y = Y0 + Row;
            bool Shining = Letter == 'g' && Kind.Glows && View.Daylight.Night;
            Color Ink    = Shining ? InkOf(Kind, Letter) : View.Lit(InkOf(Kind, Letter), Y);
            if (Clear && !Shining) View.Blend(X, Y, Ink, 180);
            else                   View.Put(X, Y, Ink);
        }
    }
}

/* At night what glows lights the water round it. */
void PaintGlow(Glass &View, const AquaCreature &Creature)
{
    const AquaKind &Kind = AquaKinds()[static_cast<std::size_t>(Creature.Kind)];
    if (!Kind.Glows || !View.Daylight.Night) return;
    const AquaSprite &Sprite = AquaFrameOf(Creature);
    int X0 = static_cast<int>(Creature.X + 0.5f), Y0 = static_cast<int>(Creature.Y + 0.5f);
    for (int Row = 0; Row < Sprite.Height; Row++) {
        for (int Col = 0; Col < Sprite.Width; Col++) {
            int At = Creature.Left ? Sprite.Width - 1 - Col : Col;
            if (Sprite.Rows[static_cast<std::size_t>(Row)][static_cast<std::size_t>(At)] != 'g') continue;
            for (int Dy = -2; Dy <= 2; Dy++)
                for (int Dx = -2; Dx <= 2; Dx++)
                    if (Dx || Dy) View.Blend(X0 + Col + Dx, Y0 + Row + Dy, Kind.Look.Glow, 70 / (Dx * Dx + Dy * Dy));
        }
    }
}

void PaintLife(Glass &View)
{
    const AquaTank &Tank = View.Tank;
    for (const AquaFlake &Flake : Tank.Flakes)
        View.PutLit(static_cast<int>(Flake.X), static_cast<int>(Flake.Y), Flake.Ink);

    /* What lies on the sand first, then what swims, the jellyfish over all. */
    for (int Pass = 0; Pass < 3; Pass++) {
        for (const AquaCreature &Creature : Tank.Creatures) {
            AquaWay Way   = AquaKinds()[static_cast<std::size_t>(Creature.Kind)].Way;
            int     Layer = Way == AquaWay::Rests || Way == AquaWay::Walks || Way == AquaWay::Creeps ? 0
                          : Way == AquaWay::Drifts                                                   ? 2
                                                                                                     : 1;
            if (Layer == Pass) PaintCreature(View, Creature);
        }
    }
    for (const AquaCreature &Creature : Tank.Creatures) PaintGlow(View, Creature);

    for (const AquaBubble &Bubble : Tank.Bubbles) {
        int   X   = static_cast<int>(Bubble.X), Y = static_cast<int>(Bubble.Y);
        Color Ink = color_mix(View.Lit(BubbleInk, Y), BubbleInk, static_cast<std::uint8_t>(View.Daylight.Sun * 120 / 256));
        View.Blend(X, Y, Ink, 210);
        if (Bubble.Big) {
            View.Blend(X + 1, Y, Ink, 160);
            View.Blend(X, Y - 1, Ink, 160);
        }
    }
}

}

AquaDaylight AquaDaylightAt(AquaLight Light, int Hour, int Minute)
{
    AquaDaylight Daylight;
    switch (Light) {
    case AquaLight::Day:   Daylight.Sun = 256; break;
    case AquaLight::Dusk:  Daylight.Sun = 110; break;
    case AquaLight::Night: Daylight.Sun = 0; break;
    case AquaLight::Clock: {
        /* Full from 07:00 to 19:00, rising from 05:30 and falling to 20:30. */
        int Now = Hour * 60 + Minute;
        if (Now >= 7 * 60 && Now < 19 * 60)            Daylight.Sun = 256;
        else if (Now >= 5 * 60 + 30 && Now < 7 * 60)   Daylight.Sun = (Now - (5 * 60 + 30)) * 256 / 90;
        else if (Now >= 19 * 60 && Now < 20 * 60 + 30) Daylight.Sun = (20 * 60 + 30 - Now) * 256 / 90;
        else                                           Daylight.Sun = 0;
        break;
    }
    }
    Daylight.Night = Daylight.Sun < 64;
    return Daylight;
}

void AquaPaint(const AquaTank &Tank, const AquaView &View, const AquaDaylight &Daylight, std::vector<Color> &Pixels)
{
    Glass Picture(Tank, View, Daylight, Pixels);
    PaintWater(Picture);
    PaintSand(Picture);
    for (const AquaRock &Rock : Tank.Scene.Rocks) PaintRock(Picture, Rock);
    PaintStalks(Picture);
    PaintCorals(Picture);
    PaintAnemone(Picture);
    PaintChest(Picture);
    PaintLife(Picture);
}

void AquaCells(const std::vector<Color> &Pixels, int Width, TextCell *Frame, int Rows, int Cols)
{
    for (int Row = 0; Row < Rows; Row++) {
        for (int Col = 0; Col < Cols; Col++) {
            TextCell &Cell = Frame[static_cast<std::size_t>(Row) * Cols + Col];
            Cell.ch = UpperHalfBlock;
            Cell.fg = Pixels[static_cast<std::size_t>(Row * 2) * Width + Col];
            Cell.bg = Pixels[static_cast<std::size_t>(Row * 2 + 1) * Width + Col];
        }
    }
}
