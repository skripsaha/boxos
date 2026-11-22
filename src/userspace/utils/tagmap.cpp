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
 * A file is in every tag it wears, so a tag is as big as all its files. A tag
 * every file here wears says nothing on a map and is left off it (the Use
 * Context's, mostly — the header says which). A file with no other is
 * "untagged". */

#include <algorithm>
#include <format>
#include <map>
#include <new>
#include <optional>

#include "box/cxx/whose.h"
#include "box/file.h"
#include "box/form.h"
#include "box/memory.h"
#include "box/print.h"

#include "keep.h"
#include "painter/core/color.h"
#include "painter/tui/glyph.h"
#include "painter/tui/stage.h"
#include "painter/tui/text.h"
#include "tagmap.h"

BOX_FORM("takes optional things \"the files to map; with none, the Use Context\"\n"
         "gives things\n");

namespace {

using Color = painter::core::Color;
namespace Colors = painter::core::Colors;
using painter::tui::EventKind;
using painter::tui::KeyEvent;
using painter::tui::Stage;
using painter::tui::StageError;
using painter::tui::TextStyle;

const Color Page      = Color::Rgb(0x12, 0x12, 0x12);
const Color BarPaper  = Color::Rgb(0x3A, 0x3A, 0x3A);
const Color TitleInk  = Colors::Amber;
const Color ScopeInk  = Color::Rgb(0x60, 0xA0, 0xE0);
const Color TextInk   = Color::Rgb(0xEE, 0xEE, 0xEE);
const Color DimInk    = Color::Rgb(0xC0, 0xC0, 0xC0);
const Color HintInk   = Color::Rgb(0x8C, 0x8C, 0x8C);
const Color KeyInk    = Colors::Amber;
const Color DarkInk   = Color::Rgb(0x10, 0x10, 0x10);
const Color MoreInk   = Color::Rgb(0x40, 0x40, 0x40);
const Color MoreGrain = Color::Rgb(0x5C, 0x5C, 0x5C);
const Color MoreLit   = Color::Rgb(0xB4, 0xB4, 0xB4);

/* One of each of the console's six dark hues, so neighbours differ there too;
 * lit, each is its light twin. */
const Color Tints[] = {Color::Rgb(0x24, 0x58, 0xB0), Color::Rgb(0x2A, 0x80, 0x40), Color::Rgb(0xA8, 0x2C, 0x2C),
                       Color::Rgb(0x1C, 0x7C, 0x84), Color::Rgb(0x8A, 0x2E, 0x9A), Color::Rgb(0xA0, 0x60, 0x1C)};

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

std::string Joined(const std::vector<std::string> &Words)
{
    std::string Out;
    for (const std::string &Word : Words) Out += (Out.empty() ? "" : ", ") + Word;
    return Out;
}

enum class Act { Stay, Leave, Give };

class Map {
public:
    std::optional<Stage>      Screen;
    std::vector<MapFile>     Files;
    std::string              Scope;
    std::vector<std::string> Common;
    std::vector<Level>       Path;
    std::string              Note;

    std::optional<std::string> Gather(const file_id_t *Given, std::size_t Count);
    Level Root();
    void  Arrange(Level &Here);
    void  Draw();
    Act   OnKey(const KeyEvent &Key);
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

/* The files given, or the Use Context's; each with its tags. */
std::optional<std::string> Map::Gather(const file_id_t *Given, std::size_t Count)
{
    file_id_t *Owned = nullptr;
    if (Count == 0) {
        int Found = query_all(box::tagfs::person_tags, nullptr, nullptr, &Owned);
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
        if (FileMetadataRead(box::tagfs::person_tags, Given[At], &Metadata) != 0) continue;
        if (Metadata.Filename)
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

    const painter::core::Size Dimensions = Screen->Size();
    int High = Dimensions.Height - 3, Wide = Dimensions.Width;
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
    painter::tui::Frame &FrameBuffer = Screen->FrameBuffer();
    bool  Gathered = One.Is == Kind::More;
    Color Ink      = Gathered ? MoreInk : Tints[Place.Index % (sizeof(Tints) / sizeof(Tints[0]))];
    if (Chosen) Ink = Gathered ? MoreLit : Color::Between(Ink, Colors::White, 120);
    Color Grain = Gathered && !Chosen ? MoreGrain : Ink;

    for (int R = 0; R < Place.High; R++) {
        for (int C = 0; C < Place.Wide; C++) {
            bool Bottom = R == Place.High - 1 && Place.High > 1, Right = C == Place.Wide - 1 && Place.Wide > 1;
            int  Row = Place.Row + R, Col = Place.Col + C;
            if (Bottom && Right) FrameBuffer.Put({Col, Row}, ' ', TextStyle{Page, Page});
            else if (Right) FrameBuffer.Put({Col, Row}, painter::tui::Glyphs::LeftHalf, TextStyle{Ink, Page});
            else if (Bottom) FrameBuffer.Put({Col, Row}, painter::tui::Glyphs::UpperHalf, TextStyle{Ink, Page});
            else FrameBuffer.Put({Col, Row}, Gathered ? painter::tui::Glyphs::LightShade : ' ',
                                 TextStyle{Grain, Ink});
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
        FrameBuffer.Write({Place.Col + 1, Place.Row + Line}, Text,
                          TextStyle{Chosen ? DarkInk : Line == 0 ? TextInk : DimInk, Ink});
    }
}

void Map::Head()
{
    painter::tui::Frame &FrameBuffer = Screen->FrameBuffer();
    const painter::core::Size Dimensions = Screen->Size();
    FrameBuffer.Fill({0, 0, Dimensions.Width, 1}, ' ', TextStyle{TextInk, Page});
    std::string Weighed = BySize ? "by size" : "by count";
    int         Edge    = Dimensions.Width - 1 - static_cast<int>(Weighed.size());
    int         Col = 1;
    Col += static_cast<int>(FrameBuffer.Write({Col, 0}, "tagmap", TextStyle{TitleInk, Page}));
    Col += 2;
    Col += static_cast<int>(FrameBuffer.Write({Col, 0}, Scope, TextStyle{ScopeInk, Page}));
    for (std::size_t Depth = 1; Depth < Path.size(); Depth++) {
        Col += static_cast<int>(FrameBuffer.Write({Col, 0}, " > ", TextStyle{HintInk, Page}));
        Col += static_cast<int>(FrameBuffer.Write({Col, 0}, Path[Depth].Crumb, TextStyle{TextInk, Page}));
    }
    if (Col < Edge) FrameBuffer.Write({Edge, 0}, Weighed, TextStyle{HintInk, Page});
}

/* What the chosen tile is, and the keys. */
void Map::Foot()
{
    painter::tui::Frame &FrameBuffer = Screen->FrameBuffer();
    const painter::core::Size Dimensions = Screen->Size();
    const Level &Here = Path.back();
    int          Row  = Dimensions.Height - 2;
    FrameBuffer.Fill({0, Row, Dimensions.Width, 1}, ' ', TextStyle{TextInk, Page});
    if (!Note.empty()) {
        FrameBuffer.Write({1, Row}, Note, TextStyle{TitleInk, Page});
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
        int Col = 1 + static_cast<int>(FrameBuffer.Write({1, Row}, About, TextStyle{TextInk, Page}));
        if (Path.size() == 1 && !Common.empty()) {
            std::string All = "all wear " + Joined(Common);
            if (Col + 2 + static_cast<int>(All.size()) < Dimensions.Width)
                FrameBuffer.Write({Dimensions.Width - 1 - static_cast<int>(All.size()), Row}, All,
                                  TextStyle{ScopeInk, Page});
        }
    }

    Row = Dimensions.Height - 1;
    FrameBuffer.Fill({0, Row, Dimensions.Width, 1}, ' ', TextStyle{TextInk, BarPaper});
    int Col = 1;
    for (auto [Keys, What] : {std::pair{"\u2190\u2191\u2193\u2192", "choose"}, {"enter", "open"}, {"bksp", "back"},
                              {"space", "give"}, {"w", BySize ? "by count" : "by size"}, {"q", "leave"}}) {
        int Wide = static_cast<int>(painter::tui::CellsOf(Keys) + 1 + std::string_view(What).size() + 2);
        if (Col + Wide > Dimensions.Width) break;
        Col += static_cast<int>(FrameBuffer.Write({Col, Row}, Keys, TextStyle{KeyInk, BarPaper}));
        Col += 1;
        Col += static_cast<int>(FrameBuffer.Write({Col, Row}, What, TextStyle{HintInk, BarPaper}));
        Col += 2;
    }
}

void Map::Draw()
{
    const painter::core::Size Dimensions = Screen->Size();
    Screen->FrameBuffer().Clear(TextStyle{Page, Page});
    Head();
    const Level  &Here  = Path.back();
    std::uint64_t Total = Whole(Here);
    for (std::size_t At = 0; At < Here.Tiles.size(); At++)
        Tile(Here.Tiles[At], Here.Shown(Here.Tiles[At].Index), At == Here.Selected, Total);
    if (Here.Tiles.empty()) Screen->FrameBuffer().Centre(Dimensions.Height / 2, "nothing here to map",
                                                        TextStyle{HintInk, Page});
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

Act Map::OnKey(const KeyEvent &Key)
{
    Note.clear();
    Level &Here = Path.back();
    if (Key.IsExtended()) {
        TagmapWay Way;
        switch (Key.Scancode) {
        case painter::tui::KeyCodes::Left:  Way = TagmapWay::Left; break;
        case painter::tui::KeyCodes::Right: Way = TagmapWay::Right; break;
        case painter::tui::KeyCodes::Up:    Way = TagmapWay::Up; break;
        case painter::tui::KeyCodes::Down:  Way = TagmapWay::Down; break;
        default: return Act::Stay;
        }
        Here.Selected = TagmapStep(Here.Tiles, Here.Selected, Way);
        return Act::Stay;
    }
    switch (Key.Character) {
    case 'q':
    case 'Q':
    case painter::tui::KeyCodes::Escape: return Act::Leave;
    case ' ': return Here.Tiles.empty() ? Act::Stay : Act::Give;
    case '\n':
    case '\r': Enter(); break;
    case painter::tui::KeyCodes::Backspace:
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

void Say(std::string_view Text)
{
    std::string Line(Text);
    Line += '\n';
    print_to_screen(Line.c_str());
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
            Say("tagmap: " + *Refusal);
            return 1;
        }
        if (Atlas.Files.empty()) {
            Say(std::format("tagmap: there are no files in {} to map", Atlas.Scope));
            return 0;
        }
        Atlas.Path.push_back(Atlas.Root());

        if (form_screen_file() != 0) {
            std::string Text = Atlas.Text();
            print_bytes(Text.data(), Text.size());
            return 0;
        }

        std::expected<Stage, StageError> Opened = Stage::Open(40, 12);
        if (!Opened) {
            Say(Opened.error().Describe("tagmap"));
            return 1;
        }
        Atlas.Screen = std::move(*Opened);
        Atlas.Arrange(Atlas.Path.back());

        bool Painted = true;
        Act  Last    = Act::Leave;
        for (;;) {
            Atlas.Draw();
            if (!(Painted = static_cast<bool>(Atlas.Screen->Present()))) break;
            const painter::tui::Event Event = Atlas.Screen->Wait();
            if (Event.Kind == EventKind::Gone) break;
            if (Event.Kind != EventKind::Key) continue;
            if ((Last = Atlas.OnKey(Event.Key)) != Act::Stay) break;
        }
        std::uint64_t Bytes = 0;
        for (const MapFile &File : Atlas.Files) Bytes += File.Bytes;
        std::size_t Tags = static_cast<std::size_t>(std::count_if(Atlas.Path.front().Entries.begin(), Atlas.Path.front().Entries.end(),
                                                                  [](const Entry &One) { return One.Is == Kind::Tag; }));
        std::string Said = Last == Act::Give ? Give(Atlas)
                                             : std::format("[TAGMAP] {} files, {}, {} tags", Atlas.Files.size(),
                                                           TagmapSize(Bytes), Tags);
        (void)Atlas.Screen->Close();
        Say(Painted ? Said : "The screen stopped taking frames.");
        return Painted ? 0 : 1;
    } catch (const std::bad_alloc &) {
        Say("There is not enough memory for a map of so many files.");
        return 1;
    }
}
