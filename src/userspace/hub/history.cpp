/* history.cpp — the file `history`: a line for each command, as it was typed —
 * the Use Context it was typed in, a tab, the command, a newline. The editor
 * types no tab, so the first one ends the context.
 *
 * The file is the one account. A line typed here is added to its end in one
 * write the volume lays after whatever another hub added meanwhile
 * (file_append), and then read back from where this hub had read to: its own
 * line and every other hub's come in by the same road, in the file's order.
 * Every write to the file is said on its tags, so a hub learns that another
 * added a line without asking. A file the person erased is made again with
 * the next line. */

#include "history.h"

#include "box/error.h"
#include "box/file.h"

#include "volume.h"

namespace hub {

namespace {

constexpr const char *FileName = "history";
constexpr const char *FileTags = "hub,history";
constexpr const char *GrownTag = "history";

// What a line may hold: anything but a byte below a blank — a tab or a line
// end would cut it — and DEL. Bytes past ASCII are kept as they are.
bool Usable(std::string_view Text)
{
    for (unsigned char C : Text)
        if (C < 0x20 || C == 0x7F) return false;
    return true;
}

// The file on the whole volume: called history, wearing both tags. 0 when
// there is none.
file_id_t Find()
{
    file_id_t *Ids = nullptr;
    const int  Count = query_all_everywhere(FileTags, &Ids);
    for (file_id_t Id : volume::Taken(Ids, Count)) {
        const std::expected<volume::Record, int> Held = volume::Record::Of(Id);
        if (Held && Held->Name() == FileName) return Id;
    }
    return 0;
}

}  // namespace

void History::Begin()
{
    Grown = touch_pair_choose(touch_intern(GrownTag));
    if (Grown != TOUCH_TAG_INVALID && touch_claim(Grown, TOUCH_REST, 0, 0) != 0) Grown = TOUCH_TAG_INVALID;
    File = Find();
    if (File != 0) ReadOn();
}

void History::Remember(std::string_view Text, std::string_view Context)
{
    if (Text.empty() || !Usable(Text) || !Usable(Context)) return;
    if (!Lines.empty() && Lines.back().Text == Text && Lines.back().Context == Context) return;
    Lines.push_back(Line{std::string(Text), std::string(Context)});
}

// The file read on from where it was read to, a whole line at a time. A file
// other than the one read so far — the person erased that one — or the same
// made shorter is the account now: what it says replaces what was read of
// the old, rather than coming in twice.
void History::ReadOn()
{
    const std::expected<volume::Record, int> Held = volume::Record::Of(File);
    if (!Held) return;
    if (File != KnownOf || Held->Size() < Known) {
        if (KnownOf != 0) Lines.clear();
        KnownOf = File;
        Known   = 0;
    }
    if (Held->Size() == Known) return;

    const std::expected<std::string, int> Text = volume::Read(File, Known, Held->Size() - Known);
    if (!Text) return;
    std::size_t At = 0;
    for (std::size_t End; (End = Text->find('\n', At)) != std::string::npos; At = End + 1) {
        const std::string_view Entry(Text->data() + At, End - At);
        const std::size_t      Tab = Entry.find('\t');
        if (Tab == std::string_view::npos) Remember(Entry, {});
        else Remember(Entry.substr(Tab + 1), Entry.substr(0, Tab));
    }
    Known += At;   // a line not ended yet is read when it is
}

void History::Catch()
{
    bool  Added = false;
    Touch Said{};
    while (Grown != TOUCH_TAG_INVALID && touch_try_pop_tag(Grown, &Said))
        Added |= (Said.flags & TOUCH_FLAG_TAGFS) != 0;
    if (!Added) return;
    if (File == 0) File = Find();
    if (File != 0) ReadOn();
}

// The file found, or made when there is none — outside the Use Context, so it
// wears only its own tags. Not reached: the volume's refusal.
std::expected<file_id_t, int> History::Reach()
{
    if (File != 0) return File;
    File = Find();
    if (File != 0) return File;
    const int64_t Made = create_everywhere(FileName, FileTags);
    if (Made < 0) return std::unexpected(static_cast<int>(Made));
    File = static_cast<file_id_t>(Made);
    if (File == 0) return std::unexpected(-ERR_UNKNOWN);   // create answers an id or a refusal, never 0
    return File;
}

// One record added to the end of the file and read back. The file known may
// have been erased since: then it is found or made again, and the record goes
// there. Not kept whole: why.
std::optional<History::Unkept> History::Write(const std::string &Record)
{
    std::expected<file_id_t, int> Reached = Reach();
    int64_t                       Wrote   = Reached ? file_append(*Reached, Record.data(), Record.size()) : 0;
    if (Reached && Wrote < 0) {
        File    = 0;
        Reached = Reach();
        if (Reached) Wrote = file_append(*Reached, Record.data(), Record.size());
    }
    if (!Reached) return Unkept{Reached.error()};
    if (Wrote < 0) return Unkept{static_cast<int>(Wrote)};
    ReadOn();
    if (Wrote != static_cast<int64_t>(Record.size())) return Unkept{0};
    return std::nullopt;
}

std::optional<History::Unkept> History::Keep(std::string_view Text, std::string_view Context, bool ToFile)
{
    if (Text.empty() || !Usable(Text) || !Usable(Context)) return std::nullopt;
    Catch();
    if (!Lines.empty() && Lines.back().Text == Text && Lines.back().Context == Context) return std::nullopt;
    if (!ToFile) {
        Remember(Text, Context);
        return std::nullopt;
    }

    std::string Record;
    Record.reserve(Context.size() + Text.size() + 2);
    Record.append(Context).append(1, '\t').append(Text).append(1, '\n');
    const std::optional<Unkept> Trouble = Write(Record);
    if (!Trouble) return std::nullopt;

    // Read back, a line the volume took only in part is held as it was typed.
    if (Lines.empty() || Lines.back().Text != Text || Lines.back().Context != Context) Remember(Text, Context);
    if (Told) return std::nullopt;
    Told = true;
    return Trouble;
}

std::vector<std::size_t> History::Walk(std::string_view Context, bool HereFirst) const
{
    std::vector<std::size_t> Order;
    Order.reserve(Lines.size());
    if (HereFirst) {
        for (std::size_t Back = 0; Back < Lines.size(); Back++)
            if (Before(Back).Context == Context) Order.push_back(Back);
        for (std::size_t Back = 0; Back < Lines.size(); Back++)
            if (Before(Back).Context != Context) Order.push_back(Back);
    } else {
        for (std::size_t Back = 0; Back < Lines.size(); Back++) Order.push_back(Back);
    }
    return Order;
}

}  // namespace hub
