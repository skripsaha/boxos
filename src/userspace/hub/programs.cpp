/* programs.cpp — the files that are programs, and the one a name means.
 *
 * What a file is comes from what it holds, never from the tags its person
 * gave it: a program holds a box or an ELF image (box/program_part.h), an
 * Itinerary begins with its header line (itinerary.h). */

#include "programs.h"

#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "box/error.h"
#include "box/file.h"
#include "box/form.h"
#include "box/program_part.h"

#include "asking.h"
#include "hub.h"
#include "itinerary.h"
#include "say.h"
#include "tags.h"
#include "volume.h"

namespace hub::programs {

namespace {

bool ProgramFile(const volume::Record &File)
{
    ProgramKind Kind = PROGRAM_NONE;
    return program_kind(File.Id(), &Kind) == 0 && Kind != PROGRAM_NONE;
}

bool ItineraryFile(const volume::Record &File)
{
    constexpr std::string_view Header = itinerary::Header;
    const std::expected<std::string, int> Head = volume::Read(File.Id(), 0, Header.size() + 1);
    return Head && Head->size() == Header.size() + 1 && std::string_view(*Head).starts_with(Header) &&
           (Head->back() == '\n' || Head->back() == '\r');
}

// Whether File is a command — a program or an Itinerary: 1 or 0, told by its
// first bytes; or the negative -error_t that kept them from being read, its
// medium gone — a file that may well be a command, not one that is none.
int CommandOf(const volume::Record &File)
{
    ProgramKind Kind = PROGRAM_NONE;
    const int   Rc   = program_kind(File.Id(), &Kind);
    if (Rc < 0) return Rc;
    return Kind != PROGRAM_NONE || ItineraryFile(File) ? 1 : 0;
}

// What a file was found to be, as the hub last read it: its name and size
// then, and whether it was a command. The rule stays what the file holds
// (CommandOf); a file is read again only when it is new to the hub or its
// name or size has changed. A line typed at the prompt read the first bytes of
// every file on the volume, and an unknown command read them all again for
// its guess — two hundred and more reads of the medium before a key was
// echoed or the answer said. A file rewritten to the same size in place is
// listed as it was until it changes again; starting one is never stale — Pick
// reads the files the name means. The jobs' keeping strand asks too, hence
// the lock.
struct KnownFile {
    std::string   Name;
    std::uint64_t Size    = 0;
    bool          Command = false;
};

std::mutex                               KnownHeld;
std::unordered_map<file_id_t, KnownFile> Known;

// One of Ids, asked for when there are several.
Picked ChooseOne(const Look &Shown, std::string_view Word, const std::vector<file_id_t> &Ids)
{
    if (Ids.size() == 1) return Ids[0];
    file_id_t      Chosen = 0;
    asking::Answer Asked  = asking::Which(Shown, Word, Ids, Chosen);
    if (Asked == asking::Answer::Given) return Chosen;
    asking::SayCancelled(Word, Asked);
    return std::unexpected(Said);
}

}  // namespace

bool IsProgram(file_id_t File)
{
    const std::expected<volume::Record, int> Held = volume::Record::Of(File);
    return Held && ProgramFile(*Held);
}

bool IsItinerary(file_id_t File)
{
    const std::expected<volume::Record, int> Held = volume::Record::Of(File);
    return Held && ItineraryFile(*Held);
}

std::optional<std::string> NameOf(file_id_t File)
{
    const std::expected<volume::Record, int> Held = volume::Record::Of(File);
    if (!Held || Held->Name().empty()) return std::nullopt;
    return std::string(Held->Name());
}

std::size_t StemLength(std::string_view FileName)
{
    const std::size_t Dot = FileName.rfind('.');
    return Dot == std::string_view::npos ? FileName.size() : Dot;
}

void EachCommand(const std::function<void(file_id_t, std::string_view)> &Visit)
{
    file_id_t                                      *Ids   = nullptr;
    const int                                       Count = query_all_everywhere(nullptr, nullptr, &Ids);
    std::vector<std::pair<file_id_t, std::string>> Commands;
    {
        const std::lock_guard<std::mutex>        Guard(KnownHeld);
        std::unordered_map<file_id_t, KnownFile> Now;
        for (file_id_t File : volume::Taken(Ids, Count)) {
            const std::expected<volume::Record, int> Held = volume::Record::Of(File);
            if (!Held || Held->Name().empty()) continue;
            KnownFile  Is{std::string(Held->Name()), Held->Size(), false};
            const auto Was = Known.find(File);
            if (Was != Known.end() && Was->second.Name == Is.Name && Was->second.Size == Is.Size) {
                Is.Command = Was->second.Command;
            } else {
                // A file whose first bytes could not be read is not
                // remembered: it is read again the next time.
                const int Rc = CommandOf(*Held);
                if (Rc < 0) continue;
                Is.Command = Rc == 1;
            }
            if (Is.Command) Commands.emplace_back(File, Is.Name);
            Now.emplace(File, std::move(Is));
        }
        Known = std::move(Now);
    }
    for (const auto &[File, Name] : Commands) Visit(File, Name);
}

Picked Pick(const Look &Shown, std::string_view Name)
{
    if (Name.empty()) return std::unexpected(-ERR_FILE_NOT_FOUND);
    file_id_t *Ids   = nullptr;
    const int  Total = query_name_all_everywhere(std::string(Name).c_str(), nullptr, nullptr, &Ids);
    if (Total < 0) return std::unexpected(Total);

    // A name whose files the volume will not show — its medium gone — is
    // there and would not start, not a name nobody has.
    std::vector<file_id_t> Commands;
    int                    Unread = 0;
    for (file_id_t File : volume::Taken(Ids, Total)) {
        const std::expected<volume::Record, int> Held = volume::Record::Of(File);
        if (!Held) {
            if (Unread == 0) Unread = Held.error();
            continue;
        }
        const int Is = CommandOf(*Held);
        if (Is < 0 && Unread == 0) Unread = Is;
        if (Is == 1) Commands.push_back(File);
    }
    if (!Commands.empty()) return ChooseOne(Shown, Name, Commands);
    return std::unexpected(Unread != 0 ? Unread : -ERR_FILE_NOT_FOUND);
}

namespace {

// The files called Thing: here, or else anywhere on the volume. A negative
// -error_t when the volume would not answer.
int FindThing(std::string_view Thing, std::vector<file_id_t> &Found)
{
    BoxFormLookup Lookup;
    form_lookup_volume(&Lookup);
    const std::string Name(Thing);
    for (bool Everywhere : {false, true}) {
        file_id_t *Ids   = nullptr;
        const int  Count = Lookup.Find(&Lookup, Name.c_str(), Everywhere, &Ids);
        Found = volume::Taken(Ids, Count);
        if (Count != 0) return Count;
    }
    return 0;
}

}  // namespace

Picked PickThing(Hub &Self, std::string_view Thing)
{
    std::vector<file_id_t> Found;
    if (Thing == "it" || Thing == "them") {
        if (Self.Given.Files().empty()) {
            Say("run: {} is nothing yet: the last command gave no files\n", Thing);
            return std::unexpected(Said);
        }
        Found.assign(Self.Given.Files().begin(), Self.Given.Files().end());
    } else {
        int Count = FindThing(Thing, Found);
        if (Count < 0) {
            Say("run: the volume would not answer for {} ({})\n", Thing, Reason(Count));
            return std::unexpected(Said);
        }
        if (Count == 0) {
            Say("run: there is no file called {}\n", Thing);
            return std::unexpected(Said);
        }
    }

    Picked Program = ChooseOne(Self.Shown, Thing, Found);
    if (Program && !IsProgram(*Program) && !IsItinerary(*Program)) {
        Say("run: {} is neither a program nor an itinerary\n", Thing);
        return std::unexpected(Said);
    }
    return Program;
}

}  // namespace hub::programs
