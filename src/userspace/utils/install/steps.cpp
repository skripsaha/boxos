/* steps — the plan done step by step (install.h).
 *
 * Every step reads the machine afresh and looks first whether it is done
 * already: a cut can land between a change on the disk and the bookmark that
 * says it was made, so a step that finds its work there takes it as its own
 * instead of making it twice. The bookmark is kept after every step and
 * every batch of files carried.
 */

#include "install.h"

#include <algorithm>
#include <cstring>

#include "box/cxx/whose.h"
#include "box/error.h"
#include "boxos_bios_boot.h"

namespace install {

namespace {

constexpr std::uint64_t EspBytes  = 512ull * 1000 * 1000;
constexpr std::size_t   BatchMost = 32;    /* files carried in one call, the bookmark kept after each */

std::uint64_t Blocks(const DiskListed &Disk, std::uint64_t Bytes)
{
    std::uint64_t Grain = DiskGrainBlocks(Disk.BlockBytes, Disk.PhysicalBytes);
    std::uint64_t N     = (Bytes + Disk.BlockBytes - 1u) / Disk.BlockBytes;
    return (N + Grain - 1u) / Grain * Grain;
}

const DiskPartListed *PartOfKind(const DiskListed &Disk, std::uint8_t Kind)
{
    for (std::size_t P = 0; P < Disk.PartCount; P++)
        if (Disk.Parts[P].Kind == Kind) return &Disk.Parts[P];
    return nullptr;
}

const DiskPartListed *PartAt(const DiskListed &Disk, std::uint16_t Entry)
{
    for (std::size_t P = 0; P < Disk.PartCount; P++)
        if (Disk.Parts[P].Entry == Entry) return &Disk.Parts[P];
    return nullptr;
}

const DiskFreeListed *FirstFree(const DiskListed &Disk)
{
    return Disk.FreeCount ? &Disk.Free[0] : nullptr;
}

/* The steps, one a call: `Disk` as the machine has it now. */
class Steps {
public:
    Steps(Bookmark &Mark, Telling &Tell, const Facts &Now, const DiskListed &Disk)
        : Mark(Mark), Tell(Tell), Now(Now), Disk(Disk)
    {
    }

    /* The step Mark.Next done, Mark.Next moved past it: true; false with the
     * refusal told. */
    bool Do()
    {
        switch (Mark.Next) {
        case Step::Begin:   return Begin();
        case Step::Table:   return Table();
        case Step::Bios:    return Bios();
        case Step::Esp:     return Esp();
        case Step::Boxos:   return Boxos();
        case Step::Lay:     return Lay();
        case Step::Shipped: return Carry(false);
        case Step::Mine:    return Carry(true);
        case Step::Boot:    return Boot();
        case Step::Done:    return true;
        }
        return false;
    }

private:
    Bookmark         &Mark;
    Telling          &Tell;
    const Facts      &Now;
    const DiskListed &Disk;

    bool No(const std::string &Why)
    {
        Tell.Said(Why);
        return false;
    }

    bool Begin()
    {
        Mark.Next = Mark.Wanted.Where == Place::Whole ? Step::Table : Step::Esp;
        return true;
    }

    bool Table()
    {
        for (std::uint32_t Number : Now.StandingOn(Disk)) {
            int Rc = volume_put_away(Number);
            if (Rc != 0)
                return No("volume " + std::to_string(Number) + " on " + SignedName(Disk) +
                          " could not be put away: " + Refused(Rc));
        }
        int Rc = Disk.Table == DISK_TABLE_GPT && !Disk.PartCount ? 0 : disk_lay(&Disk);
        if (Rc != 0) return No("the table of " + SignedName(Disk) + " was not laid: " + Refused(Rc));
        Mark.Next = Mark.Wanted.Stick ? Step::Bios : Step::Esp;
        return true;
    }

    bool Bios()
    {
        if (const DiskPartListed *Have = PartOfKind(Disk, DISK_PART_BIOS_BOOT)) {
            Mark.BiosEntry = Have->Entry;
        } else {
            const DiskFreeListed *Room = FirstFree(Disk);
            if (!Room) return No(SignedName(Disk) + " has no free space for its BIOS boot partition");
            int Rc = disk_add(&Disk, DISK_PART_BIOS_BOOT, Room->First,
                              DiskGrainBlocks(Disk.BlockBytes, Disk.PhysicalBytes), &Mark.BiosEntry);
            if (Rc != 0) return No("the BIOS boot partition was not laid: " + Refused(Rc));
        }
        Mark.Next = Step::Esp;
        return true;
    }

