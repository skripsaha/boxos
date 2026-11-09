/* view.h — what a line being typed can see: the commands, the files, and the
 * tags the files wear, read from the volume once for the line, the first time
 * each is wanted, and let go with the line. Each kind of name stands on a
 * shelf of its own — sorted, each name once — so a name is found by halving,
 * and the names that begin with a word stand side by side. */
#ifndef HUB_VIEW_H
#define HUB_VIEW_H

#include <array>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "box/types.h"

namespace hub {

class View {
public:
    enum Shelf : std::size_t {
        Commands,          // the hub's own and every program's, as typed
        ProgramFiles,      // every program's and Itinerary's file
        FilesHere,         // the files in the Use Context
        FilesEverywhere,   // the files on the whole volume
        TagsHere,          // the tags of the files in the Use Context, but its own
        TagsEverywhere,    // the tags of the whole volume
        Shelves
    };
    using Names = std::vector<std::string_view>;

    // A view from the Use Context Context: comma-joined, "" for none.
    explicit View(std::string Context) : Context(std::move(Context)) {}
    View(const View &) = delete;
    View &operator=(const View &) = delete;

    const Names &ShelfOf(Shelf Kind);

    // Where the names that begin with Prefix start on a shelf.
    static std::size_t From(const Names &Shelf, std::string_view Prefix) noexcept;
    static bool        Holds(const Names &Shelf, std::string_view Name) noexcept;

    // The program file a command's name means, the first when several do; 0
    // for a name no program on the volume has.
    file_id_t Program(std::string_view Name);

    // As BoxFormLookup's Find and All answer, from what the view holds: the
    // files called Name — every file, with no name — in the Use Context, or
    // on the whole volume. A heap array the caller frees.
    int Find(std::string_view Name, bool Everywhere, file_id_t **Ids);
    int All(bool Everywhere, file_id_t **Ids);

    bool Bounded() const noexcept { return !Context.empty(); }

private:
    struct Command {
        std::string Name;       // as it is typed
        std::string FileName;   // "" for the hub's own
        file_id_t   File;       // 0 for the hub's own
    };
    struct File {
        std::string Name;
        file_id_t   Id;
    };

    void ReadCommands();
    void ReadFiles(bool Everywhere);
    void ReadTags(bool Everywhere);
    int  Collect(bool Everywhere, const std::string_view *Name, file_id_t **Ids);

    std::string                      Context;
    std::vector<Command>             CommandList;
    bool                             CommandsRead = false;
    std::array<std::vector<File>, 2> FileList;      // in the Use Context, on the whole volume
    std::array<bool, 2>              FilesRead{};
    std::array<std::vector<std::string>, 2> TagList;
    std::array<bool, 2>              TagsRead{};
    std::array<Names, Shelves>       Shelved;
    std::array<bool, Shelves>        Built{};
};

}  // namespace hub

#endif
