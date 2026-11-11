/* draw.cpp — draws with and without putting back. Without, each draw is a
 * step of a shuffle done only as far as it goes: a place of the pool picked at
 * random takes the place of the one drawn, so the pool is never copied, however
 * wide a range it is. */

#include "draw.h"

#include <map>

namespace random_pick {

namespace {

Drawn At(const Request &Asked, std::uint64_t Place)
{
    if (Asked.From == Kind::Range)
        return {std::to_string(Asked.Low + static_cast<std::int64_t>(Place)), 0};
    return {Asked.Names[Place], Asked.From == Kind::Files ? Asked.Files[Place] : 0};
}

// The places of the pool in the order they are drawn, none twice.
std::vector<std::uint64_t> PlacesDifferent(std::uint64_t Pool, std::uint64_t Count, Source &Random)
{
    std::map<std::uint64_t, std::uint64_t> Swapped;
    const auto Held = [&](std::uint64_t Place) {
        const auto Found = Swapped.find(Place);
        return Found == Swapped.end() ? Place : Found->second;
    };
    std::vector<std::uint64_t> Places;
    Places.reserve(Count);
    for (std::uint64_t Turn = 0; Turn < Count; Turn++) {
        const std::uint64_t Chosen = Turn + Random.Below(Pool - Turn);
        Places.push_back(Held(Chosen));
        Swapped[Chosen] = Held(Turn);
    }
    return Places;
}

}  // namespace

std::vector<Drawn> Draw(const Request &Asked, Source &Random)
{
    const std::uint64_t Pool = Asked.Pool();
    std::vector<Drawn>  Result;
    Result.reserve(Asked.Count);
    if (Asked.Different) {
        for (const std::uint64_t Place : PlacesDifferent(Pool, Asked.Count, Random)) Result.push_back(At(Asked, Place));
        return Result;
    }
    for (std::uint64_t Turn = 0; Turn < Asked.Count; Turn++) Result.push_back(At(Asked, Random.Below(Pool)));
    return Result;
}

std::string Candidate(const Request &Asked, Source &Random)
{
    return At(Asked, Random.Below(Asked.Pool())).Text;
}

}  // namespace random_pick
