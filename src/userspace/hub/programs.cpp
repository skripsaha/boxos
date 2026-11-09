/* programs.cpp — the files that are programs, and the one a name means. */

#include "programs.h"

#include <string>
#include <unordered_set>
#include <vector>

#include "box/error.h"
#include "box/file.h"
#include "box/form.h"

#include "asking.h"
#include "hub.h"
#include "say.h"
#include "tags.h"
#include "volume.h"

namespace hub::programs {

namespace {

bool ProgramFile(const volume::Record &File)
{
    return tags::Wears(File.Tags(), "app") || tags::Wears(File.Tags(), "utility");
}

bool ItineraryFile(const volume::Record &File)
{
    return tags::Wears(File.Tags(), "itinerary");
}

bool CommandFile(const volume::Record &File)
{
    return ProgramFile(File) || ItineraryFile(File);
}

// One of Ids, asked for when there are several.
Picked ChooseOne(std::string_view Word, const std::vector<file_id_t> &Ids)
{
    if (Ids.size() == 1) return Ids[0];
    file_id_t      Chosen = 0;
    asking::Answer Asked  = asking::Which(Word, Ids, Chosen);
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
    std::unordered_set<file_id_t> Seen;
    for (const char *Tag : {"app", "utility", "itinerary"}) {
        file_id_t *Ids = nullptr;
        const int  Count = query_all_everywhere(Tag, &Ids);
        for (file_id_t File : volume::Taken(Ids, Count)) {
            if (!Seen.insert(File).second) continue;
            const std::expected<volume::Record, int> Held = volume::Record::Of(File);
            if (Held && CommandFile(*Held) && !Held->Name().empty()) Visit(File, Held->Name());
        }
    }
}

Picked Pick(std::string_view Name)
{
    file_id_t *Ids   = nullptr;
    const int  Total = query_all_everywhere(std::string(Name).c_str(), &Ids);
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
        if (CommandFile(*Held)) Commands.push_back(File);
    }
    if (!Commands.empty()) return ChooseOne(Name, Commands);
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

    Picked Program = ChooseOne(Thing, Found);
    if (Program && !IsProgram(*Program) && !IsItinerary(*Program)) {
        Say("run: {} is not a program: it wears neither app, utility nor itinerary\n", Thing);
        return std::unexpected(Said);
    }
    return Program;
}

}  // namespace hub::programs
