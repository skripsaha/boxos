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
#include "say.h"

#include <cstring>

#include "box/error.h"
#include "box/file.h"
#include "box/memory.h"
#include "box/print.h"

namespace hub {

namespace {

constexpr const char   *FileName = "history";
constexpr const char   *FileTags = "hub,history";
constexpr const char   *GrownTag = "history";
constexpr std::size_t   Piece    = 64 * 1024;

// What a line may hold: anything but a byte below a blank — a tab or a line
// end would cut it — and DEL. Bytes past ASCII are kept as they are.
bool Usable(std::string_view Text)
{
    for (unsigned char C : Text)
        if (C < 0x20 || C == 0x7F) return false;
    return true;
}

// The file on the whole volume: called history, wearing both tags, not in
// the trash. 0 when there is none.
file_id_t Find()
{
    file_id_t *Ids   = nullptr;
    int        Count = query_all_everywhere(FileTags, &Ids);
    file_id_t  Found = 0;
    for (int I = 0; I < Count && Found == 0; I++) {
        FileMetadata Metadata;
        if (FileMetadataRead(Ids[I], &Metadata) != 0) continue;
        if (Metadata.Filename && std::strcmp(Metadata.Filename, FileName) == 0 &&
            !(Metadata.Flags & FILE_FLAG_TRASHED))
            Found = Ids[I];
        FileMetadataRelease(&Metadata);
    }
    free(Ids);
    return Found;
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

// The file read on from where it was read to, a whole line at a time.
void History::ReadOn()
{
    FileMetadata Metadata;
    if (FileMetadataRead(File, &Metadata) != 0) return;
    const std::uint64_t Size = Metadata.Size;
    FileMetadataRelease(&Metadata);
    if (Size < Known) Known = 0;   // the file was made shorter: read it again whole
    if (Size == Known) return;

    std::string Text(static_cast<std::size_t>(Size - Known), '\0');
    std::size_t Read = 0;
    while (Read < Text.size()) {
        const std::size_t Want = Text.size() - Read < Piece ? Text.size() - Read : Piece;
        int64_t           Got  = fread(File, Known + Read, Text.data() + Read, Want);
        if (Got <= 0) break;
        Read += static_cast<std::size_t>(Got);
    }
    Text.resize(Read);

    std::size_t At = 0;
    for (;;) {
        const std::size_t End = Text.find('\n', At);
        if (End == std::string::npos) break;   // a line not ended yet is read when it is
        std::string_view Record(Text.data() + At, End - At);
        const std::size_t Tab = Record.find('\t');
        if (Tab == std::string_view::npos) Remember(Record, {});
        else Remember(Record.substr(Tab + 1), Record.substr(0, Tab));
        At = End + 1;
    }
    Known += At;
}

void History::Catch()
{
    bool Added = false;
    Touch Said{};
    while (Grown != TOUCH_TAG_INVALID && touch_try_pop_tag(Grown, &Said))
        Added |= (Said.flags & TOUCH_FLAG_TAGFS) != 0;
    if (!Added) return;
    if (File == 0) File = Find();
    if (File != 0) ReadOn();
}

// The file found, or made when there is none — outside the Use Context, so it
// wears only its own tags. False when the volume has none to give.
bool History::Reach()
{
    if (File != 0) return true;
    File  = Find();
    Known = 0;
    if (File == 0) {
        int64_t Made = create_everywhere(FileName, FileTags);
        if (Made > 0) File = static_cast<file_id_t>(Made);
    }
    return File != 0;
}

// One record added to the end of the file and read back. The file known may
// have been erased since: then it is found or made again, and the record goes
// there. A record the volume would not keep is held here.
void History::Write(std::string_view Record)
{
    int64_t Wrote = -ERR_IO;
    if (Reach()) {
        Wrote = file_append(File, Record.data(), Record.size());
        if (Wrote < 0) {
            File = 0;
            if (Reach()) Wrote = file_append(File, Record.data(), Record.size());
        }
    }
    if (Wrote == static_cast<int64_t>(Record.size())) {
        ReadOn();
        return;
    }

    const std::size_t Tab = Record.find('\t');
    Remember(Record.substr(Tab + 1, Record.size() - Tab - 2), Record.substr(0, Tab));
    if (Told) return;
    Told = true;
    if (Wrote < 0)
        Say("history: the volume would not keep the line (error {}); this hub remembers it until it ends\n",
            static_cast<int>(box_errno_of(static_cast<int>(Wrote))));
    else
        print("history: the volume kept only part of the line; this hub remembers it whole until it ends\n");
}

void History::Keep(std::string_view Text, std::string_view Context, bool ToFile)
{
    if (Text.empty() || !Usable(Text) || !Usable(Context)) return;
    Catch();
    if (!Lines.empty() && Lines.back().Text == Text && Lines.back().Context == Context) return;
    if (!ToFile) {
        Remember(Text, Context);
        return;
    }
    std::string Record;
    Record.reserve(Context.size() + Text.size() + 2);
    Record.append(Context).append(1, '\t').append(Text).append(1, '\n');
    Write(Record);
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
