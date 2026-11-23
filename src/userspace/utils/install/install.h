/* install — BoxOS put on a disk: the whole of it, or beside what is there; a
 * fresh system, or the person's own files and settings brought along.
 *
 * The parts:
 *   facts.cpp     what the machine has now: its disks, the volumes of the
 *                 house, what the machine calls itself, its firmware
 *   plan.cpp      what is to be done, checked against the facts: the disk,
 *                 where on it, what goes, the name, and what it costs —
 *                 what is lost, how much room it takes, what to warn of
 *   steps.cpp     the plan done step by step, each from where the bookmark
 *                 says the last stopped
 *   bookmark.cpp  where an installation stopped, kept on the volume install
 *                 runs from, so a cut — the power, the stick — is gone on with
 *   asking.cpp    the person asked and told in the hub, a step at a time
 *   install.cpp   the words of the line, and the whole of it in order
 *
 * The kernel does every change by the person's hands (box/disk.h): a table, a
 * partition, a volume laid with the system, files carried, the disk made to
 * start. Install only says which, in what order, and what the person agreed.
 */
#ifndef INSTALL_INSTALL_H
#define INSTALL_INSTALL_H

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "box/disk.h"
#include "box/file.h"
#include "box/memory.h"
#include "box/system.h"

namespace install {

/* ---- facts.cpp ----------------------------------------------------------- */

struct FreeBlock {
    void operator()(void *Held) const noexcept { free(Held); }
};

/* What the machine has, read at one moment. */
struct Facts {
    std::unique_ptr<DiskListed, FreeBlock>   DiskBlock;
    std::size_t                              DiskCount = 0;
    std::unique_ptr<VolumeListed, FreeBlock> VolumeBlock;
    std::size_t                              VolumeCount = 0;
    machine_info_t                           Machine{};
    std::string                              MachineName;   /* machine_shown, "" for a nameless one */
    efi_info_t                               Efi{};
    bool                                     EfiKnown = false;

