/* hexer — a file's bytes on the glass, changed in place and put back.
 *
 *   hexer FILE                    the file on the whole screen
 *   hexer FILE from N for N       only the part from N, N bytes (decimal or 0x)
 *   hexer FILE -> NAME            the part as text into NAME, as an arrow puts
 *                                 any program's text
 *
 *   arrows, PgUp PgDn, Home End   move;  TAB  the hex column or the text one
 *   0-9 a-f, or a character       the byte under the cursor becomes it
 *   ^S  ^Q                        save, leave
 *   ESC then a word               go, find, save, quit, quit!, help
 *
 * A change replaces a byte; nothing is put in or taken out, so the file keeps
 * its size and every offset keeps its meaning. The changes stay here until a
 * save writes them, side by side as one write, and anchors the file. Nothing
 * may print between the first paint and the screen given back: printed text
 * would land on top of the frame. This file decides; the bytes are
 * hexer_bytes.cpp, the glass hexer_paint.cpp, the text an arrow takes
 * hexer_list.cpp.
 */

#include <format>
#include <new>
#include <string>
#include <string_view>
#include <vector>

#include "box/file.h"
#include "box/form.h"
#include "box/print.h"

#include "file_heading.h"
#include "hexer.h"

BOX_FORM("takes thing \"the file to look into\"\n"
         "takes optional words \"from where and how many bytes\"\n"
         "gives things\n");

namespace {

/* Words a person reads before the screen is taken or after it is given back:
 * to the person, never into the file an arrow names. */
void Say(std::string_view Text)
{
    std::string Line(Text);
    Line += '\n';
    print_to_screen(Line.c_str());
}

/* A number as a person writes one: decimal, or hex after 0x. */
bool ReadNumber(std::string_view Word, std::uint64_t &Out)
{
    unsigned Base = 10;
    if (Word.size() > 2 && Word[0] == '0' && (Word[1] == 'x' || Word[1] == 'X')) {
        Base = 16;
        Word.remove_prefix(2);
    }
    if (Word.empty()) return false;

    std::uint64_t Value = 0;
    for (char Ch : Word) {
        unsigned Digit;
        if (Ch >= '0' && Ch <= '9')                     Digit = static_cast<unsigned>(Ch - '0');
        else if (Base == 16 && Ch >= 'a' && Ch <= 'f') Digit = static_cast<unsigned>(Ch - 'a' + 10);
        else if (Base == 16 && Ch >= 'A' && Ch <= 'F') Digit = static_cast<unsigned>(Ch - 'A' + 10);
        else return false;
        if (Value > (UINT64_MAX - Digit) / Base) return false;
        Value = Value * Base + Digit;
    }
    Out = Value;
    return true;
}

int HexValue(char Ch)
{
    if (Ch >= '0' && Ch <= '9') return Ch - '0';
    if (Ch >= 'a' && Ch <= 'f') return Ch - 'a' + 10;
    if (Ch >= 'A' && Ch <= 'F') return Ch - 'A' + 10;
    return -1;
}

/* The words after the file: `from N` and `for N`, each once, in any order. */
std::optional<std::string> ReadPart(const char *const *Words, std::size_t Count, std::uint64_t Size,
                                    HexerPart &Part)
{
    std::uint64_t From = 0, Many = UINT64_MAX;
    bool          FromSaid = false, ManySaid = false;

    for (std::size_t I = 0; I < Count; I += 2) {
        std::string_view Word   = Words[I];
        bool             IsFrom = Word == "from";
        if (!IsFrom && Word != "for")
            return std::format("'{}' is no file here, nor from or for: hexer say.box from 0x40 for 64", Word);
        if (IsFrom ? FromSaid : ManySaid) return std::format("{} is said twice", Word);

        std::uint64_t Value = 0;
        if (I + 1 >= Count || !ReadNumber(Words[I + 1], Value))
            return std::format("{} takes a number after it, in decimal or after 0x in hex: {} 0x40", Word, Word);
        if (IsFrom) {
            From     = Value;
            FromSaid = true;
        } else {
            if (Value == 0) return std::string("for 0 is no bytes at all: for takes 1 or more");
            Many     = Value;
            ManySaid = true;
        }
    }

    if (Size == 0 && !FromSaid) {
        Part = HexerPart{};
        return std::nullopt;
    }
    if (From >= Size)
        return Size == 0 ? std::string("the file is empty: there is no byte to start from")
                         : std::format("from {} is past its end: its last byte is {} (0x{:x})", From, Size - 1,
                                       Size - 1);
    Part.Lo = From;
    Part.Hi = Size - From > Many ? From + Many : Size;
    return std::nullopt;
}

std::vector<std::string_view> Split(std::string_view Text)
{
    std::vector<std::string_view> Words;
    std::size_t                   I = 0;
    while (I < Text.size()) {
        while (I < Text.size() && (Text[I] == ' ' || Text[I] == '\t')) I++;
        std::size_t Start = I;
        while (I < Text.size() && Text[I] != ' ' && Text[I] != '\t') I++;
        if (I > Start) Words.push_back(Text.substr(Start, I - Start));
    }
    return Words;
}

/* What `find` is asked for: bytes when every word is a pair of hex digits,
 * the words between quotes as text, and otherwise the text as typed. */
HexerNeedle ReadNeedle(std::string_view What)
{
    if (!What.empty() && What.front() == '"') {
        What.remove_prefix(1);
        if (!What.empty() && What.back() == '"') What.remove_suffix(1);
        return {{What.begin(), What.end()}, false};
    }

    HexerNeedle Wanted{{}, true};
    for (std::string_view Word : Split(What)) {
        if (Word.size() != 2 || HexValue(Word[0]) < 0 || HexValue(Word[1]) < 0)
            return {{What.begin(), What.end()}, false};
        Wanted.Bytes.push_back(static_cast<std::uint8_t>(HexValue(Word[0]) << 4 | HexValue(Word[1])));
    }
    return Wanted;
}

/* The needle as the person would write it again. */
std::string NeedleText(const HexerNeedle &Needle)
{
    if (!Needle.AsBytes)
        return std::format("\"{}\"", std::string_view(reinterpret_cast<const char *>(Needle.Bytes.data()),
                                                     Needle.Bytes.size()));
    std::string Text;
    for (std::uint8_t Byte : Needle.Bytes) Text += std::format("{}{:02x}", Text.empty() ? "" : " ", Byte);
    return Text;
}

}

