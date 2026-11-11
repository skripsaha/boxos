/* bets.cpp — the roulette bets (bets.h). */

#include "bets.h"

#include <algorithm>
#include <format>

#include "wheel.h"

namespace casino {

namespace {

const char *const Ordinals[] = {"1st", "2nd", "3rd"};

}  // namespace

bool Covers(const Spot &On, int Number)
{
    switch (On.What) {
    case Kind::Straight: return Number == On.A;
    case Kind::Split:    return Number == On.A || Number == On.B;
    case Kind::Column:   return Number != 0 && Number % 3 == (On.A + 1) % 3;
    case Kind::Dozen:    return Number != 0 && (Number - 1) / 12 == On.A;
    case Kind::Low:      return Number >= 1 && Number <= 18;
    case Kind::High:     return Number >= 19;
    case Kind::Even:     return Number != 0 && Number % 2 == 0;
    case Kind::Odd:      return Number % 2 == 1;
    case Kind::Red:      return HueOf(Number) == Hue::Red;
    case Kind::Black:    return HueOf(Number) == Hue::Black;
    }
    return false;
}

int Covered(const Spot &On)
{
    int Count = 0;
    for (int Number = 0; Number < Pockets; Number++)
        if (Covers(On, Number)) Count++;
    return Count;
}

int Return(const Spot &On)
{
    return 36 / Covered(On);
}

std::string Called(const Spot &On)
{
    switch (On.What) {
    case Kind::Straight: return std::format("number {}", On.A);
    case Kind::Split:    return std::format("split {} and {}", On.A, On.B);
    case Kind::Column:   return std::format("{} column", Ordinals[On.A]);
    case Kind::Dozen:    return std::format("{} dozen", Ordinals[On.A]);
    case Kind::Low:      return "low 1-18";
    case Kind::High:     return "high 19-36";
    case Kind::Even:     return "even";
    case Kind::Odd:      return "odd";
    case Kind::Red:      return "red";
    case Kind::Black:    return "black";
    }
    return {};
}

void Bets::Add(const Spot &On, std::int64_t Chips)
{
    const auto Found = std::find_if(Laid.begin(), Laid.end(), [&](const Placed &One) { return One.On == On; });
    if (Found != Laid.end()) Found->Chips += Chips;
    else Laid.push_back(Placed{On, Chips});
}

std::int64_t Bets::Take(const Spot &On, std::int64_t Chips)
{
    const auto Found = std::find_if(Laid.begin(), Laid.end(), [&](const Placed &One) { return One.On == On; });
    if (Found == Laid.end()) return 0;
    const std::int64_t Taken = std::min(Chips, Found->Chips);
    Found->Chips -= Taken;
    if (Found->Chips == 0) Laid.erase(Found);
    return Taken;
}

std::int64_t Bets::On(const Spot &Where) const
{
    for (const Placed &One : Laid)
        if (One.On == Where) return One.Chips;
    return 0;
}

std::int64_t Bets::Total() const
{
    std::int64_t Sum = 0;
    for (const Placed &One : Laid) Sum += One.Chips;
    return Sum;
}

std::int64_t Bets::Returned(int Number) const
{
    std::int64_t Sum = 0;
    for (const Placed &One : Laid)
        if (Covers(One.On, Number)) Sum += One.Chips * Return(One.On);
    return Sum;
}

}  // namespace casino
