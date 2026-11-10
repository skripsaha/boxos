/* circle.cpp — the programs behind the hub, and the round they come back in. */

#include "circle.h"

#include <algorithm>

#include "tags.h"

namespace hub {

void Circle::Add(BehindOne One)
{
    One.Away = ++Clock;
    Behind.push_back(std::move(One));
    Round.clear();
}

bool Circle::Remove(const Which &Program)
{
    std::erase(Round, Program);
    return std::erase_if(Behind, [&](const BehindOne &One) { return One.Program == Program; }) > 0;
}

BehindOne *Circle::Find(const Which &Program)
{
    const auto It =
        std::find_if(Behind.begin(), Behind.end(), [&](const BehindOne &One) { return One.Program == Program; });
    return It == Behind.end() ? nullptr : &*It;
}

bool Circle::Asks(const Which &Program)
{
    BehindOne *One = Find(Program);
    if (!One || One->Asks) return false;
    One->Asks = true;
    Round.clear();
    return true;
}

// Those on the circle in the order the person chose, asking ones first, then
// those of the Use Context they are in when they set it so.
std::vector<Which> Circle::Ordered(const Look &Shown, std::string_view Context) const
{
    std::vector<const BehindOne *> On;
    for (const BehindOne &One : Behind)
        if (!One.Serves || One.Asks) On.push_back(&One);
    const auto Here = [&](const BehindOne *One) {
        return Shown.BehindContextFirst && !Context.empty() && tags::Same(One->Context, Context);
    };
    std::stable_sort(On.begin(), On.end(), [&](const BehindOne *A, const BehindOne *B) {
        if (A->Asks != B->Asks) return A->Asks;
        if (Here(A) != Here(B)) return Here(A);
        switch (Shown.Order) {
        case BehindOrder::OldestFirst: return A->Away < B->Away;
        case BehindOrder::LastShown:
            if (A->Shown != B->Shown) return A->Shown > B->Shown;
            return A->Away > B->Away;
        case BehindOrder::NewestFirst: break;
        }
        return A->Away > B->Away;
    });
    std::vector<Which> Order;
    for (const BehindOne *One : On) Order.push_back(One->Program);
    return Order;
}

std::optional<BehindOne> Circle::Next(const std::optional<Which> &Now, const Look &Shown, std::string_view Context)
{
    // A round goes on from where the screen is; one the screen is not in is
    // over, and a new one begins with the order as it is now.
    auto At = Now ? std::find(Round.begin(), Round.end(), *Now) : Round.end();
    if (Round.empty() || (Now && At == Round.end())) {
        Round = Ordered(Shown, Context);
        At    = Now ? std::find(Round.begin(), Round.end(), *Now) : Round.end();
    }
    const std::size_t Following = At == Round.end() ? 0 : static_cast<std::size_t>(At - Round.begin()) + 1;
    if (Following >= Round.size()) {
        Round.clear();
        return std::nullopt;
    }
    BehindOne *One = Find(Round[Following]);
    if (!One) return std::nullopt;
    One->Shown      = ++Clock;
    One->Asks       = false;
    One->AskedToEnd = false;
    return *One;
}

std::vector<BehindOne> Circle::OnCircle() const
{
    std::vector<BehindOne> On;
    for (const BehindOne &One : Behind)
        if (!One.Serves || One.Asks) On.push_back(One);
    return On;
}

}  // namespace hub