    const DiskListed   *Disks() const { return DiskBlock.get(); }
    const VolumeListed *Volumes() const { return VolumeBlock.get(); }
    const VolumeListed *Home() const;
    const VolumeListed *VolumeOfUuid(const std::array<std::uint8_t, 16> &Uuid) const;
    /* Whether a volume the running system needs — the home one — stands on `Disk`. */
    bool                CarriesHome(const DiskListed &Disk) const;
    /* The volumes of the house standing on `Disk`, by their numbers. */
    std::vector<std::uint32_t> StandingOn(const DiskListed &Disk) const;
    /* Whether this start can tell the firmware of a boot option (a UEFI start). */
    bool                UefiStart() const { return EfiKnown && Efi.rt_available; }
};

/* The machine read now, or why not, said for the person. */
std::optional<Facts> ReadFacts(std::string &Why);

/* ---- plan.cpp ------------------------------------------------------------ */

enum class Place : std::uint8_t { Whole, Beside };
enum class Take : std::uint8_t { Fresh, Mine };

/* What the person asked for. A disk is known by its GPT's GUID once it has
 * one — across seats and starts — and by what its medium calls itself and
 * how long it is before. */
struct Plan {
    std::array<std::uint8_t, 16> DiskGuid{};
    bool                         GuidKnown = false;
    std::string                  DiskName;
    std::uint64_t                DiskBlocks = 0;
    Place                        Where = Place::Whole;
    Take                         What = Take::Fresh;
    std::string                  Without;       /* the person's tags whose files stay behind: "kind:game,old" */
    std::string                  Name;          /* the new volume's */
    bool                         First = true;  /* first in the firmware's order */
    bool                         Stick = false; /* a removable disk: started through a BIOS too */
};

/* What the disk a plan names is now, among the facts; nullptr when no disk is. */
const DiskListed *DiskOf(const Facts &Now, const Plan &Wanted);

/* The plan's disk and its choices taken from `Disk`. */
void PlanDisk(Plan &Wanted, const DiskListed &Disk);

/* What a plan costs and risks, worked out before anything is changed. */
struct Reckoning {
    std::uint64_t            SystemBytes = 0;  /* the system's files, roles and shipped */
    std::uint64_t            MineBytes = 0;    /* the person's that go too */
    std::uint32_t            MineFiles = 0;
    std::uint64_t            NeedBlocks = 0;   /* the BoxOS partition at the least, in the disk's blocks */
    std::vector<std::string> Lost;             /* what goes, a line each: whole-disk installs alone */
    std::vector<std::string> Warnings;         /* what the person is to know before saying yes */
    std::string              Refusal;          /* why it cannot be done; "" when it can */
};

/* The plan reckoned against the facts: room, what is lost, what to warn of. */
Reckoning Reckon(const Facts &Now, const Plan &Wanted);

/* Files that go to the new volume, in file id order — a program before the
 * files it made, as a carry wants them — and their bytes. */
struct Carrying {
    std::vector<file_id_t> Ids;
    std::uint64_t          Bytes = 0;
};
/* What came with the system on volume `Home` (files_shipped). */
std::optional<Carrying> Shipped(std::uint32_t Home, std::string &Why);
/* The person's own on `Home`: every live file but the system's — those it
 * keeps in a role and those it came with — and install's bookmark, the files
 * wearing a tag of `Without` left behind. */
std::optional<Carrying> Mine(std::uint32_t Home, const std::string &Without, std::string &Why);
/* The bytes of the files the system keeps in a role on `Home`, which a laid
 * volume is given by the kernel. */
std::uint64_t RoleBytes(std::uint32_t Home);

/* The name install's bookmark is kept under. */
inline constexpr const char *BookmarkName = "install.bookmark";

/* The free run of `Disk` a plan beside what is there goes into: the largest,
 * nullptr for none. */
const DiskFreeListed *LargestFree(const DiskListed &Disk);

/* The disk as a person types its name to sign for it. */
std::string SignedName(const DiskListed &Disk);

/* The volume name a machine gives when the person gives none: its own, cut on a
 * word to what a volume's name holds; "BoxOS" for a machine that says nothing. */
std::string MachineVolumeName(const Facts &Now);

/* ---- steps.cpp ----------------------------------------------------------- */

enum class Step : std::uint8_t { Begin, Table, Bios, Esp, Boxos, Lay, Shipped, Mine, Boot, Done };

/* A step as the person is told it: "laying the disk's table". */
const char *StepSaid(Step S);

/* Where an installation is: its plan, the step to do next, what the steps
 * before it made. */
struct Bookmark {
    Plan                         Wanted;
    Step                         Next = Step::Begin;
    std::uint16_t                EspEntry = 0;
    std::uint16_t                BiosEntry = 0;
    std::uint16_t                BoxosEntry = 0;
    bool                         EspShared = false;   /* an ESP another system uses: never laid, never its default */
    std::array<std::uint8_t, 16> Volume{};
    bool                         VolumeKnown = false;
    std::uint32_t                Carried = 0;         /* files of the carrying step under way already carried */
};

/* What the steps tell as they go, and what a cut leaves: the asking side. */
struct Telling {
    virtual ~Telling()                                                            = default;
    virtual void Began(Step S)                                                    = 0;
    virtual void Carrying(Step S, std::uint32_t Done, std::uint32_t Of)           = 0;
    virtual void Said(std::string_view Line)                                      = 0;
};

/* The installation done from `Mark.Next` to its end, the bookmark kept after
 * every step and every batch of files. True when it is done; false with the
 * refusal said, the bookmark where it stopped. */
bool Run(Bookmark &Mark, Telling &Tell);

/* ---- bookmark.cpp -------------------------------------------------------- */

/* The bookmark kept on the volume install runs from; nullopt for none. */
std::optional<Bookmark> BookmarkRead();
/* Kept, or — Next being Done — let go. False with the refusal in Why. */
bool BookmarkWrite(const Bookmark &Mark, std::string &Why);

/* ---- common -------------------------------------------------------------- */

/* A refusal of the kernel's, as the person is told it. */
std::string Refused(int Rc);

/* Bytes as a person is told them: "512 MB". */
std::string Size(std::uint64_t Bytes);

}  // namespace install

#endif
