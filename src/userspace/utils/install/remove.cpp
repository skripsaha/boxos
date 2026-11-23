/* remove — BoxOS taken off a disk by the same install that put it there
 * (install.h).
 *
 *   install remove Zenbook SSD
 *
 * What goes is shown first — every BoxOS volume on the disk and its files,
 * BoxOS's loader on the ESP and the firmware's entry for it — and what stays:
 * every other system's partitions, untouched. Signed for by the disk's name.
 * Then each volume is put away, the disk is made to start BoxOS no more
 * (disk_unboot), BoxOS's partitions are taken away, and the ESP and the BIOS
 * boot partition with them when they hold nothing of anyone else — an ESP
 * that starts Windows stays. Every step looks at the disk as it is, so
 * removing again after a cut goes on where the last one stopped.
 */

#include "install.h"

#include <cstring>

#include "box/form.h"
#include "box/print.h"

namespace install {

namespace {

void Tell(std::string_view Text)
{
    std::string Line(Text);
    println(Line.c_str());
}

bool SameText(std::string_view A, std::string_view B)
{
    if (A.size() != B.size()) return false;
    for (std::size_t I = 0; I < A.size(); I++) {
        char X = A[I] >= 'A' && A[I] <= 'Z' ? static_cast<char>(A[I] + 32) : A[I];
        char Y = B[I] >= 'A' && B[I] <= 'Z' ? static_cast<char>(B[I] + 32) : B[I];
        if (X != Y) return false;
    }
    return true;
}

bool StartsBoxos(const DiskPartListed &Part)
{
    for (std::size_t L = 0; L < Part.LoaderCount; L++)
        if (std::strcmp(disk_loader_shown(Part.Loaders[L]), "BoxOS") == 0) return true;
    return false;
}

/* A partition as the person is told it: its kind, size and what it holds. */
std::string PartSaid(const Facts &Now, const DiskListed &Disk, const DiskPartListed &Part)
{
    std::string Said = std::string(DiskKindName(static_cast<DiskPartKind>(Part.Kind))) + ", " +
                       Size(Part.Blocks * Disk.BlockBytes);
    if (Part.Volume) {
        for (std::size_t V = 0; V < Now.VolumeCount; V++)
            if (Now.Volumes()[V].Number == Part.Volume - 1u) {
                char Shown[STORAGE_VOLUME_NAME_MOST + 64];
                volume_shown(&Now.Volumes()[V], Shown, sizeof(Shown));
                Said += std::string(" — the BoxOS volume ") + Shown + ", " + std::to_string(Now.Volumes()[V].Files) +
                        " file(s)";
            }
    } else if (Part.LoaderCount) {
        Said += " — starts";
        for (std::size_t L = 0; L < Part.LoaderCount; L++)
            Said += std::string(L ? ", " : " ") + disk_loader_shown(Part.Loaders[L]);
    } else if (Part.Holds != DISK_HOLDS_UNKNOWN) {
        Said += std::string(" — ") + DiskHoldsName(static_cast<DiskHolds>(Part.Holds));
    }
    return Said;
}

/* The disk read afresh by its GUID, or nullptr with the person told. */
const DiskListed *Again(std::optional<Facts> &Now, const Plan &Wanted)
{
    std::string Why;
    Now = ReadFacts(Why);
    if (!Now) {
        Tell("  " + Why);
        return nullptr;
    }
    const DiskListed *Disk = DiskOf(*Now, Wanted);
    if (!Disk) Tell("  the disk " + Wanted.DiskName + " is not there any more");
    return Disk;
}

}  // namespace

int Remove(const char *Words)
{
    std::string          Why;
    std::optional<Facts> Now = ReadFacts(Why);
    if (!Now) {
        Tell("install: " + Why);
        return 1;
    }
    const char       *At     = Words;
    const DiskListed *Disk   = nullptr;
    std::size_t       Called = disk_named(&At, Now->Disks(), Now->DiskCount, &Disk);
    if (Called != 1 || *At) {
        Tell(Called > 1 ? "More than one disk is called so — say its number." : "BoxOS is taken off a disk named by "
             "its name or number: install remove <disk>");
        return 1;
    }
    if (Now->CarriesHome(*Disk)) {
        Tell("BoxOS runs from " + SignedName(*Disk) + " now, so it is not taken off it — start the machine from a "
             "stick to take it off.");
        return 1;
    }
    bool Boxos = false;
    for (std::size_t P = 0; P < Disk->PartCount; P++)
        Boxos = Boxos || Disk->Parts[P].Kind == DISK_PART_BOXOS || StartsBoxos(Disk->Parts[P]);
    if (!Boxos) {
        Tell("There is no BoxOS on " + SignedName(*Disk) + ".");
        return 0;
    }

    Tell("BoxOS goes from " + SignedName(*Disk) + ":");
    for (std::size_t P = 0; P < Disk->PartCount; P++)
        if (Disk->Parts[P].Kind == DISK_PART_BOXOS) Tell("  " + PartSaid(*Now, *Disk, Disk->Parts[P]));
    Tell("  BoxOS's loader on its ESP, and the firmware's entry for it");
    bool Stays = false;
    for (std::size_t P = 0; P < Disk->PartCount; P++) {
        const DiskPartListed &Part = Disk->Parts[P];
        if (Part.Kind == DISK_PART_BOXOS || Part.Kind == DISK_PART_BIOS_BOOT) continue;
        if (Part.Kind == DISK_PART_ESP && !(Part.LoaderCount > (StartsBoxos(Part) ? 1u : 0u))) continue;
        if (!Stays) Tell("What stays, as it is:");
        Stays = true;
        Tell("  " + PartSaid(*Now, *Disk, Part));
    }
    char *Signed = form_ask(("Type its name to sign for it — " + SignedName(*Disk) + ":").c_str());
    bool  Agreed = Signed && SameText(Signed, SignedName(*Disk));
    free(Signed);
    if (!Agreed) {
        Tell("That is not its name: nothing was changed.");
        return 0;
    }

    Plan Wanted;
    PlanDisk(Wanted, *Disk);
    Tell("Taking BoxOS off " + SignedName(*Disk) + "...");
    for (std::uint32_t Number : Now->StandingOn(*Disk)) {
        int Rc = volume_put_away(Number);
        if (Rc != 0) {
            Tell("  volume " + std::to_string(Number) + " could not be put away: " + Refused(Rc));
            return 1;
        }
    }
    if (!(Disk = Again(Now, Wanted))) return 1;
    for (std::size_t P = 0; P < Disk->PartCount; P++) {
        if (Disk->Parts[P].Kind != DISK_PART_ESP || !StartsBoxos(Disk->Parts[P])) continue;
        DiskUnbooted Unbooted{};
        int          Rc = disk_unboot(Disk, Disk->Parts[P].Entry, &Unbooted);
        if (Rc != 0) {
            Tell("  BoxOS's loader could not be taken off the ESP: " + Refused(Rc));
            return 1;
        }
        Tell("  its loader taken off the ESP" + std::string(Unbooted.Options ? ", the firmware's entry for it with it"
                                                                           : ""));
    }
    /* BoxOS's partitions first, then what held only BoxOS's loaders. */
    for (int Pass = 0; Pass < 2; Pass++) {
        if (!(Disk = Again(Now, Wanted))) return 1;
        std::vector<std::uint16_t> Going;
        for (std::size_t P = 0; P < Disk->PartCount; P++) {
            const DiskPartListed &Part = Disk->Parts[P];
            bool Ours = Pass == 0 ? Part.Kind == DISK_PART_BOXOS
                                  : (Part.Kind == DISK_PART_ESP && !Part.LoaderCount && !Part.DefaultLoader) ||
                                        Part.Kind == DISK_PART_BIOS_BOOT;
            if (Ours) Going.push_back(Part.Entry);
        }
        for (std::uint16_t Entry : Going) {
            int Rc = disk_remove(Disk, Entry);
            if (Rc != 0 && !(Pass == 1 && Rc == -ERR_INVALID_ARGUMENT)) {
                Tell("  entry " + std::to_string(Entry) + " could not be taken away: " + Refused(Rc));
                return 1;
            }
        }
    }
    if (!(Disk = Again(Now, Wanted))) return 1;
    const DiskFreeListed *Room = LargestFree(*Disk);
    Tell("BoxOS is taken off " + SignedName(*Disk) + (Room ? "; " + Size(Room->Blocks * Disk->BlockBytes) +
                                                           " of it is free now." : "."));
    return 0;
}

}  // namespace install
