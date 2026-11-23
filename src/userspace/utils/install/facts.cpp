/* facts — what the machine has now (install.h). */

#include "install.h"

#include <cstring>

#include "box/error.h"
#include "box/print.h"

namespace install {

const VolumeListed *Facts::Home() const
{
    for (std::size_t I = 0; I < VolumeCount; I++)
        if (Volumes()[I].Home) return &Volumes()[I];
    return nullptr;
}

const VolumeListed *Facts::VolumeOfUuid(const std::array<std::uint8_t, 16> &Uuid) const
{
    for (std::size_t I = 0; I < VolumeCount; I++)
        if (std::memcmp(Volumes()[I].Uuid, Uuid.data(), 16) == 0) return &Volumes()[I];
    return nullptr;
}

std::vector<std::uint32_t> Facts::StandingOn(const DiskListed &Disk) const
{
    std::vector<std::uint32_t> Numbers;
    for (std::size_t P = 0; P < Disk.PartCount; P++)
        if (Disk.Parts[P].Volume) Numbers.push_back(Disk.Parts[P].Volume - 1u);
    return Numbers;
}

bool Facts::CarriesHome(const DiskListed &Disk) const
{
    const VolumeListed *Own = Home();
    if (!Own) return false;
    for (std::uint32_t Number : StandingOn(Disk))
        if (Number == Own->Number) return true;
    return false;
}

std::optional<Facts> ReadFacts(std::string &Why)
{
    Facts        Now;
    DiskListed  *Disks   = nullptr;
    VolumeListed *Volumes = nullptr;
    int          Rc      = disk_list(&Disks, &Now.DiskCount);
    if (Rc != 0) {
        Why = "the disks could not be listed: " + Refused(Rc);
        return std::nullopt;
    }
    Now.DiskBlock.reset(Disks);
    Rc = volume_list(&Volumes, &Now.VolumeCount);
    if (Rc != 0) {
        Why = "the volumes could not be listed: " + Refused(Rc);
        return std::nullopt;
    }
    Now.VolumeBlock.reset(Volumes);
    if (machine_info(&Now.Machine) == 0) {
        char Shown[2 * MACHINE_TEXT_MOST + 2];
        machine_shown(&Now.Machine, Shown, sizeof(Shown));
        Now.MachineName = Shown;
    }
    Now.EfiKnown = efi_info(&Now.Efi) == 0;
    return Now;
}

std::string Refused(int Rc)
{
    switch (-Rc) {
    case ERR_ACCESS_DENIED:     return "only the person's hands change the disks";
    case ERR_OBJECT_NOT_FOUND:  return "the disk is not there any more, or another sits in its place";
    case ERR_WOULD_BLOCK:       return "another change of a disk is under way";
    case ERR_BUSY:              return "a volume of the house stands there";
    case ERR_INVALID_STATE:     return "the disk has no table to change";
    case ERR_CORRUPTED:         return "its table's copies are to be put right first";
    case ERR_OUT_OF_RANGE:      return "there is no such free run, or no such partition";
    case ERR_QUOTA_EXCEEDED:    return "its table has no entry left";
    case ERR_PERMISSION_DENIED: return "the disk refuses writes: it is protected";
    case ERR_DEVICE_NOT_READY:  return "a disk is not plugged in, or left on the way";
    case ERR_TAGFS_NO_SPACE:    return "there is not room enough";
    case ERR_TAGFS_READ_ONLY:   return "the volume takes no change";
    case ERR_FILE_NOT_FOUND:    return "a file is not there any more";
    case ERR_NO_MEMORY:         return "the machine has no memory left for it";
    default:                    return "the disk refused it (" + std::to_string(-Rc) + ")";
    }
}

std::string Size(std::uint64_t Bytes)
{
    char Shown[32];
    size_shown(Bytes, Shown, sizeof(Shown));
    return Shown;
}

}  // namespace install
