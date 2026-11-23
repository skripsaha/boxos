/* install — BoxOS put on a disk (install.h says the parts).
 *
 *   install                        asked step by step: which disk, the whole
 *                                  of it or beside what is there, your files
 *                                  along or a fresh system, a name, whether it
 *                                  starts first — then signed for and done
 *   install Zenbook SSD whole      what is said is not asked again:
 *   install disk 1 beside mine without kind:game called Home first
 *
 * The words, after the disk (its name or number):
 *   whole | beside        the whole disk, or the free space beside what is there
 *   fresh | mine          a fresh system, or the person's files and settings too
 *   without TAGS          the files wearing these tags (a comma between) stay behind
 *   first | last          where it stands in the firmware's order
 *   called NAME           the new volume's name: the rest of the line
 * A whole disk is always signed for by its name typed, however much was said.
 *
 * An installation cut part way — the power, the stick — keeps a bookmark on the
 * volume install runs from: started again, install says where it stopped and
 * goes on from there.
 */

#include "asking.h"

#include <cstring>

#include "box/form.h"
#include "box/print.h"

BOX_FORM("takes optional text \"a disk's name or number, then whole or beside, fresh or mine, without tags, first or last, called a name\"\n");

namespace {

using namespace install;

void Tell(std::string_view Text)
{
    std::string Line(Text);
    println(Line.c_str());
}

/* What the words of the line said; each `Said*` whether it was. */
struct Words {
    bool SaidWhere = false, SaidWhat = false, SaidFirst = false, SaidName = false;
};

/* The word at `At` and the spaces after it taken off, when it is `Word`. */
bool TakeWord(const char *&At, const char *Word)
{
    std::size_t N = std::strlen(Word);
    if (std::strncmp(At, Word, N) != 0 || (At[N] && At[N] != ' ')) return false;
    At += N;
    while (*At == ' ') At++;
    return true;
}

/* The line's words after the disk into the plan; false with the word that
 * was not understood said. */
bool Read(const char *At, Plan &Wanted, Words &Said)
{
    while (*At) {
        if (TakeWord(At, "whole")) Wanted.Where = Place::Whole, Said.SaidWhere = true;
        else if (TakeWord(At, "beside")) Wanted.Where = Place::Beside, Said.SaidWhere = true;
        else if (TakeWord(At, "fresh")) Wanted.What = Take::Fresh, Said.SaidWhat = true;
        else if (TakeWord(At, "mine")) Wanted.What = Take::Mine, Said.SaidWhat = true;
        else if (TakeWord(At, "first")) Wanted.First = true, Said.SaidFirst = true;
        else if (TakeWord(At, "last")) Wanted.First = false, Said.SaidFirst = true;
        else if (TakeWord(At, "without")) {
            const char *End = std::strchr(At, ' ');
            Wanted.Without  = End ? std::string(At, End) : std::string(At);
            At              = End ? End : At + std::strlen(At);
            while (*At == ' ') At++;
        } else if (TakeWord(At, "called")) {
            Wanted.Name   = At;
            Said.SaidName = !Wanted.Name.empty();
            break;
        } else {
            Tell(std::string("\"") + At + "\" is not understood: whole or beside, fresh or mine, without TAGS, first or "
                 "last, called NAME");
            return false;
        }
    }
    return true;
}

/* A bookmark left: told, and gone on with if the person says so; forgotten if
 * they say that. True when an installation was gone on with. */
bool Resumed(const Facts &Now)
{
    std::optional<Bookmark> Mark = BookmarkRead();
    if (!Mark) return false;
    Tell("An installation of BoxOS on " + Mark->Wanted.DiskName + " stopped while " + StepSaid(Mark->Next) + ".");
    std::string Why;
    if (!form_yes("Go on with it?")) {
        if (form_yes("Forget it? What it laid stays on the disk until the disk is laid anew")) {
            Mark->Next = Step::Done;
            if (!BookmarkWrite(*Mark, Why)) Tell("The bookmark could not be let go of: " + Why);
        }
        return true;
    }
    HubTelling Telling;
    if (Run(*Mark, Telling)) {
        const VolumeListed *Home = Now.Home();
        Handed(*Mark, Home && Home->Removable);
    } else {
        Tell("Started again, install goes on from here.");
    }
    return true;
}

int Installed(const char *Line)
{
    std::string          Why;
    std::optional<Facts> Now = ReadFacts(Why);
    if (!Now) {
        Tell("install: " + Why);
        return 1;
    }
    if (!Line[0] && Resumed(*Now)) return 0;

    Plan              Wanted;
    Words             Said;
    const DiskListed *Disk = nullptr;
    if (Line[0]) {
        const char *At     = Line;
        std::size_t Called = disk_named(&At, Now->Disks(), Now->DiskCount, &Disk);
        if (Called != 1) {
            Tell(Called ? "More than one disk is called so — say its number." : "No disk is called so:");
            if (!Called) ShowDisks(*Now);
            return 1;
        }
        PlanDisk(Wanted, *Disk);
        if (!Read(At, Wanted, Said)) return 1;
    } else {
        Disk = AskDisk(*Now);
        if (!Disk) {
            Tell("Nothing was changed.");
            return 0;
        }
        PlanDisk(Wanted, *Disk);
    }
    if (!Said.SaidWhere && !AskWhere(*Disk, Wanted)) return 1;
    if (!Said.SaidWhat && !AskWhat(*Now, Wanted)) return 1;
    if (!Said.SaidName && !AskName(*Now, Wanted)) return 1;
    if (!Said.SaidFirst && !AskFirst(*Now, Wanted)) return 1;
    if (Wanted.Stick) Wanted.First = false;

    Reckoning Cost = Reckon(*Now, Wanted);
    if (!Cost.Refusal.empty()) {
        Tell("BoxOS cannot go there: " + Cost.Refusal + ". Nothing was changed.");
        return 1;
    }
    if (!Agreed(*Now, Wanted, Cost)) return 0;

    Bookmark Mark;
    Mark.Wanted = Wanted;
    if (!BookmarkWrite(Mark, Why)) {
        Tell("The bookmark could not be kept, and without it a cut could not be gone on with: " + Why +
             ". Nothing was changed.");
        return 1;
    }
    Tell("Installing — a cut is gone on with when install is started again.");
    HubTelling Telling;
    if (!Run(Mark, Telling)) {
        Tell("The installation stopped. Started again, install goes on from where it stopped.");
        return 1;
    }
    const VolumeListed *Home = Now->Home();
    Handed(Mark, Home && Home->Removable);
    return 0;
}

}  // namespace

int main(void)
{
    const char *Line = form_text(0);
    if (!Line) {
        exit(1);
        return 1;
    }
    if (!form_person_here()) {
        Tell("install asks the person at the screen what to do, and nobody is there: nothing was changed");
        exit(1);
        return 1;
    }
    int Status = Installed(Line);
    exit(Status);
    return Status;
}
