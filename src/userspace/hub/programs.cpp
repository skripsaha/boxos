/* programs.cpp — the files that are programs, and the one a name means.
 *
 * What a file is comes from what it holds, never from the tags its person
 * gave it: a program holds a box or an ELF image (box/program_part.h), an
 * Itinerary begins with its header line (itinerary.h). The volume keeps every
 * file's first bytes on its card, so each is told without reading a file, and
 * the volume answers which files begin so. */

#include "programs.h"

#include <algorithm>
#include <string>
#include <vector>

#include "box/cxx/whose.h"
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

// What an Itinerary begins with: its header line, ended either way.
static_assert(itinerary::Header.size() + 1 <= FILE_HEAD_MOST, "an Itinerary is told by the first bytes a card keeps");
const std::string ItineraryBeginnings[] = {std::string(itinerary::Header) + '\n',
                                           std::string(itinerary::Header) + '\r'};

bool ItineraryFile(const volume::Record &File)
{
    return std::any_of(std::begin(ItineraryBeginnings), std::end(ItineraryBeginnings),
                       [&](const std::string &Beginning) { return File.Head().starts_with(Beginning); });
}

// Whether File is a command — a program or an Itinerary: 1 or 0, told by its
// first bytes; or the negative -error_t that kept its card from being read.
int CommandOf(const volume::Record &File)
{
    ProgramKind Kind = PROGRAM_NONE;
    const int   Rc   = program_kind(File.Id(), &Kind);
    if (Rc < 0) return Rc;
    return Kind != PROGRAM_NONE || ItineraryFile(File) ? 1 : 0;
}

// Every command's file on the volume, each once, in id order: the programs
// and the Itineraries, as the volume answers which files begin so.
std::vector<file_id_t> CommandFiles()
{
    file_id_t             *Ids   = nullptr;
    std::vector<file_id_t> Files = volume::Taken(Ids, program_files(&Ids));
    for (const std::string &Beginning : ItineraryBeginnings) {
        Ids = nullptr;
        const int Count = files_starting_with(Beginning.data(), Beginning.size(), &Ids);
        for (file_id_t File : volume::Taken(Ids, Count)) Files.push_back(File);
    }
    std::sort(Files.begin(), Files.end());
    Files.erase(std::unique(Files.begin(), Files.end()), Files.end());
    return Files;
}

// One of Ids — the home volume's first (files_home_first) — asked for when
// that leaves several.
Picked ChooseOne(const Look &Shown, std::string_view Word, const std::vector<file_id_t> &Ids)
{
    // The home volume's first: a choice is only ever among one volume's.
    std::vector<file_id_t> Near = Ids;
    Near.resize(files_home_first(Near.data(), Near.size()));
    if (Near.size() == 1) return Near[0];
    file_id_t      Chosen = 0;
    asking::Answer Asked  = asking::Which(Shown, Word, Near, Chosen);
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
    for (file_id_t File : CommandFiles()) {
        const std::expected<volume::Record, int> Held = volume::Record::Of(File);
        if (Held && !Held->Name().empty()) Visit(File, Held->Name());
    }
}

Picked Pick(const Look &Shown, std::string_view Name)
{
    if (Name.empty()) return std::unexpected(-ERR_FILE_NOT_FOUND);
    file_id_t *Ids   = nullptr;
    const int  Total =
        query_name_all_everywhere(box::tagfs::my_tags, std::string(Name).c_str(), nullptr, nullptr, &Ids);
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
