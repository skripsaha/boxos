/* tagmap — files as a map of their tags: every tag a tile as big as what it
 * holds, and inside it its files the same way.
 *
 *   tagmap                  the files of the Use Context, or of the volume with none
 *   tagmap them             what the last command gave
 *   tagmap everywhere all   the whole volume, whatever the Use Context
 *   tagmap -> tags.txt      the tags as text into tags.txt
 *
 * On the screen:
 *   arrows        go to the tile that way
 *   enter         into the tile: a tag's files, or the small ones gathered
 *   backspace     back out
 *   space         give the tile's files to the shell (`them`) and leave
 *   w             weigh by size, or by how many files
 *   q, ESC        leave
 *
 * A file is in every tag it wears, so a tag is as big as all its files. Two
 * kinds of tag say nothing on a map and are left off it: one every file here
 * wears (the Use Context's, mostly — the header says which), and a name tag,
 * worn only by files whose name it is. A file with no other is "untagged". */

#include <algorithm>
#include <format>
#include <map>
#include <new>

#include "box/file.h"
#include "box/form.h"
#include "box/memory.h"
#include "box/print.h"

#include "keep.h"
#include "stage.h"
#include "tagmap.h"

BOX_FORM("takes optional things \"the files to map; with none, the Use Context\"\n"
         "gives things\n");

namespace {

const Color Page      = COLOR_RGB(0x12, 0x12, 0x12);
const Color BarPaper  = COLOR_RGB(0x3A, 0x3A, 0x3A);
const Color TitleInk  = COLOR_AMBER;
const Color ScopeInk  = COLOR_RGB(0x60, 0xA0, 0xE0);
const Color TextInk   = COLOR_RGB(0xEE, 0xEE, 0xEE);
const Color DimInk    = COLOR_RGB(0xC0, 0xC0, 0xC0);
const Color HintInk   = COLOR_RGB(0x8C, 0x8C, 0x8C);
const Color KeyInk    = COLOR_AMBER;
const Color DarkInk   = COLOR_RGB(0x10, 0x10, 0x10);
const Color MoreInk   = COLOR_RGB(0x40, 0x40, 0x40);
const Color MoreGrain = COLOR_RGB(0x5C, 0x5C, 0x5C);
const Color MoreLit   = COLOR_RGB(0xB4, 0xB4, 0xB4);

/* One of each of the console's six dark hues, so neighbours differ there too;
 * lit, each is its light twin. */
const Color Tints[] = {COLOR_RGB(0x24, 0x58, 0xB0), COLOR_RGB(0x2A, 0x80, 0x40), COLOR_RGB(0xA8, 0x2C, 0x2C),
                       COLOR_RGB(0x1C, 0x7C, 0x84), COLOR_RGB(0x8A, 0x2E, 0x9A), COLOR_RGB(0xA0, 0x60, 0x1C)};

constexpr char LeftHalf = static_cast<char>(0xDD);

struct MapFile {
    file_id_t                Id;
    std::string              Name;
    std::uint64_t            Bytes;
    std::vector<std::string> Tags;
};

enum class Kind { Tag, Bare, File, More };

struct Entry {
    Kind                     Is = Kind::Tag;
    std::string              Name;
    std::uint64_t            Bytes = 0;
    std::vector<std::size_t> Files;   /* places in Map::Files */
};

/* One map on the way down: the tags, a tag's files, or the small ones. */
struct Level {
    std::string             Crumb;
    std::vector<Entry>      Entries;  /* every one, biggest first */
    std::size_t             Own = 0;  /* how many have a tile of their own */
    Entry                   More;     /* the rest, when Own < Entries.size() */
    std::vector<TagmapTile> Tiles;
    std::size_t             Selected = 0;

    const Entry &Shown(std::size_t Index) const { return Index < Own ? Entries[Index] : More; }
};

std::vector<std::string> Split(std::string_view Tags)
{
    std::vector<std::string> Out;
    while (!Tags.empty()) {
        std::size_t      Comma = Tags.find(',');
        std::string_view One   = Tags.substr(0, Comma);
        std::size_t      Begin = One.find_first_not_of(' '), End = One.find_last_not_of(' ');
        if (Begin != std::string_view::npos) Out.emplace_back(One.substr(Begin, End - Begin + 1));
        if (Comma == std::string_view::npos) break;
        Tags.remove_prefix(Comma + 1);
    }
    return Out;
}

std::string Stem(const std::string &Name)
{
    std::size_t Dot = Name.rfind('.');
    return Dot == std::string::npos ? Name : Name.substr(0, Dot);
}

std::string Joined(const std::vector<std::string> &Words)
{
    std::string Out;
    for (const std::string &Word : Words) Out += (Out.empty() ? "" : ", ") + Word;
    return Out;
}

enum class Act { Stay, Leave, Give };

class Map {
public:
    Stage                    Glass;
    std::vector<MapFile>     Files;
    std::string              Scope;
    std::vector<std::string> Common;
    std::vector<Level>       Path;
    std::string              Note;