Hexer::Hexer(file_id_t File, std::string Name, std::string Tags, HexerPart Part)
    : Bytes(File, Part), Name(std::move(Name)), Tags(std::move(Tags)), Part(Part), At(Part.Lo),
      Digits(OffsetDigits(Part.Hi))
{
}

std::optional<std::string> Hexer::TakeScreen()
{
    /* Sixteen bytes a line, or eight where sixteen do not fit. */
    auto LineWidth = [this](std::uint32_t Bytes) { return Digits + 4 * Bytes + 4; };
    std::expected<painter::tui::Stage, painter::tui::StageError> Opened =
        painter::tui::Stage::Open(static_cast<std::int32_t>(LineWidth(8)), 3);
    if (!Opened) return Opened.error().Describe("hexer");
    Screen.emplace(std::move(*Opened));
    Rows = static_cast<std::uint32_t>(Screen->Size().Height);
    Cols = static_cast<std::uint32_t>(Screen->Size().Width);

    if (LineWidth(16) <= Cols)     Width = 16;
    else if (LineWidth(8) <= Cols) Width = 8;
    else {
        GiveScreenBack();
        return std::format("This screen is {} columns wide; a line of hexer needs {}.", Cols, LineWidth(8));
    }
    Top = At - At % Width;
    return std::nullopt;
}

void Hexer::Run()
{
    while (!Leaving) {
        if (!Paint()) break;

        /* No deadline: nothing changes between keys. A refusal means there is
         * nothing left to hear, and a loop on it would be a screen no key
         * could leave. */
        painter::tui::Event Event = Screen->Wait();
        if (Event.Kind != painter::tui::EventKind::Key) break;
        Message.clear();        /* a message stands until the next key */
        OnKey(Event.Key);
    }
    GiveScreenBack();
}

void Hexer::GiveScreenBack()
{
    if (Screen) {
        (void)Screen->Close();
        Screen.reset();
    }
}

std::string Hexer::Summary() const
{
    if (Saves == 0) return std::format("hexer: {} - nothing saved", Name);
    return std::format("hexer: {} - {} byte{} saved", Name, Bytes.SavedBytes(), Bytes.SavedBytes() == 1 ? "" : "s");
}

