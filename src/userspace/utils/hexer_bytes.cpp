/* hexer_bytes.cpp — the file under the glass: what is read, what was changed,
 * saving and finding. Nothing here draws or decides; it answers. */

#include <cstring>
#include <format>
#include <vector>

#include "box/file.h"

#include "hexer.h"

namespace {

/* Bytes a search reads at a time. */
constexpr std::size_t FindChunk = 65536;

}

std::size_t HexerBytes::Read(std::uint64_t At, std::uint8_t *Out, std::size_t Count,
                             std::uint8_t *Changed) const
{
    std::size_t Got = 0;
    while (Got < Count) {
        std::int64_t Taken = fread(File, At + Got, Out + Got, Count - Got);
        if (Taken <= 0) break;
        Got += static_cast<std::size_t>(Taken);
    }
    if (Changed) std::memset(Changed, 0, Count);
    for (auto Change = Changes.lower_bound(At); Change != Changes.end() && Change->first < At + Got; ++Change) {
        Out[Change->first - At] = Change->second;
        if (Changed) Changed[Change->first - At] = 1;
    }
    return Got;
}

/* Each run of changed bytes that stand side by side is one write. A write the
 * file refuses keeps every change from it on, so nothing typed is lost. */
std::string HexerBytes::Save()
{
    if (Changes.empty()) return "nothing is changed";

    std::vector<std::uint8_t> Run;
    std::uint64_t             Written = 0;
    auto                      Change  = Changes.begin();
    while (Change != Changes.end()) {
        std::uint64_t Start = Change->first;
        Run.clear();
        auto Last = Change;
        while (Last != Changes.end() && Last->first == Start + Run.size()) {
            Run.push_back(Last->second);
            ++Last;
        }

        std::size_t Done = 0;
        while (Done < Run.size()) {
            std::int64_t Wrote = fwrite(File, Start + Done, Run.data() + Done, Run.size() - Done);
            if (Wrote <= 0) {
                Changes.erase(Changes.begin(), Changes.lower_bound(Start + Done));
                Saved += Written + Done;
                return std::format("the file stopped taking bytes at 0x{:x}; {} change{} not saved",
                                   Start + Done, Changes.size(), Changes.size() == 1 ? " is" : "s are");
            }
            Done += static_cast<std::size_t>(Wrote);
        }
        Written += Run.size();
        Change = Last;
    }
    Changes.clear();
    Saved += Written;

    if (anchor(File) != 0)
        return std::format("{} byte{} written, but the volume did not confirm it", Written, Written == 1 ? "" : "s");
    return std::format("saved - {} byte{} changed", Written, Written == 1 ? "" : "s");
}

/* A place in First..Last where Needle begins. */
bool HexerBytes::FindIn(std::span<const std::uint8_t> Needle, std::uint64_t First, std::uint64_t Last,
                        std::uint64_t &Found) const
{
    std::vector<std::uint8_t> Chunk(FindChunk + Needle.size());
    for (std::uint64_t At = First; At <= Last;) {
        std::uint64_t Starts = Last - At + 1;
        std::size_t   Take   = Starts > FindChunk ? FindChunk : static_cast<std::size_t>(Starts);
        std::size_t   Got    = Read(At, Chunk.data(), Take + Needle.size() - 1, nullptr);

        for (std::size_t I = 0; I < Take && I + Needle.size() <= Got; I++) {
            if (std::memcmp(Chunk.data() + I, Needle.data(), Needle.size()) == 0) {
                Found = At + I;
                return true;
            }
        }
        if (Got < Take + Needle.size() - 1) return false;
        At += Take;
    }
    return false;
}

std::optional<std::uint64_t> HexerBytes::Find(std::span<const std::uint8_t> Needle, std::uint64_t From) const
{
    if (Needle.empty() || Needle.size() > Part.Hi - Part.Lo) return std::nullopt;

    std::uint64_t Last  = Part.Hi - Needle.size();
    std::uint64_t Found = 0;
    if (From < Last && FindIn(Needle, From + 1, Last, Found)) return Found;
    if (FindIn(Needle, Part.Lo, From < Last ? From : Last, Found)) return Found;
    return std::nullopt;
}
