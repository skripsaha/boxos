/* view.cpp — the volume asked once for the line, and only for what the line
 * wants: the commands when a command's name is typed, the files of the Use
 * Context when a command takes files, the whole volume only after
 * `everywhere`. What is read is put on shelves, so every key after that finds
 * a name by halving however many the volume holds. */

#include "view.h"

#include <algorithm>
#include <cstring>

#include "box/file.h"
#include "box/memory.h"

#include "builtins.h"
#include "held.h"
#include "programs.h"
#include "tags.h"
#include "volume.h"

namespace hub {

void View::ReadCommands()
{
    if (CommandsRead) return;
    CommandsRead = true;
    for (const Builtin &One : Builtins()) CommandList.push_back(Command{std::string(One.Name), {}, 0});
    programs::EachCommand([&](file_id_t File, std::string_view FileName) {
        CommandList.push_back(
            Command{std::string(FileName.substr(0, programs::StemLength(FileName))), std::string(FileName), File});
    });
}

file_id_t View::Program(std::string_view Name)
{
    ReadCommands();
    for (const Command &One : CommandList)
        if (One.File != 0 && One.Name == Name) return One.File;
    return 0;
}

void View::ReadFiles(bool Everywhere)
{
    if (FilesRead[Everywhere]) return;
    FilesRead[Everywhere] = true;
    file_id_t *Ids   = nullptr;
    const int  Found = Everywhere ? query_all_everywhere(nullptr, "trashed,hidden", &Ids) : query_all(nullptr, "trashed,hidden", &Ids);
    for (file_id_t Id : volume::Taken(Ids, Found)) {
        const std::expected<volume::Record, int> Held = volume::Record::Of(Id);
        if (Held && !Held->Name().empty())
            FileList[Everywhere].push_back(File{std::string(Held->Name()), Id});
    }
}

int View::Collect(bool Everywhere, const std::string_view *Name, file_id_t **Ids)
{
    ReadFiles(Everywhere);
    *Ids = nullptr;
    std::size_t Found = 0;
    for (const File &One : FileList[Everywhere]) Found += !Name || One.Name == *Name;
    if (Found == 0) return 0;
    *Ids = static_cast<file_id_t *>(malloc(Found * sizeof(file_id_t)));
    if (!*Ids) return 0;
    std::size_t At = 0;
    for (const File &One : FileList[Everywhere])
        if (!Name || One.Name == *Name) (*Ids)[At++] = One.Id;
    return static_cast<int>(Found);
}

int View::Find(std::string_view Name, bool Everywhere, file_id_t **Ids)
{
    return Collect(Everywhere, &Name, Ids);
}

int View::All(bool Everywhere, file_id_t **Ids)
{
    return Collect(Everywhere, nullptr, Ids);
}

// The tags the files wear, from the volume's own list of them.
void View::ReadTags(bool Everywhere)
{
    if (TagsRead[Everywhere]) return;
    TagsRead[Everywhere] = true;
    TagListed  *Listed = nullptr;
    std::size_t Count  = 0;
    const int   Rc     = Everywhere ? tag_list_everywhere("trashed,hidden", &Listed, &Count) : tag_list("trashed,hidden", &Listed, &Count);
    const HeldHeap<TagListed> Held(Listed);
    if (Rc == 0)
        for (std::size_t I = 0; I < Count; I++) TagList[Everywhere].emplace_back(Listed[I].Tag);
}

const View::Names &View::ShelfOf(Shelf Kind)
{
    Names &Shelf = Shelved[Kind];
    if (Built[Kind]) return Shelf;
    Built[Kind] = true;

    const bool Everywhere = Kind == FilesEverywhere || Kind == TagsEverywhere;
    if (Kind == Commands || Kind == ProgramFiles) {
        ReadCommands();
        for (const Command &One : CommandList) {
            if (Kind == Commands) Shelf.push_back(One.Name);
            else if (One.File != 0) Shelf.push_back(One.FileName);
        }
    } else if (Kind == TagsHere || Kind == TagsEverywhere) {
        // In the Use Context its own tags are on every file, so they say
        // nothing and are left off.
        ReadTags(Everywhere);
        for (const std::string &Tag : TagList[Everywhere])
            if (Everywhere || !tags::Holds(Context, Tag)) Shelf.push_back(Tag);
    } else {
        ReadFiles(Everywhere);
        for (const File &One : FileList[Everywhere]) Shelf.push_back(One.Name);
    }
    std::sort(Shelf.begin(), Shelf.end());
    Shelf.erase(std::unique(Shelf.begin(), Shelf.end()), Shelf.end());
    return Shelf;
}

std::size_t View::From(const Names &Shelf, std::string_view Prefix) noexcept
{
    return static_cast<std::size_t>(std::lower_bound(Shelf.begin(), Shelf.end(), Prefix) - Shelf.begin());
}

bool View::Holds(const Names &Shelf, std::string_view Name) noexcept
{
    return std::binary_search(Shelf.begin(), Shelf.end(), Name);
}

}  // namespace hub