    bool Esp()
    {
        const DiskPartListed *Have = PartOfKind(Disk, DISK_PART_ESP);
        if (Have) {
            Mark.EspEntry  = Have->Entry;
            Mark.EspShared = Mark.Wanted.Where == Place::Beside;
        } else {
            const DiskFreeListed *Room = Mark.Wanted.Where == Place::Whole ? FirstFree(Disk) : LargestFree(Disk);
            std::uint64_t         Want = Blocks(Disk, EspBytes);
            if (!Room || Room->Blocks < Want) return No(SignedName(Disk) + " has no free space for an ESP");
            int Rc = disk_add(&Disk, DISK_PART_ESP, Room->First, Want, &Mark.EspEntry);
            if (Rc != 0) return No("the EFI system partition was not laid: " + Refused(Rc));
        }
        Mark.Next = Step::Boxos;
        return true;
    }

    bool Boxos()
    {
        /* A BoxOS partition laid before a cut: one no volume stands on, whose
         * head is empty — the one this installation laid. */
        for (std::size_t P = 0; P < Disk.PartCount && !Mark.BoxosEntry; P++) {
            const DiskPartListed &Part = Disk.Parts[P];
            if (Part.Kind == DISK_PART_BOXOS && !Part.Volume &&
                (Part.Holds == DISK_HOLDS_EMPTY || Mark.Wanted.Where == Place::Whole))
                Mark.BoxosEntry = Part.Entry;
        }
        if (!Mark.BoxosEntry || !PartAt(Disk, Mark.BoxosEntry)) {
            const DiskFreeListed *Room = LargestFree(Disk);
            if (!Room) return No(SignedName(Disk) + " has no free space left for BoxOS");
            int Rc = disk_add(&Disk, DISK_PART_BOXOS, Room->First, Room->Blocks, &Mark.BoxosEntry);
            if (Rc != 0) return No("BoxOS's partition was not laid: " + Refused(Rc));
        }
        Mark.Next = Step::Lay;
        return true;
    }

    bool Lay()
    {
        const DiskPartListed *Part = PartAt(Disk, Mark.BoxosEntry);
        if (!Part || Part->Kind != DISK_PART_BOXOS) return No("BoxOS's partition is not in entry " +
                                                              std::to_string(Mark.BoxosEntry) + " any more");
        std::uint32_t Number = 0;
        if (Part->Volume) {
            Number = Part->Volume - 1u;     /* laid before a cut */
        } else {
            std::uint32_t Roles = 0;
            int Rc = volume_lay(&Disk, Mark.BoxosEntry, Mark.Wanted.Name.c_str(), &Number, &Roles);
            if (Rc != 0) return No("the volume was not laid: " + Refused(Rc));
        }
        VolumeListed *Laid = nullptr;
        if (volume_info(Number, &Laid) != 0) return No("the volume laid could not be asked for");
        std::memcpy(Mark.Volume.data(), Laid->Uuid, 16);
        free(Laid);
        Mark.VolumeKnown = true;
        Mark.Carried     = 0;
        Mark.Next        = Step::Shipped;
        return true;
    }

    /* Whether a file of `Id`'s name and size lies on volume `To` already: a
     * batch a cut ended part way carried some of. */
    static bool There(std::uint32_t To, file_id_t Id)
    {
        FileMetadata Meta{};
        if (FileMetadataRead(box::tagfs::my_tags, Id, &Meta) != 0) return false;
        file_id_t *Ids   = nullptr;
        int        Count = Meta.Filename ? query_word_all_on(To, box::tagfs::my_tags, Meta.Filename, &Ids) : 0;
        bool       Found = false;
        for (int I = 0; I < Count && !Found; I++) {
            FileMetadata Other{};
            if (FileMetadataRead(box::tagfs::my_tags, Ids[I], &Other) != 0) continue;
            Found = Other.Filename && std::strcmp(Other.Filename, Meta.Filename) == 0 && Other.Size == Meta.Size;
            FileMetadataRelease(&Other);
        }
        free(Ids);
        FileMetadataRelease(&Meta);
        return Found;
    }

