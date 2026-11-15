/* retag — one tag made another on many files at once, after a look at what
 * will change and a yes.
 *
 *   retag project:old project:new           that tag becomes that one
 *   retag project:... proj:...              the key is renamed, each value kept:
 *                                           project:a becomes proj:a
 *   retag draft:... status:draft            every tag of the key becomes the one
 *   retag draft wip them                    only among what the last command gave
 *   retag draft wip everywhere all          on the whole volume
 *
 * `key:...` is every tag of the key, as a query says it; a tag without it is
 * that tag alone, a bare one too.
 *
 * With no files said, it is the files of the Use Context that wear the tag.
 * What will change is shown first, grouped by what becomes what, with the
 * files of each; nothing is changed until the person says yes. The new tag
 * goes on before the old one comes off — save where the volume can take the
 * old one off only with others of its key (Apply) — so a refusal halfway
 * leaves a file wearing both, never neither. The volume's own keys — name and
 * the others the volume keeps for itself — are not changed this way. The files
 * changed are given back: `retag project:new project:old them` takes it back.
 */

#include <algorithm>
#include <format>
#include <map>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "box/color.h"
#include "box/cxx/error.h"
#include "box/display.h"
#include "box/file.h"
#include "box/form.h"
#include "box/memory.h"
#include "box/print.h"
#include "box/use.h"
#include "tagfs_reserved.h"

BOX_FORM("takes words \"the tag, then what it becomes: project:old project:new; key:... is every tag of the key\"\n"
         "takes optional things \"which files: them, files, everywhere all; the Use Context if none\"\n"
         "gives things\n");

namespace {

constexpr Color OldInk  = COLOR_CORAL;
constexpr Color NewInk  = COLOR_LEAF;
constexpr Color NameInk = COLOR_SLATE;

#define RETAG_RESERVED(Name, Key) Key,
constexpr std::string_view Reserved[] = {TAGFS_RESERVED_KEYS(RETAG_RESERVED)};
#undef RETAG_RESERVED

/* A tag as the volume spells it: blanks around it dropped, the first colon
 * between key and value, "key:" the bare key, "key:..." every tag of the key. */
struct Tag {
    std::string Key;
    std::string Value;

    bool        Bare() const noexcept { return Value.empty(); }
    bool        Every() const noexcept { return Value == "..."; }
    std::string Spelled() const { return Bare() ? Key : Key + ":" + Value; }
    bool        operator==(const Tag &) const = default;
};

std::string_view Trimmed(std::string_view Text)
{
    while (!Text.empty() && Text.front() == ' ') Text.remove_prefix(1);
    while (!Text.empty() && Text.back() == ' ') Text.remove_suffix(1);
    return Text;
}

std::optional<Tag> Spell(std::string_view Text)
{
    Text                    = Trimmed(Text);
    const std::size_t Colon = Text.find(':');
    Tag               One{std::string(Trimmed(Text.substr(0, Colon))), {}};
    if (Colon != std::string_view::npos) One.Value = std::string(Trimmed(Text.substr(Colon + 1)));
    if (One.Key.empty()) return std::nullopt;
    return One;
}

/* The tags a file wears, as its metadata lists them. */
std::vector<Tag> TagsOf(std::string_view List)
{
    std::vector<Tag> Tags;
    std::size_t      At = 0;
    while (At <= List.size()) {
        const std::size_t Comma = std::min(List.find(',', At), List.size());
        if (std::optional<Tag> One = Spell(List.substr(At, Comma - At))) Tags.push_back(std::move(*One));
        At = Comma + 1;
    }
    return Tags;
}

bool IsReserved(std::string_view Key)
{
    return std::find(std::begin(Reserved), std::end(Reserved), Key) != std::end(Reserved);
}

void Say(std::string_view Text, Color Ink = COLOR_DEFAULT)
{
    if (Text.empty()) return;
    set_color(Ink);
    print_bytes_to_screen(Text.data(), Text.size());
    set_color(COLOR_DEFAULT);
}

std::string Why(int Answer)
{
    return std::string(box::error(box_errno_of(Answer)).message());
}

std::string Plural(std::size_t Count, std::string_view One, std::string_view Many)
{
    return std::format("{} {}", Count, Count == 1 ? One : Many);
}

std::string ContextTags()
{
    std::size_t Needed = 0;
    if (use_get(nullptr, 0, &Needed) < 0 || Needed <= 1) return {};
    std::string Tags(Needed, '\0');
    if (use_get(Tags.data(), Tags.size(), &Needed) < 0) return {};
    Tags.resize(Tags.find('\0'));
    return Tags;
}

std::size_t GlassWidth()
{
    std::uint32_t Cols = 0, Rows = 0;
    if (display_size(&Cols, &Rows) != 0 || Cols == 0) return 80;
    return Cols;
}

/* What the person asked: the tag, and what it becomes. */
class Change {
public:
    Change(Tag FromValue, Tag ToValue) : From(std::move(FromValue)), To(std::move(ToValue)) {}

