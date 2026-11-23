/* asking — the person asked and told in the hub (asking.h). */

#include "asking.h"

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

/* A line the person typed, its Enter given; nullopt when nobody is there or
 * the reading ended. */
std::optional<std::string> Ask(const std::string &Question)
{
    char *Typed = form_ask(Question.c_str());
    if (!Typed) return std::nullopt;
    std::string Line(Typed);
    free(Typed);
    return Line;
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

/* What lies on a disk, in a few words: its partitions as their kinds and
 * what they hold, the volumes of the house on it, its free space. */
std::string Holding(const Facts &Now, const DiskListed &Disk)
{
    if (!Disk.PartCount) return Disk.Table == DISK_TABLE_NONE ? "nothing on it" : "no partition";
    std::string Said;
    for (std::size_t P = 0; P < Disk.PartCount; P++) {
        const DiskPartListed &Part = Disk.Parts[P];
        std::string           One  = DiskKindName(static_cast<DiskPartKind>(Part.Kind));
        if (Part.Volume) {
            for (std::size_t V = 0; V < Now.VolumeCount; V++)
                if (Now.Volumes()[V].Number == Part.Volume - 1u) {
                    char Shown[STORAGE_VOLUME_NAME_MOST + 64];
                    volume_shown(&Now.Volumes()[V], Shown, sizeof(Shown));
                    One = std::string("BoxOS \"") + Shown + "\"" + (Now.Volumes()[V].Home ? " (running now)" : "");
                }
        } else if (Part.LoaderCount) {
            One += " (starts";
            for (std::size_t L = 0; L < Part.LoaderCount; L++)
                One += std::string(L ? ", " : " ") + disk_loader_shown(Part.Loaders[L]);
            One += ")";
        } else if (Part.Holds != DISK_HOLDS_UNKNOWN) {
            One += std::string(" (") + DiskHoldsName(static_cast<DiskHolds>(Part.Holds)) + ")";
        }
        Said += (P ? ", " : "") + One;
    }
    if (const DiskFreeListed *Room = LargestFree(Disk)) Said += "; " + Size(Room->Blocks * Disk.BlockBytes) + " free";
    return Said;
}

}  // namespace

void HubTelling::Began(Step S)
{
    Quarter = 0;
    if (S != Step::Begin) Tell(std::string("  ") + StepSaid(S) + "...");
}

void HubTelling::Carrying(Step, std::uint32_t Done, std::uint32_t Of)
{
    std::uint32_t Now = Of ? Done * 4u / Of : 4u;
    if (Now <= Quarter && Done != Of) return;
    Quarter = Now;
    Tell("    " + std::to_string(Done) + " of " + std::to_string(Of) + " file(s)");
}

void HubTelling::Said(std::string_view Line)
{
    Tell(std::string("  ") + std::string(Line));
}

void ShowDisks(const Facts &Now)
{
    for (std::size_t I = 0; I < Now.DiskCount; I++) {
        const DiskListed &Disk = Now.Disks()[I];
        Tell("  disk " + std::to_string(Disk.Seat) + "  " + SignedName(Disk) + ", " +
             Size(Disk.Blocks * Disk.BlockBytes) + (Disk.Removable ? ", a stick" : "") + " — " + Holding(Now, Disk));
    }
}

const DiskListed *AskDisk(const Facts &Now)
{
    Tell("Which disk is BoxOS to go on?");
    ShowDisks(Now);
    for (;;) {
        std::optional<std::string> Typed = Ask("Its name or number:");
        if (!Typed || Typed->empty()) return nullptr;
        const char       *At   = Typed->c_str();
        const DiskListed *Disk = nullptr;
        std::size_t       Called = disk_named(&At, Now.Disks(), Now.DiskCount, &Disk);
        if (Called == 1 && !*At) {
            if (Disk->RefusesWrites) {
                Tell(SignedName(*Disk) + " refuses writes: it is protected. Another one?");
                continue;
            }
            return Disk;
        }
        Tell(Called > 1 ? "More than one disk is called so — say its number." : "No disk is called so. Again?");
    }
}

bool AskWhere(const DiskListed &Disk, Plan &Wanted)
{
    if (!Disk.PartCount) {
        Wanted.Where = Place::Whole;
        return true;
    }
    for (;;) {
        std::optional<std::string> Typed =
            Ask("The whole of " + SignedName(Disk) + " — everything on it goes — or beside what is there? (whole or beside)");
        if (!Typed) return false;
        if (SameText(*Typed, "whole")) Wanted.Where = Place::Whole;
        else if (SameText(*Typed, "beside")) Wanted.Where = Place::Beside;
        else continue;
        return true;
    }
}

