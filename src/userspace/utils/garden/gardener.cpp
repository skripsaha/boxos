/* gardener — the garden's keeper, in the background. It hears every program
 * the machine starts (Touch, process:spawned), names it by its file, and
 * counts it in garden.plants: how often, when first, when last. The garden
 * (utility garden) shows what it keeps.
 *
 * It sleeps on the Touch and does nothing between starts; the file is written
 * once for a start, or for the several that came together, and only then is
 * the garden told. It has no screen and no keys. It lives on as the system's,
 * and when it falls the system raises it again; a second one aboard would
 * count every start twice, so one that finds another ends at once. */

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "box/cxx/whose.h"
#include "box/clock.h"
#include "box/core/cabin.h"
#include "box/debug.h"
#include "box/file.h"
#include "box/life.h"
#include "box/system.h"
#include "box/touch.h"

#include "record.h"

BOX_LIFE("serves\nrise");

namespace {

// Another gardener aboard: one more that wears the tag its file gives it.
bool Another()
{
    ProcMate      *Raw   = nullptr;
    const int      Count = proc_crew("gardener", &Raw, nullptr);
    if (Count <= 0) return false;
    const std::unique_ptr<ProcMate, void (*)(void *)> Held(Raw, free);
    for (int At = 0; At < Count; At++)
        if (Raw[At].pid != cabin_info()->pid) return true;
    return false;
}

// A program aboard: the file it was started from, 0 when it is gone already or
// was made from bytes, and whether its cabin wears system — which the system
// gives a program started from a file it keeps in its own role; a file's tags
// give none.
struct Aboard {
    file_id_t File   = 0;
    bool      System = false;
};

bool CabinWearsSystem(std::string_view Tags)
{
    while (!Tags.empty()) {
        const std::size_t Comma = Tags.find(',');
        if (Tags.substr(0, Comma) == "system") return true;
        if (Comma == std::string_view::npos) break;
        Tags.remove_prefix(Comma + 1);
    }
    return false;
}

Aboard AboardOf(std::uint32_t Pid, std::uint32_t Generation)
{
    RosterProgram *Raw   = nullptr;
    const int      Count = proc_roster(&Raw, nullptr);
    if (Count < 0) return {};
    const std::unique_ptr<RosterProgram, void (*)(void *)> Held(Raw, free);
    for (int At = 0; At < Count; At++)
        if (Raw[At].pid == Pid && Raw[At].generation == Generation)
            return {Raw[At].program_file, Raw[At].tags && CabinWearsSystem(Raw[At].tags)};
    return {};
}

struct Named {
    std::string Name;
    bool        System = false;
};

// The program started as (Pid, Generation), by its file's name without the
// extension; nothing when it is already gone, or was made from bytes.
std::optional<Named> NameOf(std::uint32_t Pid, std::uint32_t Generation)
{
    const Aboard    Program = AboardOf(Pid, Generation);
    FileMetadata    Metadata;
    if (Program.File == 0 || FileMetadataRead(box::tagfs::my_tags, Program.File, &Metadata) != 0) return std::nullopt;
    Named Found{Metadata.Filename ? Metadata.Filename : "", Program.System};
    FileMetadataRelease(&Metadata);
    if (const std::size_t Dot = Found.Name.rfind('.'); Dot != std::string::npos && Dot > 0) Found.Name.resize(Dot);
    if (Found.Name.empty()) return std::nullopt;
    return Found;
}

// What one start heard adds to the record; whether it added anything.
bool Count(const Touch &Heard, garden::Record &Kept)
{
    TouchProcessSpawned Born{};
    if (Heard.payload_len < sizeof(Born)) return false;
    std::memcpy(&Born, Heard.payload, sizeof(Born));
    const std::optional<Named> Who = NameOf(Born.pid, Born.generation);
    if (!Who) return false;
    Kept.Start(Who->Name, Who->System, clock_unix_now());
    return true;
}

// The record written, and the garden told; false when the volume would not
// take it, said once until it does.
bool Settle(const garden::Record &Kept, bool &Complained)
{
    if (const std::optional<std::string> Trouble = garden::WriteRecord(Kept)) {
        if (!Complained) kdbg_print("[gardener] the plants could not be kept: %s", Trouble->c_str());
        Complained = true;
        return false;
    }
    Complained = false;
    (void)touch_send(garden::GrewTag(), nullptr, 0, 0);
    return true;
}

}  // namespace

int main()
{
    if (Another()) return 0;

    const TouchTag Births = touch_pair_choose(touch_intern(TOUCH_TAG_PROCESS_SPAWNED));
    if (Births == TOUCH_TAG_INVALID || touch_claim(Births, TOUCH_REST, 0, 0) != 0) {
        kdbg_print("[gardener] starts of programs cannot be heard: nothing is counted");
        return 1;
    }
    garden::Record Kept;
    if (const std::optional<std::string> Trouble = garden::ReadRecord(Kept)) {
        kdbg_print("[gardener] the plants could not be read, so none is written over them: %s", Trouble->c_str());
        return 1;
    }

    bool Owed       = false;
    bool Complained = false;
    for (;;) {
        Touch Heard{};
        if (touch_await(Births, &Heard, 0) != 0) {
            kdbg_print("[gardener] waiting for starts failed: the gardener ends");
            return 1;
        }
        do {
            Owed |= Count(Heard, Kept);
        } while (touch_try_pop_tag(Births, &Heard));
        if (Owed) Owed = !Settle(Kept, Complained);
    }
}