    /* Does a tag a file wears fall under the change: the very tag, or any
     * tag of the key said as key:... */
    bool Covers(const Tag &Worn) const
    {
        return From.Every() ? Worn.Key == From.Key : Worn == From;
    }

    /* What a tag under the change becomes: key:... keeps the tag's value
     * under the new key; anything else is the one tag said. */
    Tag Becomes(const Tag &Worn) const
    {
        if (To.Every()) return {To.Key, Worn.Value};
        return To;
    }

    const Tag &Asked() const noexcept { return From; }

private:
    Tag From;
    Tag To;
};

/* A tag of a file that becomes another. Wore: the file wears the other
 * already. Puts: this move is the one that puts it on — not when the file
 * wore it, nor when an earlier move of the same file puts the same tag on. */
struct Move {
    Tag  Old;
    Tag  New;
    bool Wore = false;
    bool Puts = false;
};

/* One file's part of the change, and every tag it wears. */
struct Plan {
    file_id_t         File = 0;
    std::string       Name;
    std::vector<Tag>  Wears;
    std::vector<Move> Moves;
};

std::optional<Plan> PlanFor(file_id_t File, const Change &Asked)
{
    FileMetadata Facts;
    if (FileMetadataRead(File, &Facts) != 0) return std::nullopt;
    Plan One;
    One.File        = File;
    One.Name        = Facts.Filename ? Facts.Filename : "";
    One.Wears       = TagsOf(Facts.Tags ? Facts.Tags : "");
    const bool Gone = Facts.KeptUntil != 0;
    FileMetadataRelease(&Facts);
    if (Gone) return std::nullopt;

    const std::vector<Tag> &Wears = One.Wears;
    for (const Tag &Worn : Wears) {
        if (!Asked.Covers(Worn)) continue;
        Move Next{Worn, Asked.Becomes(Worn)};
        if (Next.New == Worn) continue;
        Next.Wore = std::find(Wears.begin(), Wears.end(), Next.New) != Wears.end();
        Next.Puts = !Next.Wore && std::none_of(One.Moves.begin(), One.Moves.end(),
                                               [&](const Move &Earlier) { return Earlier.New == Next.New; });
        One.Moves.push_back(std::move(Next));
    }
    if (One.Moves.empty()) return std::nullopt;
    return One;
}

/* The names of a group, as many to a row as the glass is wide. */
void SayNames(const std::vector<std::string> &Names, std::size_t Width)
{
    std::string Row = "    ";
    for (std::size_t Index = 0; Index < Names.size(); Index++) {
        const std::string Piece = Names[Index] + (Index + 1 < Names.size() ? ", " : "");
        if (Row.size() > 4 && Row.size() + Piece.size() >= Width) {
            Say(Row + "\n", NameInk);
            Row = "    ";
        }
        Row += Piece;
    }
    Say(Row + "\n", NameInk);
}

/* What will change, grouped by what becomes what, the largest group first. */
void Preview(const std::vector<Plan> &Plans)
{
    struct Group {
        std::vector<std::string> Names;
        std::size_t              Already = 0;
    };
    std::map<std::pair<std::string, std::string>, Group> Groups;
    for (const Plan &One : Plans)
        for (const Move &Each : One.Moves) {
            Group &Into = Groups[{Each.Old.Spelled(), Each.New.Spelled()}];
            Into.Names.push_back(One.Name);
            if (Each.Wore) Into.Already++;
        }

    std::vector<std::pair<std::pair<std::string, std::string>, Group>> Ordered(Groups.begin(), Groups.end());
    std::stable_sort(Ordered.begin(), Ordered.end(),
                     [](const auto &A, const auto &B) { return A.second.Names.size() > B.second.Names.size(); });

    const std::size_t Width = GlassWidth();
    for (auto &[Pair, Into] : Ordered) {
        std::sort(Into.Names.begin(), Into.Names.end());
        Say("  ");
        Say(Pair.first, OldInk);
        Say(" -> ");
        Say(Pair.second, NewInk);
        Say(std::format("   {}\n", Plural(Into.Names.size(), "file", "files")));
        SayNames(Into.Names, Width);
        if (Into.Already != 0)
            Say(std::format("    {} of them {} {} already: only {} comes off\n", Into.Already,
                            Into.Already == 1 ? "wears" : "wear", Pair.second, Pair.first));
    }
}

/* Yes or no from the person; nobody there to answer is a no. */
bool Agreed(std::size_t Files)
{
    for (;;) {
        Say(std::format("change {}? (yes or no) ", Plural(Files, "file", "files")));
        char *Typed = nullptr;
        if (readline_grow(&Typed) < 0) {
            Say("\n");
            return false;
        }
        const std::string Answer(Trimmed(Typed));
        free(Typed);
        if (Answer == "yes" || Answer == "y") return true;
        if (Answer == "no" || Answer == "n") return false;
        Say("please answer yes or no. ");
    }
}

/* The volume takes a bare tag off only as its key — every tag of the key at
 * once. So the tags of such a key the file is to keep — ones the change does
 * not touch, ones it makes — come off with it, and are put back on after.
 * Where nothing has to be put back, every new tag goes on before any old one
 * comes off, so a refusal halfway leaves a file wearing both, never neither. */
std::optional<std::string> Apply(const Plan &One)
{
    std::vector<std::string> KeysWhole;
    for (const Move &Each : One.Moves)
        if (Each.Old.Bare()) KeysWhole.push_back(Each.Old.Key);
    const auto OffWhole = [&](const Tag &Kept) {
        return std::find(KeysWhole.begin(), KeysWhole.end(), Kept.Key) != KeysWhole.end();
    };

    /* What the file wears once the change is made, and of that what a key
     * coming off whole would take with it. */
    std::vector<Tag> Kept;
    for (const Tag &Worn : One.Wears)
        if (std::none_of(One.Moves.begin(), One.Moves.end(), [&](const Move &Each) { return Each.Old == Worn; }))
            Kept.push_back(Worn);
    for (const Move &Each : One.Moves)
        if (std::find(Kept.begin(), Kept.end(), Each.New) == Kept.end()) Kept.push_back(Each.New);
    std::vector<Tag> Restore;
    for (const Tag &Each : Kept)
        if (OffWhole(Each)) Restore.push_back(Each);

    const auto TakeOff = [&]() -> std::optional<std::string> {
        for (const Move &Each : One.Moves) {
            const std::string Text   = Each.Old.Spelled();
            const int         Answer = tag_remove(One.File, Text.c_str());
            if (Answer != 0 && box_errno_of(Answer) != ERR_TAGFS_TAG_NOT_FOUND)
                return std::format("{} would not come off: {}", Text, Why(Answer));
        }
        return std::nullopt;
    };
    const auto PutOn = [&](const std::vector<Tag> &Tags) -> std::optional<std::string> {
        for (const Tag &Each : Tags) {
            const std::string Text = Each.Spelled();
            if (const int Answer = tag_add(One.File, Text.c_str()); Answer != 0)
                return std::format("{} would not go on: {}", Text, Why(Answer));
        }
        return std::nullopt;
    };

    std::vector<Tag> New;
    for (const Move &Each : One.Moves)
        if (Each.Puts) New.push_back(Each.New);

    if (Restore.empty()) {
        if (std::optional<std::string> Refusal = PutOn(New)) return Refusal;
        return TakeOff();
    }
    if (std::optional<std::string> Refusal = TakeOff()) return Refusal;
    for (const Tag &Put : New)
        if (std::find(Restore.begin(), Restore.end(), Put) == Restore.end()) Restore.push_back(Put);
    return PutOn(Restore);
}

int Retag(const std::vector<std::string> &Words, std::vector<file_id_t> Where, bool Given)
{
    if (Words.size() != 2) {
        Say("retag: say the tag and what it becomes: retag project:old project:new\n");
        return 1;
    }
    const std::optional<Tag> From = Spell(Words[0]), To = Spell(Words[1]);
    if (!From || !To) {
        Say(std::format("retag: {} is no tag\n", From ? Words[1] : Words[0]));
        return 1;
    }
    for (const Tag *One : {&*From, &*To})
        if (IsReserved(One->Key)) {
            Say(std::format("retag: {} is a key the volume keeps for itself, and is not changed this way\n", One->Key));
            return 1;
        }
    if (To->Every() && !From->Every()) {
        Say(std::format("retag: only {}:... can become {}\n", From->Key, To->Spelled()));
        return 1;
    }
    const Change Asked(*From, *To);

    std::string Place;
    if (Given) {
        Place = "of the " + Plural(Where.size(), "file given", "files given");
    } else {
        const std::string Text  = From->Spelled();
        file_id_t        *Ids   = nullptr;
        const int         Count = query_all(Text.c_str(), nullptr, &Ids);
        if (Count < 0) {
            Say(std::format("retag: the volume would not say which files wear {}: {}\n", Text, Why(Count)));
            return 1;
        }
        Where.assign(Ids, Ids + Count);
        free(Ids);
        const std::string Context = ContextTags();
        Place = Context.empty() ? std::string("on the whole volume") : "in the Use Context " + Context;
    }

    std::vector<Plan> Plans;
    for (const file_id_t File : Where)
        if (std::optional<Plan> One = PlanFor(File, Asked)) Plans.push_back(std::move(*One));
    if (Plans.empty()) {
        Say(std::format("retag: no file {} wears {} that would change\n", Place, From->Spelled()));
        return 0;
    }

    Say(std::format("retag: {} {} will change\n", Plural(Plans.size(), "file", "files"), Place));
    Preview(Plans);
    if (!Agreed(Plans.size())) {
        Say("retag: nothing was changed\n");
        return 0;
    }

    std::vector<file_id_t>   Changed;
    std::vector<std::string> Refused;
    for (const Plan &One : Plans) {
        if (std::optional<std::string> Refusal = Apply(One)) Refused.push_back(One.Name + ": " + *Refusal);
        else                                             Changed.push_back(One.File);
    }
    const std::string Done = std::format("retag: {} changed\n", Plural(Changed.size(), "file", "files"));
    print_bytes(Done.data(), Done.size());
    for (const std::string &Line : Refused) Say("  " + Line + "\n", OldInk);
    souvenir_things(Changed.data(), Changed.size());
    return Refused.empty() ? 0 : 1;
}

}  // namespace

int main()
{
    try {
        const char *const *Words;
        std::size_t        WordCount;
        const file_id_t   *Files;
        std::size_t        FileCount;
        if (form_words(0, &Words, &WordCount) != 0 || form_things(1, &Files, &FileCount) != 0) return 1;
        return Retag(std::vector<std::string>(Words, Words + WordCount),
                     std::vector<file_id_t>(Files, Files + FileCount), FileCount != 0);
    } catch (const std::bad_alloc &) {
        Say("retag: there is no memory to plan the change in\n");
        return 1;
    }
}