    std::optional<std::string> Gather(const file_id_t *Given, std::size_t Count);
    Level Root();
    void  Arrange(Level &Here);
    void  Draw();
    Act   OnKey(const kb_event_t &Key);
    std::string Text();

private:
    std::uint64_t Weight(const Entry &One) const { return BySize ? One.Bytes : One.Files.size(); }
    void Enter();
    void Head();
    void Foot();
    void Tile(const TagmapTile &Place, const Entry &One, bool Chosen, std::uint64_t Whole);
    std::uint64_t Whole(const Level &Here) const;

    bool BySize = true;
};

/* The files given, or the Use Context's; each with its tags, the trashed left out. */
std::optional<std::string> Map::Gather(const file_id_t *Given, std::size_t Count)
{
    file_id_t *Owned = nullptr;
    if (Count == 0) {
        int Found = query_all(nullptr, &Owned);
        if (Found < 0) return std::format("the volume would not list its files (error {})", -Found);
        Given = Owned;
        Count = static_cast<std::size_t>(Found);
        std::string Context = KeepContext();
        Scope = Context.empty() ? "the whole volume" : "the Use Context " + Context;
    } else {
        Scope = std::format("{} file{} given", Count, Count == 1 ? "" : "s");
    }
    for (std::size_t At = 0; At < Count; At++) {
        FileMetadata Metadata;
        if (FileMetadataRead(Given[At], &Metadata) != 0) continue;
        if (Metadata.Filename && !(Metadata.Flags & FILE_FLAG_TRASHED))
            Files.push_back({Given[At], Metadata.Filename, Metadata.Size, Split(Metadata.Tags ? Metadata.Tags : "")});
        FileMetadataRelease(&Metadata);
    }
    free(Owned);
    return std::nullopt;
}

/* The tags of the files, each as big as its files, less those that say
 * nothing here (the header comment). */
Level Map::Root()
{
    std::map<std::string, std::vector<std::size_t>> ByTag;
    for (std::size_t At = 0; At < Files.size(); At++)
        for (const std::string &Tag : Files[At].Tags) {
            std::vector<std::size_t> &Holders = ByTag[Tag];
            if (Holders.empty() || Holders.back() != At) Holders.push_back(At);
        }

    Level             Top;
    std::vector<bool> Placed(Files.size(), false);
    for (auto &[Tag, Holders] : ByTag) {
        if (Holders.size() == Files.size()) {
            Common.push_back(Tag);
            continue;
        }
        bool Named = std::all_of(Holders.begin(), Holders.end(), [&](std::size_t At) { return Stem(Files[At].Name) == Tag; });
        if (Named) continue;
        Entry One{Kind::Tag, Tag, 0, Holders};
        for (std::size_t At : Holders) {
            One.Bytes += Files[At].Bytes;
            Placed[At] = true;
        }
        Top.Entries.push_back(std::move(One));
    }
    Entry Bare{Kind::Bare, Common.empty() ? "untagged" : "no other tag", 0, {}};
    for (std::size_t At = 0; At < Files.size(); At++) {
        if (Placed[At]) continue;
        Bare.Files.push_back(At);
        Bare.Bytes += Files[At].Bytes;
    }
    if (!Bare.Files.empty()) Top.Entries.push_back(std::move(Bare));
    return Top;
}

/* Biggest first by the weight now chosen, as many tiles of their own as the
 * glass gives room for, the rest gathered into one; the one chosen kept. */
void Map::Arrange(Level &Here)
{
    std::string Was = Here.Selected < Here.Tiles.size() ? Here.Shown(Here.Tiles[Here.Selected].Index).Name : "";
    std::stable_sort(Here.Entries.begin(), Here.Entries.end(), [&](const Entry &A, const Entry &B) {
        return Weight(A) != Weight(B) ? Weight(A) > Weight(B) : A.Name < B.Name;
    });

    int High = Glass.Rows - 3, Wide = Glass.Cols;
    std::vector<std::uint64_t> Weights;
    for (const Entry &One : Here.Entries) Weights.push_back(Weight(One));
    std::uint64_t MoreWeight = 0;
    Here.Own = TagmapFit(Weights, High, Wide, MoreWeight);
    Weights.resize(Here.Own);

    Here.More = Entry{Kind::More, std::format("+{} more", Here.Entries.size() - Here.Own), 0, {}};
    if (Here.Own < Here.Entries.size()) {
        std::vector<bool> In(Files.size(), false);
        for (std::size_t At = Here.Own; At < Here.Entries.size(); At++) {
            Here.More.Bytes += Here.Entries[At].Bytes;
            for (std::size_t File : Here.Entries[At].Files) In[File] = true;
        }
        for (std::size_t File = 0; File < In.size(); File++)
            if (In[File]) Here.More.Files.push_back(File);
        Weights.push_back(MoreWeight);
    }
    Here.Tiles    = TagmapLay(Weights, 1, 0, High, Wide);
    Here.Selected = 0;
    for (std::size_t At = 0; At < Here.Tiles.size(); At++)
        if (Here.Shown(Here.Tiles[At].Index).Name == Was) Here.Selected = At;
}

std::uint64_t Map::Whole(const Level &Here) const
{
    std::uint64_t Sum = 0;
    for (const Entry &One : Here.Entries) Sum += Weight(One);
    return Sum;
}

/* A tile: its colour, a half-cell gap at its right and bottom edges, and in
 * it as many of its name, its size and its count as it has rows for. */
void Map::Tile(const TagmapTile &Place, const Entry &One, bool Chosen, std::uint64_t Whole)
{
    bool  Gathered = One.Is == Kind::More;
    Color Ink      = Gathered ? MoreInk : Tints[Place.Index % (sizeof(Tints) / sizeof(Tints[0]))];
    if (Chosen) Ink = Gathered ? MoreLit : color_mix(Ink, COLOR_WHITE, 120);
    Color Grain = Gathered && !Chosen ? MoreGrain : Ink;

    for (int R = 0; R < Place.High; R++) {
        for (int C = 0; C < Place.Wide; C++) {
            bool Bottom = R == Place.High - 1 && Place.High > 1, Right = C == Place.Wide - 1 && Place.Wide > 1;
            int  Row = Place.Row + R, Col = Place.Col + C;
            if (Bottom && Right) Glass.Put(Row, Col, ' ', Page, Page);
            else if (Right) Glass.Put(Row, Col, LeftHalf, Ink, Page);
            else if (Bottom) Glass.Put(Row, Col, Glyph::UpperHalf, Ink, Page);
            else Glass.Put(Row, Col, Gathered ? Glyph::LightShade : ' ', Grain, Ink);
        }
    }

    int Room = Place.Wide - 2, Rows = std::max(1, Place.High - 1);
    if (Room < 1) return;
    std::uint64_t Share = Whole ? Weight(One) * 100 / Whole : 0;
    std::string   Count = std::format("{} file{}", One.Files.size(), One.Files.size() == 1 ? "" : "s");
    std::string   Size  = TagmapSize(One.Bytes);
    std::string   Main  = BySize || One.Is == Kind::File ? Size : Count;
    std::string   Part  = Share == 0 ? "<1%" : std::format("{}%", Share);
    if (static_cast<int>(Main.size() + 2 + Part.size()) <= Room) Main += "  " + Part;
    std::string   Lines[] = {One.Name, Main, BySize ? Count : Size};
    int           Shown   = One.Is == Kind::File ? 2 : 3;
    for (int Line = 0; Line < std::min(Rows, Shown); Line++) {
        std::string Text = Lines[Line];
        if (static_cast<int>(Text.size()) > Room) Text = Text.substr(0, static_cast<std::size_t>(Room - 1)) + "~";
        Glass.Write(Place.Row + Line, Place.Col + 1, Text, Chosen ? DarkInk : Line == 0 ? TextInk : DimInk, Ink);
    }
}

void Map::Head()
{
    Glass.Fill(0, 0, Glass.Cols, ' ', TextInk, Page);
    std::string Weighed = BySize ? "by size" : "by count";
    int         Edge    = Glass.Cols - 1 - static_cast<int>(Weighed.size());
    int         Col     = Glass.Write(0, 1, "tagmap", TitleInk, Page);
    Col = Glass.Write(0, Col + 2, Scope, ScopeInk, Page);
    for (std::size_t Depth = 1; Depth < Path.size(); Depth++) {
        Col = Glass.Write(0, Col, " > ", HintInk, Page);
        Col = Glass.Write(0, Col, Path[Depth].Crumb, TextInk, Page);
    }
    if (Col < Edge) Glass.Write(0, Edge, Weighed, HintInk, Page);
}

/* What the chosen tile is, and the keys. */
void Map::Foot()
{
    const Level &Here = Path.back();
    int          Row  = Glass.Rows - 2;
    Glass.Fill(Row, 0, Glass.Cols, ' ', TextInk, Page);
    if (!Note.empty()) {
        Glass.Write(Row, 1, Note, TitleInk, Page);
    } else if (Here.Selected < Here.Tiles.size()) {
        const Entry &One   = Here.Shown(Here.Tiles[Here.Selected].Index);
        std::string  About = One.Name;
        if (One.Is == Kind::Tag) About = "tag " + About;
        if (One.Is == Kind::File) {
            const MapFile &File = Files[One.Files.front()];
            About += std::format(": {}", TagmapSize(One.Bytes));
            if (!File.Tags.empty()) About += "; wears " + Joined(File.Tags);
        } else {
            About += std::format(": {} file{}, {}", One.Files.size(), One.Files.size() == 1 ? "" : "s", TagmapSize(One.Bytes));
        }
        int Col = Glass.Write(Row, 1, About, TextInk, Page);
        if (Path.size() == 1 && !Common.empty()) {
            std::string All = "all wear " + Joined(Common);
            if (Col + 2 + static_cast<int>(All.size()) < Glass.Cols)
                Glass.Write(Row, Glass.Cols - 1 - static_cast<int>(All.size()), All, ScopeInk, Page);
        }
    }

    Row = Glass.Rows - 1;
    Glass.Fill(Row, 0, Glass.Cols, ' ', TextInk, BarPaper);
    int Col = 1;
    for (auto [Keys, What] : {std::pair{"\x1B\x18\x19\x1A", "choose"}, {"enter", "open"}, {"bksp", "back"},
                              {"space", "give"}, {"w", BySize ? "by count" : "by size"}, {"q", "leave"}}) {
        int Wide = static_cast<int>(std::string_view(Keys).size() + 1 + std::string_view(What).size() + 2);
        if (Col + Wide > Glass.Cols) break;
        Col = Glass.Write(Row, Col, Keys, KeyInk, BarPaper);
        Col = Glass.Write(Row, Col + 1, What, HintInk, BarPaper) + 2;
    }
}

void Map::Draw()
{
    Glass.Clear(Page);
    Head();
    const Level  &Here  = Path.back();
    std::uint64_t Total = Whole(Here);
    for (std::size_t At = 0; At < Here.Tiles.size(); At++)
        Tile(Here.Tiles[At], Here.Shown(Here.Tiles[At].Index), At == Here.Selected, Total);
    if (Here.Tiles.empty()) Glass.Centre(Glass.Rows / 2, "nothing here to map", HintInk, Page);
    Foot();
}

/* Into the chosen tile: a tag's files, or the ones gathered as "more". */
void Map::Enter()
{
    const Level &Here = Path.back();
    if (Here.Selected >= Here.Tiles.size()) return;
    const Entry &One = Here.Shown(Here.Tiles[Here.Selected].Index);
    Level        Down;
    Down.Crumb = One.Name;
    if (One.Is == Kind::File) {
        Note = "a file has nothing inside on this map; space gives it";
        return;
    }
    if (One.Is == Kind::More) {
        Down.Entries.assign(Here.Entries.begin() + static_cast<std::ptrdiff_t>(Here.Own), Here.Entries.end());
    } else {
        for (std::size_t At : One.Files) Down.Entries.push_back({Kind::File, Files[At].Name, Files[At].Bytes, {At}});
    }
    Path.push_back(std::move(Down));
    Arrange(Path.back());
}

Act Map::OnKey(const kb_event_t &Key)
{
    Note.clear();
    Level &Here = Path.back();
    if (Key.mods & KB_MOD_EXTENDED) {
        TagmapWay Way;
        switch (Key.scancode) {
        case KEY_LEFT:  Way = TagmapWay::Left; break;
        case KEY_RIGHT: Way = TagmapWay::Right; break;
        case KEY_UP:    Way = TagmapWay::Up; break;
        case KEY_DOWN:  Way = TagmapWay::Down; break;
        default: return Act::Stay;
        }
        Here.Selected = TagmapStep(Here.Tiles, Here.Selected, Way);
        return Act::Stay;
    }
    switch (Key.ascii) {
    case 'q':
    case 'Q':
    case KEY_ESCAPE: return Act::Leave;
    case ' ': return Here.Tiles.empty() ? Act::Stay : Act::Give;
    case '\n':
    case '\r': Enter(); break;
    case KEY_BACKSPACE:
    case 0x7F:
        if (Path.size() > 1) {
            Path.pop_back();
            Arrange(Path.back());
        }
        break;
    case 'w':
    case 'W':
        BySize = !BySize;
        Arrange(Here);
        break;
    default: break;
    }
    return Act::Stay;
}

/* The tags as text, for an arrow: a line to a tag, biggest first. */
std::string Map::Text()
{
    std::vector<Entry> Top = Path.front().Entries;
    std::stable_sort(Top.begin(), Top.end(), [](const Entry &A, const Entry &B) { return A.Bytes > B.Bytes; });
    std::string Out = std::format("tagmap of {}: {} files\n", Scope, Files.size());
    if (!Common.empty()) Out += "all wear " + Joined(Common) + "\n";
    for (const Entry &One : Top)
        Out += std::format("{:<24} {:>6} files {:>11}\n", One.Name, One.Files.size(), TagmapSize(One.Bytes));
    return Out;
}

/* The chosen tile's files given to the shell; what is said of it. */
std::string Give(const Map &Atlas)
{
    const Level &Here = Atlas.Path.back();
    const Entry &One  = Here.Shown(Here.Tiles[Here.Selected].Index);
    std::vector<file_id_t> Ids;
    for (std::size_t At : One.Files) Ids.push_back(Atlas.Files[At].Id);
    int Code = souvenir_things(Ids.data(), Ids.size());
    if (Code != 0) return std::format("tagmap: the files of {} were not given (error {})", One.Name, -Code);
    return std::format("[TAGMAP] gave the {} file{} of {}: they are `them` now", Ids.size(), Ids.size() == 1 ? "" : "s",
                       One.Name);
}

}