void Hexer::OnKey(const painter::tui::KeyEvent &Key)
{
    if (CommandOpen) OnCommandKey(Key);
    else             OnEditKey(Key);
}

void Hexer::MoveTo(std::uint64_t Place)
{
    if (Place < Part.Lo)  Place = Part.Lo;
    if (Place >= Part.Hi) Place = Part.Hi - 1;
    At      = Place;
    LowHalf = false;
}

void Hexer::MoveBy(std::int64_t Delta)
{
    if (Delta < 0) MoveTo(static_cast<std::uint64_t>(-Delta) > At - Part.Lo ? Part.Lo : At - static_cast<std::uint64_t>(-Delta));
    else           MoveTo(static_cast<std::uint64_t>(Delta) > Part.Hi - 1 - At ? Part.Hi - 1 : At + static_cast<std::uint64_t>(Delta));
}

/* The byte under the cursor becomes Value; the cursor goes on to the next. */
void Hexer::Type(std::uint8_t Value)
{
    Bytes.Set(At, Value);
    MoveBy(1);
}

void Hexer::OnEditKey(const painter::tui::KeyEvent &Key)
{
    if (Key.IsCtrl()) {
        if (Key.Ascii == 's' || Key.Ascii == 'S')      Save();
        else if (Key.Ascii == 'q' || Key.Ascii == 'Q') Leave();
        return;         /* every other chord is deliberately not a command */
    }

    std::int64_t Page = static_cast<std::int64_t>(TextRows()) * Width;
    if (Key.IsExtended()) {
        switch (Key.Scancode) {
        case painter::tui::KeyCodes::Left:    MoveBy(-1); break;
        case painter::tui::KeyCodes::Right:   MoveBy(1); break;
        case painter::tui::KeyCodes::Up:      MoveBy(-static_cast<std::int64_t>(Width)); break;
        case painter::tui::KeyCodes::Down:    MoveBy(Width); break;
        case painter::tui::KeyCodes::PageUp:  MoveBy(-Page); break;
        case painter::tui::KeyCodes::PageDown: MoveBy(Page); break;
        case painter::tui::KeyCodes::Home:    MoveTo(At - At % Width); break;
        case painter::tui::KeyCodes::End:     MoveTo(At - At % Width + Width - 1); break;
        default: break;
        }
        return;
    }

    char Ch = Key.Ascii;
    if (Ch == painter::tui::KeyCodes::Escape) {
        CommandOpen = true;
        Command.clear();
        return;
    }
    if (Ch == painter::tui::KeyCodes::Tab) {
        InText  = !InText;
        LowHalf = false;
        return;
    }
    if (Ch == painter::tui::KeyCodes::Backspace || Ch == 0x7f) {
        MoveBy(-1);
        return;
    }

    if (InText) {
        if (Ch >= 0x20 && Ch < 0x7f) Type(static_cast<std::uint8_t>(Ch));
        return;
    }
    int Half = HexValue(Ch);
    if (Half < 0) return;

    std::uint8_t Now = 0;
    if (Bytes.Read(At, &Now, 1, nullptr) != 1) {
        Message = std::format("the byte at 0x{:x} would not be read", At);
        return;
    }
    if (!LowHalf) {
        Bytes.Set(At, static_cast<std::uint8_t>(Half << 4 | (Now & 0x0f)));
        LowHalf = true;
    } else {
        Type(static_cast<std::uint8_t>((Now & 0xf0) | Half));
    }
}

void Hexer::OnCommandKey(const painter::tui::KeyEvent &Key)
{
    if (Key.IsCtrl() || Key.IsExtended()) return;

    char Ch = Key.Ascii;
    if (Ch == painter::tui::KeyCodes::Escape) {
        CommandOpen = false;
        return;
    }
    if (Ch == '\n' || Ch == '\r') {
        CommandOpen = false;
        RunCommand();
        return;
    }
    if (Ch == painter::tui::KeyCodes::Backspace || Ch == 0x7f) {
        if (!Command.empty()) Command.pop_back();
        return;
    }
    /* The line is shown on one row of the foot, after ": ". */
    if (Ch >= 0x20 && Ch < 0x7f && Command.size() + 3 < Cols) Command += Ch;
}

