/* owners.cpp — the owners of memory, their colors, order and ledger. */

#include "owners.h"

#include <algorithm>
#include <cctype>
#include <format>

#include "box/color.h"

namespace memmap {

namespace {

// How long what an owner gained is still shown as new.
constexpr std::uint64_t GlowUs = 6000000;

constexpr char32_t ShadeDark  = U'\u2593';   /* ▓ */
constexpr char32_t ShadeMid   = U'\u2592';   /* ▒ */
constexpr char32_t ShadeLight = U'\u2591';   /* ░ */
constexpr char32_t BlockFull  = U'\u2588';   /* █ */
constexpr char32_t BlockSmall = U'\u25A0';   /* ■ */
constexpr char32_t MarkDot    = U'\u00B7';   /* · */

std::uint32_t Hash(std::string_view Text) noexcept
{
    std::uint32_t Value = 2166136261u;
    for (const char Letter : Text) Value = (Value ^ static_cast<unsigned char>(Letter)) * 16777619u;
    return Value;
}

// A program's color is its name's alone, so it is the same one from one start
// to the next and on every machine; its mark on a screen without pixels is
// told apart by the same hash.
Color ProgramInk(std::string_view Name, std::uint32_t Tint)
{
    const std::uint32_t Mixed = Hash(Name);
    return Color::FromRaw(color_wheel(static_cast<std::uint16_t>(Mixed % 1536u),
                                      static_cast<std::uint8_t>(std::min<std::uint32_t>(Tint, 100) * 255u / 100u), 230));
}

char32_t ProgramMark(std::string_view Name) noexcept
{
    static constexpr char32_t Marks[] = {BlockFull, ShadeDark, ShadeMid};
    return Marks[(Hash(Name) / 1536u) % std::size(Marks)];
}

Color InkOf(Kind What, const Look &Shown) noexcept
{
    switch (What) {
    case Kind::Image:   return Shown.Image;
    case Kind::Kernel:  return Shown.Kernel;
    case Kind::Rings:   return Shown.Rings;
    case Kind::Shared:  return Shown.Shared;
    case Kind::Device:  return Shown.Device;
    case Kind::Program: return Shown.Programs;
    case Kind::Free:    return Shown.Free;
    case Kind::Untold:  break;
    }
    return Shown.Untold;
}

struct KindWords {
    const char *Key;
    const char *Name;
    const char *Note;
    char32_t    Mark;
};

KindWords WordsOf(Kind What) noexcept
{
    switch (What) {
    case Kind::Image:
        return {"image", "kernel image", "the kernel's own code and data, as it was loaded", BlockFull};
    case Kind::Kernel:
        return {"kernel", "kernel heap",
                "in use and mapped by no program: the kernel's heap, page tables, caches and buffers", ShadeDark};
    case Kind::Rings:
        return {"rings", "rings", "the rings every program talks to the system through, kept by the kernel", BlockSmall};
    case Kind::Shared:
        return {"shared", "shared", "windows shared between programs: Bay, Brook and what rides on them", ShadeMid};
    case Kind::Device:
        return {"device", "device", "devices, firmware and memory no program can use", ShadeLight};
    case Kind::Program:
        return {"programs", "programs", "what is mapped for programs: code, heap, stack and what they share", BlockFull};
    case Kind::Free:
        return {"free", "free", "memory nobody holds", MarkDot};
    case Kind::Untold:
        break;
    }
    return {"untold", "untold",
            "memory the registry names only by its zone: a program's, the kernel's, or free — it does not say which",
            ShadeLight};
}

Owner KindOwner(Kind What, const Look &Shown, std::uint64_t Bytes)
{
    const KindWords Words = WordsOf(What);
    Owner           One;
    One.What  = What;
    One.Key   = Words.Key;
    One.Name  = Words.Name;
    One.Note  = Words.Note;
    One.Bytes = Bytes;
    One.Ink   = InkOf(What, Shown);
    One.Mark  = Words.Mark;
    return One;
}

Owner ProgramOwner(const ProgramSeen &Seen, const Look &Shown, std::uint64_t Bytes)
{
    Owner One;
    One.What    = Kind::Program;
    One.Key     = std::format("p{}.{}", Seen.Pid, Seen.Generation);
    One.Name    = Seen.Name;
    One.Tags    = Seen.Tags;
    One.Note    = WordsOf(Kind::Program).Note;
    One.Bytes   = Bytes;
    One.Ink     = ProgramInk(Seen.Name, Shown.Tint);
    One.Mark    = ProgramMark(Seen.Name);
    One.Pid     = Seen.Pid;
    One.File    = Seen.File;
    One.BeganUs = Seen.BeganUs;
    return One;
}

std::uint64_t BytesOfKind(const Reading &Now, Kind What)
{
    std::uint64_t Sum = 0;
    for (const RegionSeen &One : Now.Regions)
        if (One.What == What) Sum += One.Bytes;
    return Sum;
}

// What takes precedence where regions lie over one another.
int Rank(Kind What) noexcept
{
    return What == Kind::Untold ? 0 : What == Kind::Device ? 1 : 2;
}

// Last in the list; the free and the untold, which are what is left once the
// rest is named.
bool Rest(Kind What) noexcept
{
    return What == Kind::Free || What == Kind::Untold;
}

bool Before(const Owner &A, const Owner &B, SortBy By)
{
    if (Rest(A.What) != Rest(B.What)) return !Rest(A.What);
    if (A.What != B.What && Rest(A.What)) return A.What == Kind::Untold;
    switch (By) {
    case SortBy::Age:
        if (A.IsProgram() != B.IsProgram()) return !A.IsProgram();
        if (A.IsProgram() && A.BeganUs != B.BeganUs) return A.BeganUs < B.BeganUs;
        if (!A.IsProgram() && A.What != B.What) return A.What < B.What;
        break;
    case SortBy::Name:
        if (A.Name != B.Name) return A.Name < B.Name;
        break;
    case SortBy::Size:
        if (A.Bytes != B.Bytes) return A.Bytes > B.Bytes;
        break;
    }
    return A.Key < B.Key;
}

bool FoldedHolds(std::string_view Text, std::string_view Part)
{
    const auto Fold = [](char C) { return static_cast<char>(std::tolower(static_cast<unsigned char>(C))); };
    return std::search(Text.begin(), Text.end(), Part.begin(), Part.end(),
                       [&](char A, char B) { return Fold(A) == Fold(B); }) != Text.end();
}

}  // namespace

bool Owner::Holds(std::string_view Part) const noexcept
{
    return Part.empty() || FoldedHolds(Name, Part) || FoldedHolds(Tags, Part);
}

const char *KindName(Kind What) noexcept
{
    return WordsOf(What).Name;
}

std::vector<Segment> Resolve(const std::vector<RegionSeen> &Regions, std::uint64_t Span)
{
    std::vector<std::uint64_t> Edges;
    Edges.reserve(Regions.size() * 2);
    for (const RegionSeen &One : Regions) {
        Edges.push_back(std::min(One.Base, Span));
        Edges.push_back(std::min(One.Base + One.Bytes, Span));
    }
    std::sort(Edges.begin(), Edges.end());
    Edges.erase(std::unique(Edges.begin(), Edges.end()), Edges.end());

    std::vector<Segment> Parts;
    for (std::size_t Index = 0; Index + 1 < Edges.size(); Index++) {
        const std::uint64_t From = Edges[Index], To = Edges[Index + 1];
        const RegionSeen   *Best = nullptr;
        for (const RegionSeen &One : Regions)
            if (One.Base <= From && One.Base + One.Bytes >= To && (!Best || Rank(One.What) >= Rank(Best->What)))
                Best = &One;
        if (!Best) continue;
        if (!Parts.empty() && Parts.back().Region == Best && Parts.back().Base + Parts.back().Bytes == From)
            Parts.back().Bytes += To - From;
        else Parts.push_back(Segment{From, To - From, Best});
    }
    return Parts;
}

std::vector<Owner> MemoryOwners(const Reading &Now, const Look &Shown)
{
    std::vector<Owner> Owners;
    std::uint64_t      Mapped = 0;
    for (const ProgramSeen &One : Now.Programs) {
        Owners.push_back(ProgramOwner(One, Shown, One.Bytes));
        Mapped += One.Bytes;
    }
    // What is in use and no program maps is the kernel's; of it the registry
    // names the image, the rings and the shared windows, and the rest is what
    // the kernel keeps for itself.
    std::uint64_t Kernel = Now.Used > Mapped ? Now.Used - Mapped : 0;
    const auto    Take   = [&](Kind What) {
        const std::uint64_t Named = std::min(BytesOfKind(Now, What), Kernel);
        Kernel -= Named;
        Owners.push_back(KindOwner(What, Shown, Named));
    };
    Take(Kind::Image);
    Take(Kind::Rings);
    Take(Kind::Shared);
    Owners.push_back(KindOwner(Kind::Kernel, Shown, Kernel));
    Owners.push_back(KindOwner(Kind::Free, Shown, Now.Total > Now.Used ? Now.Total - Now.Used : 0));
    return Owners;
}

std::ptrdiff_t OwnerOfRegion(const std::vector<Owner> &Owners, const RegionSeen &Region)
{
    const auto Is = [&](const Owner &One) {
        if (Region.What == Kind::Rings && Region.Cabin != 0 && One.IsProgram()) return One.Pid == Region.Cabin;
        return !One.IsProgram() && One.Key == WordsOf(Region.What).Key;
    };
    auto Found = std::find_if(Owners.begin(), Owners.end(), Is);
    if (Found == Owners.end() && Region.What == Kind::Rings)
        Found = std::find_if(Owners.begin(), Owners.end(),
                             [](const Owner &One) { return One.Key == WordsOf(Kind::Rings).Key; });
    return Found == Owners.end() ? -1 : Found - Owners.begin();
}

std::vector<Owner> AddressOwners(const Reading &Now, const Look &Shown, const std::vector<Segment> &Parts)
{
    std::vector<Owner> Owners;
    const auto         Hold = [&](const RegionSeen &Region, std::uint64_t Bytes) {
        std::ptrdiff_t At = OwnerOfRegion(Owners, Region);
        if (At < 0 && Region.What == Kind::Rings && Shown.Group == GroupBy::Program && Region.Cabin != 0) {
            const auto Program = std::find_if(Now.Programs.begin(), Now.Programs.end(),
                                              [&](const ProgramSeen &One) { return One.Pid == Region.Cabin; });
            if (Program != Now.Programs.end()) {
                Owners.push_back(ProgramOwner(*Program, Shown, 0));
                Owners.back().Note = "the rings this program talks to the system through, kept by the kernel";
                At                 = static_cast<std::ptrdiff_t>(Owners.size()) - 1;
            }
        }
        if (At < 0) {
            Owners.push_back(KindOwner(Region.What, Shown, 0));
            At = static_cast<std::ptrdiff_t>(Owners.size()) - 1;
        }
        Owners[static_cast<std::size_t>(At)].Bytes += Bytes;
    };
    for (const Segment &One : Parts) Hold(*One.Region, One.Bytes);
    return Owners;
}

void Group(std::vector<Owner> &Owners, const Look &Shown)
{
    if (Shown.Group != GroupBy::Kind) return;
    Owner Programs = KindOwner(Kind::Program, Shown, 0);
    Programs.BeganUs = UINT64_MAX;
    std::size_t Count = 0;
    std::erase_if(Owners, [&](const Owner &One) {
        if (!One.IsProgram()) return false;
        Programs.Bytes += One.Bytes;
        Programs.Since += One.Since;
        Programs.Glow += One.Glow;
        Programs.BeganUs = std::min(Programs.BeganUs, One.BeganUs);
        Programs.Tags += (Programs.Tags.empty() ? "" : ",") + One.Name;
        Count++;
        return true;
    });
    if (Count == 0) return;
    Programs.Name = std::format("programs ({})", Count);
    Owners.push_back(std::move(Programs));
}

void Order(std::vector<Owner> &Owners, SortBy By)
{
    std::stable_sort(Owners.begin(), Owners.end(), [By](const Owner &A, const Owner &B) { return Before(A, B, By); });
}

void Ledger::Take(std::vector<Owner> &Owners, std::uint64_t NowUs)
{
    const bool Primed = !Accounts.empty();
    std::unordered_map<std::string, Account> Kept;
    for (Owner &One : Owners) {
        const auto Known   = Accounts.find(One.Key);
        Account    Entry   = Known != Accounts.end() ? Known->second : Account{.Opened = Primed ? 0 : One.Bytes,
                                                                                .Before = Primed ? 0 : One.Bytes};
        if (One.Bytes > Entry.Before) {
            const bool Warm = NowUs - Entry.GrownAtUs < GlowUs;
            Entry.Grown     = (Warm ? Entry.Grown : 0) + (One.Bytes - Entry.Before);
            Entry.GrownAtUs = NowUs;
        }
        Entry.Before = One.Bytes;
        One.Since    = static_cast<std::int64_t>(One.Bytes) - static_cast<std::int64_t>(Entry.Opened);
        One.Glow     = NowUs - Entry.GrownAtUs < GlowUs ? Entry.Grown : 0;
        Kept.emplace(One.Key, Entry);
    }
    Accounts = std::move(Kept);
}

}  // namespace memmap