int main()
{
    try {
        const file_id_t *Given;
        std::size_t      Count;
        if (form_things(0, &Given, &Count) != 0) return 1;

        Map Atlas;
        if (std::optional<std::string> Refusal = Atlas.Gather(Given, Count)) {
            StageSay("tagmap: " + *Refusal);
            return 1;
        }
        if (Atlas.Files.empty()) {
            StageSay(std::format("tagmap: there are no files in {} to map", Atlas.Scope));
            return 0;
        }
        Atlas.Path.push_back(Atlas.Root());

        if (form_screen_file() != 0) {
            std::string Text = Atlas.Text();
            print_bytes(Text.data(), Text.size());
            return 0;
        }

        if (std::optional<std::string> Refusal = Atlas.Glass.Take("tagmap", 40, 12)) {
            Atlas.Glass.Give();
            StageSay(*Refusal);
            return 1;
        }
        Atlas.Arrange(Atlas.Path.back());

        bool Painted = true;
        Act  Last    = Act::Leave;
        for (;;) {
            Atlas.Draw();
            if (!(Painted = Atlas.Glass.Show())) break;
            kb_event_t Key;
            StageHeard Heard = Atlas.Glass.Listen(Key, 0);
            if (Heard == StageHeard::Gone) break;
            if (Heard != StageHeard::Key) continue;
            if ((Last = Atlas.OnKey(Key)) != Act::Stay) break;
        }
        std::uint64_t Bytes = 0;
        for (const MapFile &File : Atlas.Files) Bytes += File.Bytes;
        std::size_t Tags = static_cast<std::size_t>(std::count_if(Atlas.Path.front().Entries.begin(), Atlas.Path.front().Entries.end(),
                                                                  [](const Entry &One) { return One.Is == Kind::Tag; }));
        std::string Said = Last == Act::Give ? Give(Atlas)
                                             : std::format("[TAGMAP] {} files, {}, {} tags", Atlas.Files.size(),
                                                           TagmapSize(Bytes), Tags);
        Atlas.Glass.Give();
        StageSay(Painted ? Said : "The screen stopped taking frames.");
        return Painted ? 0 : 1;
    } catch (const std::bad_alloc &) {
        StageSay("There is not enough memory for a map of so many files.");
        return 1;
    }
}