bool AskWhat(const Facts &Now, Plan &Wanted)
{
    const VolumeListed *Home = Now.Home();
    std::string         Why;
    std::optional<Carrying> Own = Home ? Mine(Home->Number, "", Why) : std::nullopt;
    if (!Own || Own->Ids.empty()) {
        Wanted.What = Take::Fresh;
        return true;
    }
    Wanted.What = form_yes(("Bring your own files and settings along — " + std::to_string(Own->Ids.size()) +
                            " file(s), " + Size(Own->Bytes) + "?").c_str())
                      ? Take::Mine
                      : Take::Fresh;
    if (Wanted.What == Take::Mine) {
        std::optional<std::string> Typed =
            Ask("Leave the files of some tags behind? Name them, a comma between — or Enter for none:");
        if (!Typed) return false;
        Wanted.Without = *Typed;
    }
    return true;
}

bool AskName(const Facts &Now, Plan &Wanted)
{
    std::string Given = MachineVolumeName(Now);
    for (;;) {
        std::optional<std::string> Typed =
            Ask("What will you call it? Any name, in any language — or Enter for \"" + Given + "\":");
        if (!Typed) return false;
        if (Typed->size() > STORAGE_VOLUME_NAME_MOST) {
            Tell("A name is " + std::to_string(STORAGE_VOLUME_NAME_MOST) + " bytes at the most.");
            continue;
        }
        Wanted.Name = Typed->empty() ? Given : *Typed;
        return true;
    }
}

bool AskFirst(const Facts &Now, Plan &Wanted)
{
    Wanted.First = false;
    if (Wanted.Stick || !Now.UefiStart()) return true;
    Wanted.First = form_yes("Start BoxOS first when this machine starts?");
    return true;
}

bool Agreed(const Facts &Now, const Plan &Wanted, const Reckoning &Cost)
{
    const DiskListed *Disk = DiskOf(Now, Wanted);
    if (!Disk) return false;
    Tell("");
    Tell("BoxOS goes " + std::string(Wanted.Where == Place::Whole ? "on the whole of " : "beside what is on ") +
         SignedName(*Disk) + ", as the volume \"" + Wanted.Name + "\":");
    Tell("  the system, " + Size(Cost.SystemBytes) +
         (Wanted.What == Take::Mine ? ", and your " + std::to_string(Cost.MineFiles) + " file(s), " +
                                          Size(Cost.MineBytes) + (Wanted.Without.empty() ? "" : " — those of " +
                                          Wanted.Without + " left behind")
                                    : ", fresh: your files stay where they are"));
    if (Wanted.Stick) Tell("  a stick: it starts machines through UEFI and through a BIOS alike");
    else if (Now.UefiStart())
        Tell(std::string("  the firmware starts it ") + (Wanted.First ? "first" : "from its menu"));
    for (const std::string &Warning : Cost.Warnings) Tell("  ! " + Warning);
    if (Wanted.Where == Place::Beside) return form_yes("Go on?");

    Tell("Everything on " + SignedName(*Disk) + " goes:");
    if (Cost.Lost.empty()) Tell("  nothing lies on it");
    for (const std::string &Line : Cost.Lost) Tell("  " + Line);
    std::optional<std::string> Signed = Ask("Type its name to sign for it — " + SignedName(*Disk) + ":");
    if (!Signed || !SameText(*Signed, SignedName(*Disk))) {
        Tell("That is not its name: nothing was changed.");
        return false;
    }
    return true;
}

void Handed(const Bookmark &Mark, bool FromStick)
{
    Tell("");
    Tell("BoxOS is installed on " + Mark.Wanted.DiskName + ", as the volume \"" + Mark.Wanted.Name + "\".");
    if (Mark.Wanted.Stick) {
        Tell("The stick starts BoxOS on any machine, through UEFI or a BIOS.");
        return;
    }
    if (!form_person_here()) return;
    if (form_yes("Start it now?")) {
        Tell(FromStick ? "Starting it. When the screen goes dark, take the stick out." : "Starting it.");
        reboot();
        return;
    }
    Tell(FromStick ? "It starts the next time the machine does — take the stick out first."
                   : "It starts the next time the machine does.");
}

}  // namespace install
