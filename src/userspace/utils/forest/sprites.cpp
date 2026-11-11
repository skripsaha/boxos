/* sprites.cpp — the pictures. Letters: b the coat, s its light side or belly,
 * w white, d dark, e eye, a a mark (a breast, an inner ear, an antler), k a
 * beak, y an owl's iris, l a leaf. */

#include "sprites.h"

#include <cmath>

namespace forest {

namespace {

// ---- the fox ------------------------------------------------------------------

constexpr Sprite FoxTrot{5, 4, {"............d.d.",
                                "...........bbbbb",
                                "wbb.bbbbbbbbbebb",
                                "bbbbbbbbbbbbwwwd",
                                ".bb.ssssssssww.."}};

constexpr Sprite FoxSniff{5, 2, {"wbb.bbbbbbbb....",
                                 "bbbbbbbbbbbbbd..",
                                 ".bb.ssssssssbbb.",
                                 "............bebb",
                                 "............wwwd"}};

constexpr Sprite FoxSit{9, 8, {".......d.d.",
                               "......bbbbb",
                               "......bbebb",
                               "......wwwwd",
                               ".....bbwww.",
                               "....bbbbww.",
                               "...bbbbbww.",
                               "..bbbbbbbd.",
                               "wbbbbbbbbdd"}};

constexpr Sprite FoxSleep{4, 3, {"...bbbbbb...",
                                 "..bbbbbbbbd.",
                                 ".bbbbbbbbbbd",
                                 "wwwwwwwbbbbd"}};

constexpr Sprite FoxDive{7, 6, {"wbb...........",
                                "..bbbb........",
                                "...bbbbbb.....",
                                "....sbbbbbb...",
                                "....d.sbbbbbd.",
                                ".....d..bbbbbw",
                                ".........wwwwd"}};

constexpr Legs FoxLegs{3, 11, 3};

// ---- the hedgehog -------------------------------------------------------------

constexpr Sprite HogWalk{5, 4, {"....bsbbs...",
                                "..bbsbbsbb..",
                                ".bsbbsbbsbs.",
                                "bbsbbsbsbbaa",
                                ".bbbbbbbbaad"}};

constexpr Sprite HogSniff{5, 4, {"....bsbbs...",
                                 "..bbsbbsbb..",
                                 ".bsbbsbbsbs.",
                                 "bbsbbsbsbbad",
                                 ".bbbbbbbbaa."}};

constexpr Sprite HogCurl{5, 4, {"..sbbsb..",
                                ".sbbsbbs.",
                                "bsbbsbsbb",
                                "bbsbbsbsb",
                                ".bbbbbbb."}};

constexpr Sprite HogSleep{4, 3, {"....lsbsl....",
                                 "..llbsbsbsll.",
                                 ".lllsbsbsllll",
                                 "lllllllllllll"}};

constexpr Legs HogLegs{2, 8, 1};

// ---- the hare -----------------------------------------------------------------

constexpr Sprite HareSit{10, 9, {"...dd....",
                                 "..bba....",
                                 "..bba....",
                                 "...bbbb..",
                                 "....bbebb",
                                 "....bbbbd",
                                 "..bbbbbb.",
                                 ".bbbbbbb.",
                                 "wbbbbbbb.",
                                 ".bbbbbdd."}};

constexpr Sprite HareAlert{12, 11, {"...dd....",
                                    "..bba....",
                                    "..bba....",
                                    "..bba....",
                                    "...bbbb..",
                                    "....bbebb",
                                    "....bbbbd",
                                    "...bbbbb.",
                                    "..bbbbbb.",
                                    ".bbbbbbb.",
                                    "wbbbbbbb.",
                                    ".bbbbbdd."}};

constexpr Sprite HareWashA{10, 9, {"...dd....",
                                   "..bba....",
                                   "..bba....",
                                   "...bbbb..",
                                   "....bbebb",
                                   "....bbbbw",
                                   "..bbbbbw.",
                                   ".bbbbbbb.",
                                   "wbbbbbbb.",
                                   ".bbbbbdd."}};

constexpr Sprite HareWashB{10, 9, {"...dd....",
                                   "..bba....",
                                   "..bba....",
                                   "...bbbb..",
                                   "....bbebb",
                                   "....bbbwd",
                                   "..bbbbww.",
                                   ".bbbbbbb.",
                                   "wbbbbbbb.",
                                   ".bbbbbdd."}};

constexpr Sprite HareStretch{6, 5, {"......dd.....",
                                    ".....bbbb....",
                                    "wbbbbbbbbbebb",
                                    "bbbbbbbbbbbbd",
                                    ".bbbbbbbbb...",
                                    "dd.........dd"}};

// ---- the deer -----------------------------------------------------------------

constexpr Sprite DeerWalk{6, 5, {"................a.a.",
                                 "................bbbd",
                                 "..............bbbebb",
                                 "..bbbbbbbbbbbbbbbwwd",
                                 "wbbbbbbbbbbbbbbb....",
                                 ".bbssssssssssss....."}};

constexpr Sprite DeerGraze{6, 2, {"..bbbbbbbbbbbb......",
                                  "wbbbbbbbbbbbbbbb....",
                                  ".bbssssssssssbbb....",
                                  "..............bbb...",
                                  "..............bbebb.",
                                  "...............wwwd."}};

constexpr Sprite DeerLie{6, 5, {"................a.a.",
                                "................bbbd",
                                "..............bbbebb",
                                "...bbbbbbbbbbbbbbwwd",
                                "wbbbbbbbbbbbbbbbb...",
                                ".bbsssssssssssbbd..."}};

constexpr Sprite FawnWalk{4, 3, {"........bbd.",
                                 "..bbbbbbbebb",
                                 "wbbwbbbbbbwd",
                                 ".bssssssbb.."}};

constexpr Legs DeerLegs{3, 14, 5};
constexpr Legs FawnLegs{3, 8, 3};

// ---- the birds ----------------------------------------------------------------

constexpr Sprite BirdPerchA{6, 5, {"....bbb.",
                                   "...bbebk",
                                   "..bbbbaa",
                                   "bbbbbbaa",
                                   ".bbbbss.",
                                   "....d.d."}};

constexpr Sprite BirdPerchB{6, 5, {"....bbb.",
                                   "...bbebk",
                                   "bbbbbbaa",
                                   ".bbbbbaa",
                                   ".bbbbss.",
                                   "....d.d."}};

constexpr Sprite BirdSing{6, 5, {"....bbb.",
                                 "...bbebk",
                                 "..bbbbak",
                                 "bbbbbbaa",
                                 ".bbbbss.",
                                 "....d.d."}};

constexpr Sprite BirdPeck{5, 4, {"..bbbb..",
                                 "bbbbbbb.",
                                 ".bbbbaab",
                                 "..bbssek",
                                 "...d.d.."}};

constexpr Sprite BirdUp{7, 6, {"..bb.....",
                               "...bb....",
                               "....b....",
                               "bbbbbbaek",
                               ".bbbbss..",
                               ".........",
                               "........."}};

constexpr Sprite BirdMid{7, 6, {".........",
                                ".........",
                                "..bbbb...",
                                "bbbbbbaek",
                                ".bbbbss..",
                                ".........",
                                "........."}};

constexpr Sprite BirdDown{7, 6, {".........",
                                 ".........",
                                 ".........",
                                 "bbbbbbaek",
                                 ".bbbbss..",
                                 "..bb.....",
                                 "...b....."}};

// ---- the owl ------------------------------------------------------------------

constexpr Sprite OwlPerch{10, 9, {"d.......d",
                                  "bbbbbbbbb",
                                  "byyybyyyb",
                                  "bydybydyb",
                                  "byyybyyyb",
                                  "bbbbkbbbb",
                                  "bsbsbsbsb",
                                  "bbsbsbsbb",
                                  ".bsbsbsb.",
                                  "..dd.dd.."}};

constexpr Sprite OwlBlink{10, 9, {"d.......d",
                                  "bbbbbbbbb",
                                  "bbbbbbbbb",
                                  "bdddbdddb",
                                  "bbbbbbbbb",
                                  "bbbbkbbbb",
                                  "bsbsbsbsb",
                                  "bbsbsbsbb",
                                  ".bsbsbsb.",
                                  "..dd.dd.."}};

constexpr Sprite OwlHoot{10, 9, {"d.......d",
                                 "bbbbbbbbb",
                                 "byyybyyyb",
                                 "bydybydyb",
                                 "byyybyyyb",
                                 "bbbkkkbbb",
                                 "bsbsbsbsb",
                                 "bbsbsbsbb",
                                 ".bsbsbsb.",
                                 "..dd.dd.."}};

constexpr Sprite OwlUp{8, 7, {"...bb.......",
                              "....bbb.....",
                              ".....bbbb...",
                              "bbbbbbbbyek.",
                              ".bbbbbsssbb.",
                              "..bbbsssb...",
                              "............",
                              "............"}};

constexpr Sprite OwlDown{8, 7, {"............",
                                "............",
                                "............",
                                "bbbbbbbbyek.",
                                ".bbbbbsssbb.",
                                "..bbbsssb...",
                                ".....bbbb...",
                                "....bbb....."}};

// ---- what drifts --------------------------------------------------------------

constexpr Sprite BatUp{3, 2, {"d..b..d",
                              ".dbbbd.",
                              "..d.d.."}};

constexpr Sprite BatDown{3, 2, {".......",
                                "dd.b.dd",
                                ".ddbdd."}};

constexpr Sprite ButterflyOpen{3, 2, {"ww.ww",
                                      "wwdww",
                                      ".w.w."}};

constexpr Sprite ButterflyHalf{3, 2, {"...",
                                      "wdw",
                                      "..."}};

constexpr Sprite ButterflyShut{3, 2, {"w.",
                                      "wd",
                                      "w."}};

int Frame(float Time, float Rate, int Count)
{
    return static_cast<int>(std::floor(Time * Rate)) % Count;
}

Pose Fox(const Animal &Beast)
{
    switch (Beast.Doing) {
    case Act::Sniff: return {&FoxSniff, &FoxLegs, false};
    case Act::Sit:   return {&FoxSit, nullptr, false};
    case Act::Sleep: return {&FoxSleep, nullptr, false};
    case Act::Pounce: return Beast.Rise > 0 ? Pose{&FoxTrot, &FoxLegs, true} : Pose{&FoxDive, nullptr, false};
    case Act::Walk:
    case Act::Trot:
    case Act::Run:   return {&FoxTrot, &FoxLegs, true};
    default:         return {&FoxTrot, &FoxLegs, false};
    }
}

Pose Hedgehog(const Animal &Beast)
{
    switch (Beast.Doing) {
    case Act::Walk:  return {&HogWalk, &HogLegs, true};
    case Act::Sniff: return {Frame(Beast.Time, 3.0f, 2) == 0 ? &HogWalk : &HogSniff, &HogLegs, false};
    case Act::Curl:  return {&HogCurl, nullptr, false};
    case Act::Sleep: return {&HogSleep, nullptr, false};
    default:         return {&HogWalk, &HogLegs, false};
    }
}

Pose Hare(const Animal &Beast)
{
    switch (Beast.Doing) {
    case Act::Hop:   return {Beast.Rise > 0 ? &HareStretch : &HareSit, nullptr, false};
    case Act::Wash:  return {Frame(Beast.Time, 3.0f, 2) == 0 ? &HareWashA : &HareWashB, nullptr, false};
    case Act::Alert: return {&HareAlert, nullptr, false};
    default:         return {&HareSit, nullptr, false};
    }
}

Pose Deer(const Animal &Beast)
{
    bool Fawn = Beast.Variant == 2;
    const Legs *Gait = Fawn ? &FawnLegs : &DeerLegs;
    switch (Beast.Doing) {
    case Act::Walk:
    case Act::Run:   return {Fawn ? &FawnWalk : &DeerWalk, Gait, true};
    case Act::Graze: return {Fawn ? &FawnWalk : &DeerGraze, Gait, false};
    case Act::Lie:
    case Act::Sit:   return {Fawn ? &FawnWalk : &DeerLie, nullptr, false};
    default:         return {Fawn ? &FawnWalk : &DeerWalk, Gait, false};
    }
}

Pose Bird(const Animal &Beast)
{
    switch (Beast.Doing) {
    case Act::Fly: {
        static const Sprite *const Strokes[] = {&BirdUp, &BirdMid, &BirdDown, &BirdMid};
        return {Strokes[Frame(Beast.Time, 14.0f, 4)], nullptr, false};
    }
    case Act::Sing: return {Frame(Beast.Time, 4.0f, 2) == 0 ? &BirdSing : &BirdPerchA, nullptr, false};
    case Act::Peck: return {Frame(Beast.Time, 3.0f, 2) == 0 ? &BirdPeck : &BirdPerchA, nullptr, false};
    default:        return {Frame(Beast.Time + Beast.Phase * 7.0f, 1.3f, 3) == 2 ? &BirdPerchB : &BirdPerchA, nullptr, false};
    }
}

Pose Owl(const Animal &Beast)
{
    switch (Beast.Doing) {
    case Act::Fly:  return {Frame(Beast.Time, 5.0f, 2) == 0 ? &OwlUp : &OwlDown, nullptr, false};
    case Act::Hoot: return {&OwlHoot, nullptr, false};
    default:        return {std::fmod(Beast.Time + Beast.Phase * 5.0f, 4.0f) < 0.18f ? &OwlBlink : &OwlPerch, nullptr, false};
    }
}

}  // namespace

Pose PoseOf(const Animal &Beast)
{
    switch (Beast.What) {
    case Kind::Fox:       return Fox(Beast);
    case Kind::Hedgehog:  return Hedgehog(Beast);
    case Kind::Hare:      return Hare(Beast);
    case Kind::Deer:      return Deer(Beast);
    case Kind::Bird:      return Bird(Beast);
    case Kind::Owl:       return Owl(Beast);
    case Kind::Bat:       return {Frame(Beast.Time + Beast.Phase, 12.0f, 2) == 0 ? &BatUp : &BatDown, nullptr, false};
    case Kind::Butterfly:
        if (Beast.Doing == Act::Sit) return {&ButterflyShut, nullptr, false};
        return {Frame(Beast.Time + Beast.Phase, 7.0f, 2) == 0 ? &ButterflyOpen : &ButterflyHalf, nullptr, false};
    case Kind::Firefly:   return {&ButterflyHalf, nullptr, false};
    }
    return {&FoxTrot, nullptr, false};
}

}  // namespace forest