void Hexer::RunCommand()
{
    std::string_view Line = Command;
    while (!Line.empty() && Line.front() == ' ') Line.remove_prefix(1);
    std::size_t      Cut  = Line.find(' ');
    std::string_view Verb = Line.substr(0, Cut);
    std::string_view Rest = Cut == std::string_view::npos ? std::string_view() : Line.substr(Cut + 1);
    while (!Rest.empty() && Rest.front() == ' ') Rest.remove_prefix(1);
    while (!Rest.empty() && Rest.back() == ' ') Rest.remove_suffix(1);

    if (Verb.empty())          return;
    if (Verb == "go")          Go(Rest);
    else if (Verb == "find")   Find(Rest);
    else if (Verb == "save")   Save();
    else if (Verb == "quit")   Leave();
    else if (Verb == "quit!")  Leaving = true;
    else if (Verb == "help")
        Message = "go 0x40  go end   find text  find 7f 45  find \"7f\"   save  quit  quit!";
    else
        Message = "hexer: no such word - try  help";
}

void Hexer::Go(std::string_view Where)
{
    std::uint64_t Place = 0;
    if (Where == "end") {
        MoveTo(Part.Hi - 1);
        return;
    }
    if (!ReadNumber(Where, Place)) {
        Message = "go where? say  go 0x400, go 1024 or go end";
        return;
    }
    if (Place < Part.Lo || Place >= Part.Hi) {
        Message = std::format("0x{:x} is outside what is shown: 0x{:x} to 0x{:x}", Place, Part.Lo, Part.Hi - 1);
        return;
    }
    MoveTo(Place);
}

void Hexer::Find(std::string_view What)
{
    if (!What.empty()) LastFind = ReadNeedle(What);
    if (LastFind.Bytes.empty()) {
        Message = "find what? say  find <text>  or  find 7f 45 4c";
        return;
    }

    std::optional<std::uint64_t> Found = Bytes.Find(LastFind.Bytes, At);
    if (!Found) {
        Message = std::format("no {} in what is shown", NeedleText(LastFind));
        return;
    }
    MoveTo(*Found);
    Message = std::format("found {} at 0x{:x}", NeedleText(LastFind), *Found);
}

void Hexer::Save()
{
    if (Bytes.ChangeCount() == 0) {
        Message = "nothing is changed";
        return;
    }
    std::uint64_t Before = Bytes.SavedBytes();
    Message = Bytes.Save();
    if (Bytes.SavedBytes() != Before) Saves++;
}

void Hexer::Leave()
{
    if (Bytes.ChangeCount() != 0) {
        Message = "unsaved changes - ^S to save, or ESC then  quit!";
        return;
    }
    Leaving = true;
}

int main()
{
    try {
        const file_id_t   *Files;
        std::size_t        FileCount;
        const char *const *Words;
        std::size_t        WordCount;
        if (form_things(0, &Files, &FileCount) != 0 || form_words(1, &Words, &WordCount) != 0) return 1;

        file_id_t    File = Files[0];
        FileMetadata Metadata;
        if (FileMetadataRead(File, &Metadata) != 0) {
            Say("hexer: could not look at that file; it may have been erased");
            return 1;
        }
        std::string Name = Metadata.Filename ? Metadata.Filename : "a file with no name";
        std::string Tags = Metadata.Tags ? Metadata.Tags : "";

        HexerPart                  Part;
        std::optional<std::string> Refusal = ReadPart(Words, WordCount, Metadata.Size, Part);
        if (Refusal) {
            FileMetadataRelease(&Metadata);
            Say(*Refusal);
            return 1;
        }

        /* Under an arrow the result is text for the file, not a frame. */
        if (form_screen_file() != 0) {
            bool Listed = HexerList(File, Metadata, Part);
            FileMetadataRelease(&Metadata);
            return Listed ? 0 : 1;
        }
        FileMetadataRelease(&Metadata);

        if (Part.Lo == Part.Hi) {
            Say(std::format("{} is empty: hexer changes the bytes a file has, and it has none", Name));
            souvenir_things(&File, 1);
            return 0;
        }

        Hexer Viewer(File, Name, Tags, Part);
        if (std::optional<std::string> Refused = Viewer.TakeScreen()) {
            Say(*Refused);
            return 1;
        }
        Viewer.Run();
        Say(Viewer.Summary());
        souvenir_things(&File, 1);
        return 0;
    } catch (const std::bad_alloc &) {
        Say("hexer: there is no memory for this file");
        return 1;
    }
}
