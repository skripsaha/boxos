/* aquarium_kinds.cpp — what lives in the tank, and how each looks.
 *
 * A sprite faces right, a row of letters for each pixel row (aquarium.h says
 * what each letter is); one facing left is read right to left. Every colour is
 * chosen to keep its family on a sixteen-colour text console as well, which
 * projects a saturated #rrggbb by hue and a grey by lightness (typer_screen.h
 * spells the rule out): a fish stays orange there, its water dark blue. */

#include <cmath>

#include "aquarium.h"

/* ---- numbers ------------------------------------------------------------ */

std::uint64_t AquaDice::Next()
{
    State ^= State << 13;
    State ^= State >> 7;
    State ^= State << 17;
    return State;
}

float AquaDice::Unit()
{
    return static_cast<float>(Next() >> 40) / static_cast<float>(1ull << 24);
}

float AquaDice::Between(float Low, float High)
{
    return Low + (High - Low) * Unit();
}

int AquaDice::Count(int Low, int High)
{
    if (High <= Low) return Low;
    return Low + static_cast<int>(Next() % static_cast<std::uint64_t>(High - Low + 1));
}

bool AquaDice::Chance(float Probability)
{
    return Unit() < Probability;
}

namespace {

constexpr int WaveSteps = 1024;
std::array<float, WaveSteps> Waves;

}

void AquaMakeWaves()
{
    for (int Step = 0; Step < WaveSteps; Step++)
        Waves[Step] = static_cast<float>(std::sin(2.0 * 3.14159265358979323846 * Step / WaveSteps));
}

float AquaWave(float Turns)
{
    auto Step = static_cast<std::int64_t>(Turns * WaveSteps);
    return Waves[static_cast<std::size_t>(Step & (WaveSteps - 1))];
}

AquaColor AquaShade(AquaColor Ink, int Light)
{
    if (Light >= 256) return Ink;
    if (Light <= 0) return painter::core::Colors::Black;
    auto Channel = [Light](std::uint32_t Value) { return (Value * static_cast<std::uint32_t>(Light)) >> 8; };
    return AquaColor::Rgb(Channel(Ink.Red()), Channel(Ink.Green()), Channel(Ink.Blue()));
}

/* ---- the kinds ------------------------------------------------------------ */

