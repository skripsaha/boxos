/* plan — what is to be done, checked against the facts (install.h). */

#include "install.h"

#include <algorithm>
#include <cstring>

#include "box/cxx/whose.h"
#include "box/error.h"
#include "boxos_bios_boot.h"

namespace install {

namespace {

/* The ESP a whole disk is given (the owner's word, 11-24): room for Gangway,
 * its crossings and another system's loaders, should one come later. */
constexpr std::uint64_t EspBytes    = 512ull * 1000 * 1000;
/* What a volume takes past its files — its book, its ribbon, its spine and
 * the room a system grows into — as a share of them and at the least. */
constexpr std::uint64_t GroundLeast = 256ull * 1024 * 1024;

std::uint64_t Blocks(const DiskListed &Disk, std::uint64_t Bytes)
{
    std::uint64_t Grain = DiskGrainBlocks(Disk.BlockBytes, Disk.PhysicalBytes);
    std::uint64_t N     = (Bytes + Disk.BlockBytes - 1u) / Disk.BlockBytes;
    return (N + Grain - 1u) / Grain * Grain;
}

/* Whether any of the disk's partitions holds Windows, or its ESP starts it. */
bool HoldsWindows(const DiskListed &Disk, bool &Locked)
{
    bool Windows = false;
    Locked       = false;
    for (std::size_t P = 0; P < Disk.PartCount; P++) {
        const DiskPartListed &Part = Disk.Parts[P];
        Locked  = Locked || Part.Holds == DISK_HOLDS_BITLOCKER;
        Windows = Windows || Part.Holds == DISK_HOLDS_NTFS || Part.Holds == DISK_HOLDS_BITLOCKER;
        for (std::size_t L = 0; L < Part.LoaderCount; L++)
            Windows = Windows || std::strcmp(disk_loader_shown(Part.Loaders[L]), "Windows") == 0;
    }
    return Windows;
}

/* One partition of a disk about to be laid anew, as the person is told what goes. */
std::string LostLine(const Facts &Now, const DiskListed &Disk, const DiskPartListed &Part)
{
    std::string Line = std::string(DiskKindName(static_cast<DiskPartKind>(Part.Kind))) + ", " +
                       Size(Part.Blocks * Disk.BlockBytes);
    if (Part.Name[0]) Line += std::string(" \"") + Part.Name + "\"";
    if (Part.Volume) {
        for (std::size_t V = 0; V < Now.VolumeCount; V++)
            if (Now.Volumes()[V].Number == Part.Volume - 1u) {
                char Shown[STORAGE_VOLUME_NAME_MOST + 64];
                volume_shown(&Now.Volumes()[V], Shown, sizeof(Shown));
                Line += std::string(" — the BoxOS volume ") + Shown + ", " +
                        std::to_string(Now.Volumes()[V].Files) + " file(s)";
            }
    } else if (Part.LoaderCount) {
        Line += " — the loaders of";
        for (std::size_t L = 0; L < Part.LoaderCount; L++)
            Line += std::string(L ? ", " : " ") + disk_loader_shown(Part.Loaders[L]);
    } else if (Part.Holds != DISK_HOLDS_UNKNOWN) {
        Line += std::string(" — ") + DiskHoldsName(static_cast<DiskHolds>(Part.Holds));
    }
    return Line;
}

std::vector<file_id_t> Taken(file_id_t *Ids, int Count)
{
    std::vector<file_id_t> Got;
    if (Ids && Count > 0) Got.assign(Ids, Ids + Count);
    free(Ids);
    std::sort(Got.begin(), Got.end());
    return Got;
}

}  // namespace

const DiskListed *DiskOf(const Facts &Now, const Plan &Wanted)
{
    for (std::size_t I = 0; I < Now.DiskCount; I++) {
        const DiskListed &Disk = Now.Disks()[I];
        if (Wanted.GuidKnown ? std::memcmp(Disk.Guid, Wanted.DiskGuid.data(), 16) == 0
                             : Wanted.DiskName == Disk.Name && Wanted.DiskBlocks == Disk.Blocks)
            return &Disk;
    }
    return nullptr;
}

void PlanDisk(Plan &Wanted, const DiskListed &Disk)
{
    Wanted.DiskName   = Disk.Name;
    Wanted.DiskBlocks = Disk.Blocks;
    Wanted.GuidKnown  = false;
    Wanted.Stick = Disk.Removable && Disk.BlockBytes == BIOS_SECTOR_BYTES;
}

const DiskFreeListed *LargestFree(const DiskListed &Disk)
{
    const DiskFreeListed *Best = nullptr;
    for (std::size_t F = 0; F < Disk.FreeCount; F++)
        if (!Best || Disk.Free[F].Blocks > Best->Blocks) Best = &Disk.Free[F];
    return Best;
}

std::string SignedName(const DiskListed &Disk)
{
    return Disk.Name[0] ? std::string(Disk.Name) : "disk " + std::to_string(Disk.Seat);
}

std::string MachineVolumeName(const Facts &Now)
{
    std::string Name = Now.MachineName.empty() ? "BoxOS" : Now.MachineName;
    if (Name.size() > STORAGE_VOLUME_NAME_MOST) {
        std::size_t Cut = Name.rfind(' ', STORAGE_VOLUME_NAME_MOST);
        Name.resize(Cut != std::string::npos && Cut > 0 ? Cut : STORAGE_VOLUME_NAME_MOST);
    }
    return Name;
}

std::optional<Carrying> Shipped(std::uint32_t Home, std::string &Why)
{
    file_id_t *Ids   = nullptr;
    int        Count = files_shipped(Home, &Ids);
    if (Count < 0) {
        Why = "what came with the system could not be asked: " + Refused(Count);
        return std::nullopt;
    }
    Carrying Going;
    Going.Ids = Taken(Ids, Count);
    for (file_id_t Id : Going.Ids) {
        FileMetadata Meta{};
        if (FileMetadataRead(box::tagfs::my_tags, Id, &Meta) == 0) {
            Going.Bytes += Meta.Size;
            FileMetadataRelease(&Meta);
        }
    }
    return Going;
}

std::optional<Carrying> Mine(std::uint32_t Home, const std::string &Without, std::string &Why)
{
    std::string Lost;
    std::optional<Carrying> System = Shipped(Home, Lost);
    if (!System) {
        Why = Lost;
        return std::nullopt;
    }
    file_id_t *Ids   = nullptr;
    int        Count = query_all_everywhere_on(Home, box::tagfs::person_tags, nullptr,
                                               Without.empty() ? nullptr : Without.c_str(), &Ids);
    if (Count < 0) {
        Why = "the person's files could not be asked for: " + Refused(Count);
        return std::nullopt;
    }
    Carrying Going;
    for (file_id_t Id : Taken(Ids, Count)) {
        if (std::binary_search(System->Ids.begin(), System->Ids.end(), Id)) continue;
        FileMetadata Meta{};
        if (FileMetadataRead(box::tagfs::my_tags, Id, &Meta) != 0) continue;
        bool Own = Meta.Role == FILE_ROLE_ORDINARY && !(Meta.Filename && std::strcmp(Meta.Filename, BookmarkName) == 0);
        if (Own) {
            Going.Ids.push_back(Id);
            Going.Bytes += Meta.Size;
        }
        FileMetadataRelease(&Meta);
    }
    return Going;
}

std::uint64_t RoleBytes(std::uint32_t Home)
{
    file_id_t    *Ids   = nullptr;
    int           Count = query_all_everywhere_on(Home, box::tagfs::person_tags, nullptr, nullptr, &Ids);
    std::uint64_t Bytes = 0;
    for (file_id_t Id : Taken(Ids, Count)) {
        FileMetadata Meta{};
        if (FileMetadataRead(box::tagfs::my_tags, Id, &Meta) != 0) continue;
        if (Meta.Role != FILE_ROLE_ORDINARY) Bytes += Meta.Size;
        FileMetadataRelease(&Meta);
    }
    return Bytes;
}

Reckoning Reckon(const Facts &Now, const Plan &Wanted)
{
    Reckoning           Cost;
    const DiskListed   *Disk = DiskOf(Now, Wanted);
    const VolumeListed *Home = Now.Home();
    if (!Disk) {
        Cost.Refusal = "the disk " + Wanted.DiskName + " is not there any more";
        return Cost;
    }
    if (!Home) {
        Cost.Refusal = "this machine stands on no volume of its own to take the system from";
        return Cost;
    }

    std::string Why;
    std::optional<Carrying> System = Shipped(Home->Number, Why);
    if (!System) {
        Cost.Refusal = Why;
        return Cost;
    }
    Cost.SystemBytes = System->Bytes + RoleBytes(Home->Number);
    if (Wanted.What == Take::Mine) {
        std::optional<Carrying> Own = Mine(Home->Number, Wanted.Without, Why);
        if (!Own) {
            Cost.Refusal = Why;
            return Cost;
        }
        Cost.MineBytes = Own->Bytes;
        Cost.MineFiles = static_cast<std::uint32_t>(Own->Ids.size());
    }
    std::uint64_t Files = Cost.SystemBytes + Cost.MineBytes;
    Cost.NeedBlocks     = Blocks(*Disk, std::max(Files + Files / 4u, Files + GroundLeast));

    std::uint64_t Esp  = Blocks(*Disk, EspBytes);
    std::uint64_t Bios = Wanted.Stick ? DiskGrainBlocks(Disk->BlockBytes, Disk->PhysicalBytes) : 0;
    if (Wanted.Where == Place::Whole) {
        if (Now.CarriesHome(*Disk)) {
            Cost.Refusal = "BoxOS runs from " + SignedName(*Disk) + " now, so it cannot be laid anew — start the "
                           "machine from a stick to install on it";
            return Cost;
        }
        for (std::size_t P = 0; P < Disk->PartCount; P++) Cost.Lost.push_back(LostLine(Now, *Disk, Disk->Parts[P]));
        std::uint64_t Usable = Disk->Blocks > Esp + Bios + 2048u ? Disk->Blocks - Esp - Bios - 2048u : 0;
        if (Usable < Cost.NeedBlocks) {
            Cost.Refusal = SignedName(*Disk) + " is " + Size(Disk->Blocks * Disk->BlockBytes) + ", and BoxOS takes " +
                           Size((Cost.NeedBlocks + Esp + Bios) * Disk->BlockBytes) + " with what goes on it";
            return Cost;
        }
    } else {
        if (Disk->Table != DISK_TABLE_GPT) {
            Cost.Refusal = SignedName(*Disk) + (Disk->Table == DISK_TABLE_MBR ? " keeps the old MBR table" : " has no table")
                           + ": BoxOS goes beside another system on a disk of a GPT alone";
            return Cost;
        }
        bool HasEsp = false;
        for (std::size_t P = 0; P < Disk->PartCount; P++) HasEsp = HasEsp || Disk->Parts[P].Kind == DISK_PART_ESP;
        const DiskFreeListed *Room = LargestFree(*Disk);
        std::uint64_t         Want = Cost.NeedBlocks + (HasEsp ? 0 : Esp);
        if (!Room || Room->Blocks < Want) {
            Cost.Refusal = "the largest free space on " + SignedName(*Disk) + " is " +
                           Size(Room ? Room->Blocks * Disk->BlockBytes : 0) + ", and BoxOS takes " +
                           Size(Want * Disk->BlockBytes) +
                           " — make room in the other system first (in Windows: Disk Management, Shrink Volume)";
            return Cost;
        }
        bool Locked = false;
        if (HoldsWindows(*Disk, Locked))
            Cost.Warnings.push_back("Windows is on this disk: turn off its Fast Startup and hibernation first, or the "
                                    "files it left open can be lost when it starts again");
        if (Locked)
            Cost.Warnings.push_back("a part of this disk is locked by BitLocker: keep its recovery key at hand — a "
                                    "change of what starts the machine can make Windows ask for it");
    }
    if (Now.EfiKnown && Now.Efi.sb_enforced)
        Cost.Warnings.push_back("Secure Boot is on: this machine's firmware will not start BoxOS until it is turned "
                                "off in the firmware's settings");
    if (!Wanted.Stick && !Now.UefiStart())
        Cost.Warnings.push_back("this start came through a BIOS, so the firmware is not told of BoxOS — the disk "
                                "starts by its ESP's default, where the firmware looks for one");
    if (Disk->Removable && !Wanted.Stick)
        Cost.Warnings.push_back("this stick's blocks are 4096 bytes, which no BIOS reads: it starts machines through "
                                "UEFI alone");
    return Cost;
}

}  // namespace install
