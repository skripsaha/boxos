/* programs.cpp — the files that are programs, and the one a name means. */

#include "programs.h"

#include <cstring>
#include <string>
#include <unordered_set>
#include <vector>

#include "box/error.h"
#include "box/file.h"
#include "box/form.h"
#include "box/memory.h"
#include "box/print.h"

#include "asking.h"
#include "hub.h"
#include "say.h"

namespace hub::programs {

namespace {

constexpr std::uint32_t FileActive = 1u << 0;   // the volume's flag of a file in use

// The comma-joined tags hold Key, bare or with a value.
bool Wears(const char *Tags, std::string_view Key)
{
    std::string_view Rest = Tags ? Tags : "";
    while (!Rest.empty()) {
        const std::size_t Comma = Rest.find(',');
        std::string_view  Tag   = Rest.substr(0, Comma);
        if (Tag.substr(0, Tag.find(':')) == Key) return true;
        if (Comma == std::string_view::npos) break;
        Rest.remove_prefix(Comma + 1);
    }
    return false;
}

bool ProgramFile(const FileMetadata &Metadata)
{
    return (Metadata.Flags & FileActive) && (Wears(Metadata.Tags, "app") || Wears(Metadata.Tags, "utility"));
}

bool ItineraryFile(const FileMetadata &Metadata)
{
    return (Metadata.Flags & FileActive) && Wears(Metadata.Tags, "itinerary");
}

bool CommandFile(const FileMetadata &Metadata)
{
    return ProgramFile(Metadata) || ItineraryFile(Metadata);
}

template <typename Question>
bool Ask(file_id_t File, Question &&Asked)
{
    FileMetadata Metadata;
    if (FileMetadataRead(File, &Metadata) != 0) return false;
    const bool Answer = Asked(Metadata);
    FileMetadataRelease(&Metadata);
    return Answer;
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

std::vector<file_id_t> Taken(file_id_t *Ids, int Count)
{
    std::vector<file_id_t> Found(Ids, Ids + (Count > 0 ? Count : 0));
    free(Ids);
    return Found;
}

}  // namespace

bool IsProgram(file_id_t File)
{
    return Ask(File, ProgramFile);
}

bool IsItinerary(file_id_t File)
{
    return Ask(File, ItineraryFile);
}

std::optional<std::string> NameOf(file_id_t File)
{
    FileMetadata Metadata;
    if (FileMetadataRead(File, &Metadata) != 0) return std::nullopt;
    std::optional<std::string> Name;
    if (Metadata.Filename) Name.emplace(Metadata.Filename);
    FileMetadataRelease(&Metadata);
    return Name;
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
        for (file_id_t File : Taken(Ids, query_all_everywhere(Tag, &Ids))) {
            if (!Seen.insert(File).second) continue;
            FileMetadata Metadata;
            if (FileMetadataRead(File, &Metadata) != 0) continue;
            if (CommandFile(Metadata) && Metadata.Filename) Visit(File, Metadata.Filename);
            FileMetadataRelease(&Metadata);
        }
    }
}

Picked Pick(std::string_view Name)
{
    file_id_t *Ids   = nullptr;
    int        Total = query_all_everywhere(std::string(Name).c_str(), &Ids);
    if (Total < 0) return std::unexpected(Total);

    // A name whose files the volume will not show — its medium gone — is
    // there and would not start, not a name nobody has.
    std::vector<file_id_t> Commands;
    int                    Unread = 0;
    for (file_id_t File : Taken(Ids, Total)) {
        FileMetadata Metadata;
        int          Rc = FileMetadataRead(File, &Metadata);
        if (Rc != 0) {
            if (Unread == 0) Unread = Rc;
            continue;
        }
        if (CommandFile(Metadata)) Commands.push_back(File);
        FileMetadataRelease(&Metadata);
    }
    if (!Commands.empty()) return ChooseOne(Name, Commands);
    return std::unexpected(Unread != 0 ? Unread : -ERR_FILE_NOT_FOUND);
}

namespace {

// The files called Thing: here, or else anywhere on the volume.
int FindThing(std::string_view Thing, std::vector<file_id_t> &Found)
{
    BoxFormLookup Lookup;
    form_lookup_volume(&Lookup);
    const std::string Name(Thing);
    for (bool Everywhere : {false, true}) {
        file_id_t *Ids   = nullptr;
        int        Count = Lookup.Find(&Lookup, Name.c_str(), Everywhere, &Ids);
        Found = Taken(Ids, Count);
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
            Say("run: the volume would not answer for {} (error {})\n", Thing, -Count);
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