namespace {

AquaSprite Picture(std::vector<std::string> Rows)
{
    AquaSprite Sprite;
    Sprite.Height = static_cast<int>(Rows.size());
    for (const std::string &Row : Rows)
        if (static_cast<int>(Row.size()) > Sprite.Width) Sprite.Width = static_cast<int>(Row.size());
    Sprite.Rows = std::move(Rows);
    return Sprite;
}

std::vector<AquaKind> MakeKinds()
{
    std::vector<AquaKind> Kinds;

    Kinds.push_back({"neon tetra", AquaWay::Schools,
                     {Picture({"..bbb.", "fbbbeb", "..sss."}),
                      Picture({"f.bbb.", ".bbbeb", "f.sss."})},
                     {},
                     {AquaColor::Rgb(0x30, 0xB0, 0xFF), AquaColor::Rgb(0xE8, 0x3A, 0x4A), AquaColor::Rgb(0x5A, 0x7C, 0xA0),
                      AquaColor::Rgb(0x10, 0x10, 0x10), AquaColor{}},
                     16.0f, 0.12f, 0.60f, 2, 3, false});

    Kinds.push_back({"clownfish", AquaWay::Guards,
                     {Picture({"....bb...", "f.bwbbweb", "ffbwbbwbb", "f..wbbw.."}),
                      Picture({"....bb...", "ffbwbbweb", ".fbwbbwbb", "...wbbw.."})},
                     {},
                     {AquaColor::Rgb(0xFF, 0x8A, 0x1C), AquaColor::Rgb(0xFF, 0xA0, 0x40), AquaColor::Rgb(0xE0, 0x6A, 0x10),
                      AquaColor::Rgb(0x18, 0x10, 0x10), AquaColor{}},
                     11.0f, 0.0f, 1.0f, 2, 2, false});

    Kinds.push_back({"goldfish", AquaWay::Swims,
                     {Picture({".....bb..", "fff.bbbeb", ".ffbbbbbb", "fff.ssss.", "......f.."}),
                      Picture({".....bb..", ".ff.bbbeb", "fffbbbbbb", ".ff.ssss.", "......f.."})},
                     {},
                     {AquaColor::Rgb(0xFF, 0x9A, 0x18), AquaColor::Rgb(0xFF, 0xD0, 0x70), AquaColor::Rgb(0xFF, 0x6A, 0x28),
                      AquaColor::Rgb(0x18, 0x10, 0x08), AquaColor{}},
                     9.0f, 0.20f, 0.75f, 2, 3, false});

    Kinds.push_back({"angelfish", AquaWay::Swims,
                     {Picture({"...f...", "..ff...", ".fbsbb.", "fbbsbeb", "fbbsbbb", ".fbsbb.", "..ff...", "...f..."}),
                      Picture({"...f...", "..ff...", "ffbsbb.", ".bbsbeb", ".bbsbbb", "ffbsbb.", "..ff...", "...f..."})},
                     {},
                     {AquaColor::Rgb(0xDC, 0xDC, 0xCC), AquaColor::Rgb(0x34, 0x34, 0x34), AquaColor::Rgb(0xD8, 0xC8, 0x60),
                      AquaColor::Rgb(0x10, 0x10, 0x10), AquaColor{}},
                     6.0f, 0.25f, 0.75f, 1, 2, false});

    Kinds.push_back({"blue tang", AquaWay::Swims,
                     {Picture({"...bbbb..", "ffbddbbeb", "ffbbbddbb", "...bbbb.."}),
                      Picture({"...bbbb..", "f.bddbbeb", ".fbbbddbb", "...bbbb.."})},
                     {},
                     {AquaColor::Rgb(0x20, 0x60, 0xE8), AquaColor::Rgb(0x40, 0x80, 0xFF), AquaColor::Rgb(0xFF, 0xD0, 0x20),
                      AquaColor::Rgb(0x0C, 0x14, 0x30), AquaColor{}},
                     14.0f, 0.15f, 0.65f, 2, 3, false});

    Kinds.push_back({"pufferfish", AquaWay::Swims,
                     {Picture({"..bbb..", ".bbbbeb", "fbbbbbb", ".sssss.", "..f.f.."}),
                      Picture({"..bbb..", ".bbbbeb", ".bbbbbb", "fsssss.", "..f.f.."})},
                     {Picture({"..d.d.d..", ".dbbbbbd.", "dbbbbbbeb", "fbbbbbbbd", "dbsssssbd", ".dsssssd.", "..d.d.d.."})},
                     {AquaColor::Rgb(0xC8, 0xA0, 0x40), AquaColor::Rgb(0xEC, 0xE0, 0xA8), AquaColor::Rgb(0xA0, 0x80, 0x30),
                      AquaColor::Rgb(0x60, 0x40, 0x20), AquaColor{}},
                     5.0f, 0.30f, 0.85f, 1, 1, false});

    Kinds.push_back({"jellyfish", AquaWay::Drifts,
                     {Picture({".bbb.", "bgggb", "bbbbb", "f.f.f", ".f.f.", "f.f.f", ".f.f."}),
                      Picture({"..b..", ".bgb.", ".bbb.", ".fff.", "f.f.f", ".f.f.", "f...f"})},
                     {},
                     {AquaColor::Rgb(0xC0, 0x60, 0xD0), AquaColor::Rgb(0xD8, 0x90, 0xE8), AquaColor::Rgb(0xA0, 0x70, 0xC8),
                      AquaColor::Rgb(0x30, 0x10, 0x40), AquaColor::Rgb(0xFF, 0x90, 0xFF)},
                     3.0f, 0.05f, 0.60f, 2, 4, true});

    Kinds.push_back({"crab", AquaWay::Walks,
                     {Picture({"s.e.e.s", "sbbbbbs", "f.f.f.f"}),
                      Picture({".se.es.", "sbbbbbs", ".f.f.f."})},
                     {},
                     {AquaColor::Rgb(0xD8, 0x30, 0x20), AquaColor::Rgb(0xFF, 0x5A, 0x3C), AquaColor::Rgb(0xA0, 0x20, 0x18),
                      AquaColor::Rgb(0x10, 0x08, 0x08), AquaColor{}},
                     6.0f, 1.0f, 1.0f, 1, 2, false});

    Kinds.push_back({"seahorse", AquaWay::Hovers,
                     {Picture({".bb..", ".bebb", ".bb..", "fbs..", ".bs..", "..bs.", "..b..", ".bb.."}),
                      Picture({".bb..", ".bebb", ".bb..", ".bs..", "fbs..", "..bs.", "..b..", ".bb.."})},
                     {},
                     {AquaColor::Rgb(0xF0, 0xA0, 0x20), AquaColor::Rgb(0xFF, 0xD0, 0x60), AquaColor::Rgb(0xF0, 0xC8, 0x40),
                      AquaColor::Rgb(0x20, 0x10, 0x08), AquaColor{}},
                     2.0f, 0.45f, 0.90f, 1, 2, false});

    Kinds.push_back({"snail", AquaWay::Creeps,
                     {Picture({".ss..e", "sdss.b", "bbbbbb"}),
                      Picture({".ss.e.", "sdssbb", "bbbbb."})},
                     {},
                     {AquaColor::Rgb(0xB8, 0xA8, 0x90), AquaColor::Rgb(0x9A, 0x5A, 0x22), AquaColor::Rgb(0xB8, 0xA8, 0x90),
                      AquaColor::Rgb(0x4A, 0x26, 0x0C), AquaColor{}},
                     1.0f, 1.0f, 1.0f, 1, 2, false});

    Kinds.push_back({"starfish", AquaWay::Rests,
                     {Picture({"..b..", "bbsbb", ".bsb.", ".b.b.", "b...b"})},
                     {},
                     {AquaColor::Rgb(0xE0, 0x50, 0x30), AquaColor::Rgb(0xFF, 0x8A, 0x60), AquaColor::Rgb(0xE0, 0x50, 0x30),
                      AquaColor::Rgb(0x60, 0x20, 0x10), AquaColor{}},
                     0.0f, 1.0f, 1.0f, 2, 3, false});

    Kinds.push_back({"lanternfish", AquaWay::Swims,
                     {Picture({".bbbe", "fbgbb"}),
                      Picture({"fbbbe", ".bgbb"})},
                     {},
                     {AquaColor::Rgb(0x2A, 0x3A, 0x5A), AquaColor::Rgb(0x2A, 0x3A, 0x5A), AquaColor::Rgb(0x1A, 0x2A, 0x40),
                      AquaColor::Rgb(0xC0, 0xE8, 0xFF), AquaColor::Rgb(0x40, 0xFF, 0xD0)},
                     10.0f, 0.65f, 0.95f, 3, 5, true});

    Kinds.push_back({"sea turtle", AquaWay::Visits,
                     {Picture({"....bbbb....", "..bsbbsbb.ff", ".bbbsbbsbbfe", "f.ssssssss..", "ff..ff..ff.."}),
                      Picture({"....bbbb....", "..bsbbsbb.ff", ".bbbsbbsbbfe", ".fssssssss..", "f..ff..ff..."})},
                     {},
                     {AquaColor::Rgb(0x3A, 0x6A, 0x30), AquaColor::Rgb(0x8A, 0xA0, 0x40), AquaColor::Rgb(0x5A, 0x8A, 0x40),
                      AquaColor::Rgb(0x10, 0x18, 0x08), AquaColor{}},
                     7.0f, 0.20f, 0.60f, 0, 0, false});

    return Kinds;
}

}

const std::vector<AquaKind> &AquaKinds()
{
    static const std::vector<AquaKind> Kinds = MakeKinds();
    return Kinds;
}

const AquaSprite &AquaFrameOf(const AquaCreature &Creature)
{
    const AquaKind &Kind = AquaKinds()[static_cast<std::size_t>(Creature.Kind)];
    if (Creature.Fright > 0 && !Kind.Puffed.empty()) return Kind.Puffed[0];
    auto Frame = static_cast<std::size_t>(Creature.Stroke * 2.0f) % Kind.Frames.size();
    return Kind.Frames[Frame];
}