    bool Carry(bool Own)
    {
        const VolumeListed *Home = Now.Home();
        const VolumeListed *To   = Mark.VolumeKnown ? Now.VolumeOfUuid(Mark.Volume) : nullptr;
        if (!Home || !To || !To->Mounted) return No("the new volume is not here to carry files to");
        Step                    This = Own ? Step::Mine : Step::Shipped;
        std::string             Why;
        std::optional<Carrying> Going = Own ? Mine(Home->Number, Mark.Wanted.Without, Why) : Shipped(Home->Number, Why);
        if (!Going) return No(Why);
        if (Own && Mark.Wanted.What != Take::Mine) Going->Ids.clear();
        std::uint32_t Of     = static_cast<std::uint32_t>(Going->Ids.size());
        bool          Resume = Mark.Carried > 0;
        while (Mark.Carried < Of) {
            std::uint32_t          From  = Mark.Carried;
            std::size_t            Count = std::min<std::size_t>(BatchMost, Of - From);
            std::vector<file_id_t> Batch(Going->Ids.begin() + From, Going->Ids.begin() + From + Count);
            if (Resume) {
                Batch.erase(std::remove_if(Batch.begin(), Batch.end(),
                                           [&](file_id_t Id) { return There(To->Number, Id); }),
                            Batch.end());
                Resume = false;
            }
            std::size_t   Carried = 0;
            std::uint32_t Dropped = 0;
            int Rc = Batch.empty() ? 0
                                   : files_copy_to(To->Number, Batch.data(), Batch.size(), nullptr, &Carried, &Dropped);
            if (Rc != 0 && Rc != -ERR_INVALID_ARGUMENT)
                return No(std::string(Own ? "the person's files" : "the system's files") + " stopped being carried: " +
                          Refused(Rc));
            Mark.Carried = From + static_cast<std::uint32_t>(Count);
            Tell.Carrying(This, Mark.Carried, Of);
            if (!BookmarkWrite(Mark, Why)) return No("the bookmark could not be kept: " + Why);
        }
        Mark.Carried = 0;
        Mark.Next    = Own ? Step::Boot : Step::Mine;
        return true;
    }

    bool Boot()
    {
        const VolumeListed *To = Mark.VolumeKnown ? Now.VolumeOfUuid(Mark.Volume) : nullptr;
        if (!To) return No("the new volume is not here to start");
        DiskBooted Booted{};
        int        Rc = disk_boot(&Disk, Mark.EspEntry, To->Number, Mark.Wanted.First, &Booted);
        if (Rc != 0) return No(SignedName(Disk) + " was not made to start: " + Refused(Rc));
        if (Booted.Given)
            Tell.Said(std::string("the firmware starts it ") + (Mark.Wanted.First ? "first" : "from its menu"));
        else
            Tell.Said("this start knows no firmware to tell: the disk starts by its ESP's default");
        if (Booted.Bios) Tell.Said("a BIOS starts it too");
        Mark.Next = Step::Done;
        return true;
    }
};

}  // namespace

const char *StepSaid(Step S)
{
    switch (S) {
    case Step::Begin:   return "beginning";
    case Step::Table:   return "laying the disk's table";
    case Step::Bios:    return "laying its BIOS boot partition";
    case Step::Esp:     return "laying its EFI system partition";
    case Step::Boxos:   return "laying BoxOS's partition";
    case Step::Lay:     return "laying the volume and the system on it";
    case Step::Shipped: return "carrying the system's files";
    case Step::Mine:    return "carrying the person's files";
    case Step::Boot:    return "making the disk start";
    case Step::Done:    return "done";
    }
    return "?";
}

bool Run(Bookmark &Mark, Telling &Tell)
{
    std::string Why;
    while (Mark.Next != Step::Done) {
        std::optional<Facts> Now = ReadFacts(Why);
        if (!Now) {
            Tell.Said(Why);
            return false;
        }
        const DiskListed *Disk = DiskOf(*Now, Mark.Wanted);
        if (!Disk) {
            Tell.Said("the disk " + Mark.Wanted.DiskName + " is not there any more");
            return false;
        }
        if (Mark.Next == Step::Esp && !Mark.Wanted.GuidKnown && Disk->Table == DISK_TABLE_GPT) {
            Mark.Wanted.GuidKnown = true;   /* a table laid: the disk is known by its GUID from here */
            std::memcpy(Mark.Wanted.DiskGuid.data(), Disk->Guid, 16);
        }
        Tell.Began(Mark.Next);
        Steps Doing(Mark, Tell, *Now, *Disk);
        if (!Doing.Do()) return false;
        if (!BookmarkWrite(Mark, Why)) {
            Tell.Said("the bookmark could not be kept: " + Why);
            return false;
        }
    }
    return true;
}

}  // namespace install
