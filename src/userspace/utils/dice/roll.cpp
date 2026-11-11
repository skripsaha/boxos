/* roll.cpp — the throw. */

#include "roll.h"

#include <algorithm>
#include <format>
#include <numeric>

namespace dice {

namespace {

// Marks which of a group's dice count: the Amount highest or lowest, or the rest.
void Choose(GroupRoll &Group)
{
    const Term &Of = Group.Of;
    if (Of.Rule == Keep::All) return;
    std::vector<std::size_t> Order(Group.Dice.size());
    std::iota(Order.begin(), Order.end(), std::size_t{0});
    const bool HighFirst = Of.Rule == Keep::Highest || Of.Rule == Keep::DropHighest;
    std::stable_sort(Order.begin(), Order.end(), [&](std::size_t A, std::size_t B) {
        return HighFirst ? Group.Dice[A].Value > Group.Dice[B].Value : Group.Dice[A].Value < Group.Dice[B].Value;
    });
    const bool Keeping = Of.Rule == Keep::Highest || Of.Rule == Keep::Lowest;
    for (std::size_t Place = 0; Place < Order.size(); Place++) {
        const bool Chosen = Place < static_cast<std::size_t>(Of.Amount);
        Group.Dice[Order[Place]].Counts = Keeping ? Chosen : !Chosen;
    }
}

GroupRoll ThrowGroup(const Term &Of, Source &From)
{
    GroupRoll Group;
    Group.Of = Of;
    for (int Each = 0; Each < Of.Count; Each++)
        Group.Dice.push_back(Die{static_cast<int>(From.Below(static_cast<std::uint64_t>(Of.Sides))) + 1, true});
    Choose(Group);
    for (const Die &One : Group.Dice)
        if (One.Counts) Group.Sum += One.Value;
    if (Of.Count == 0) Group.Sum = Of.Modifier;
    if (Of.Negative) Group.Sum = -Group.Sum;
    return Group;
}

std::string Signed(std::int64_t Number)
{
    return std::format("{}{}", Number < 0 ? '-' : '+', Number < 0 ? -Number : Number);
}

}  // namespace

Roll Throw(const Expression &Of, Source &From)
{
    Roll Thrown;
    for (const Term &One : Of.Terms) {
        Thrown.Groups.push_back(ThrowGroup(One, From));
        Thrown.Total += Thrown.Groups.back().Sum;
    }
    return Thrown;
}

Standing StandingOf(int Sides, int Value)
{
    if (Sides < 3) return Standing::Middle;
    return Value == Sides ? Standing::High : Value == 1 ? Standing::Low : Standing::Middle;
}

std::string PlainText(const Roll &Thrown)
{
    std::string Out;
    for (const GroupRoll &Group : Thrown.Groups) {
        if (Group.Of.Count == 0) {
            Out += std::format("{}\n", Signed(Group.Sum));
            continue;
        }
        Out += std::format("{}{}:", Group.Of.Negative ? "-" : "", Group.Of.Spelled);
        for (const Die &One : Group.Dice) Out += One.Counts ? std::format(" {}", One.Value) : std::format(" ({})", One.Value);
        Out += std::format(" = {}\n", Group.Sum);
    }
    Out += std::format("total {}\n", Thrown.Total);
    return Out;
}

}  // namespace dice
