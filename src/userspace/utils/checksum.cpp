/* checksum — the sum of a file, of the kind the person names first.
 *
 *   checksum sha256 a.txt                    9f86d0...  a.txt
 *   checksum crc32 a.txt, b.txt              one line for each file
 *   checksum sha256 a.txt 9f86d0...          says whether a.txt has that sum
 *   checksum sha256 them -> sums.txt         the lines as text into sums.txt
 *
 * A line is the sum in lowercase hex, two blanks, the file's name: the layout
 * every sum tool prints and reads. The kinds are crc32, md5, sha1, sha256,
 * sha384, sha512, sha3-256, sha3-512 and blake2b.
 *
 * What the program gives back is what it vouches for: the files it summed,
 * so `them` can go on to the next command; and when a sum was given to check,
 * the file only if it had that sum, so what is tagged after a check is
 * only ever a file that matched. A file that could not be read is never given
 * back. The program ends with 0, or with 1 if a sum did not match, a file could
 * not be read, or the line could not be made sense of.
 */

#include <cstdint>
#include <format>
#include <memory>
#include <new>
#include <string>
#include <span>
#include <string_view>
#include <vector>

#include "box/cxx/error.h"
#include "box/color.h"
#include "box/file.h"
#include "box/form.h"
#include "box/print.h"
#include "box/time.h"
#include "box/vga.h"

#include "checksum.h"

BOX_FORM("takes name \"the kind of sum: crc32, md5, sha1, sha256, sha384, sha512, sha3-256, sha3-512, blake2b\"\n"
         "takes things \"the files\"\n"
         "takes optional words \"the sum a file should have, to check it\"\n"
         "gives things\n");

namespace {

constexpr std::size_t   ChunkBytes     = 256 * 1024;
constexpr std::uint64_t SlowAfterMs    = 1000;
constexpr std::uint64_t RepaintEveryMs = 250;
constexpr Color         NameColor      = COLOR_SLATE;

constexpr std::uint32_t KindField  = 0;
constexpr std::uint32_t FilesField = 1;
constexpr std::uint32_t SumField   = 2;

/* What only the person should see: refusals, progress, notes. */
void Say(std::string_view Text)
{
    std::string Line(Text);
    Line += '\n';
    print_to_screen(Line.c_str());
}

void Put(std::string_view Text, Color Fg = COLOR_DEFAULT)
{
    set_color(Fg);
    print_bytes(Text.data(), Text.size());
    set_color(COLOR_DEFAULT);
}

std::string Why(std::int64_t Answer)
{
    return std::string(box::error(static_cast<::error_t>(-Answer)).message());
}

std::string Grouped(std::uint64_t Number)
{
    std::string Digits = std::to_string(Number);
    std::string Result;
    for (std::size_t I = 0; I < Digits.size(); I++) {
        if (I > 0 && (Digits.size() - I) % 3 == 0) Result += ' ';
        Result += Digits[I];
    }
    return Result;
}

/* Slow work shows how far it is, on the screen only, one line rewritten where
 * it stands and as wide as the screen so no old text shows through. */
class Progress {
public:
    Progress(std::string_view Kind, std::string_view Label, std::uint64_t Total)
        : Heading(std::format("{} {}", Kind, Label)), Total(Total), Began(Now())
    {
        vga_dimensions_t Dimensions;
        if (vga_getdimensions(&Dimensions) == 0 && Dimensions.cols > 8) Width = Dimensions.cols - 1u;
    }

    void Update(std::uint64_t Done)
    {
        std::uint64_t Moment = Now();
        if (Moment - Began < SlowAfterMs) return;
        if (Shown && Moment - Last < RepaintEveryMs) return;
        Last  = Moment;
        Shown = true;
        std::uint64_t Percent = Total == 0 ? 100 : Done * 100 / Total;
        Paint(std::format("{}  {}%  {} of {} bytes", Heading, Percent, Grouped(Done), Grouped(Total)));
    }

    void Clear()
    {
        if (!Shown) return;
        Paint("");
        print_to_screen("\r");
        Shown = false;
    }

private:
    static std::uint64_t Now()
    {
        std::uint64_t Milliseconds = 0;
        time_uptime_ms(&Milliseconds);
        return Milliseconds;
    }

    void Paint(std::string Text)
    {
        Text.resize(Width, ' ');
        Text.insert(Text.begin(), '\r');
        print_to_screen(Text.c_str());
    }

    std::string   Heading;
    std::uint64_t Total;
    std::uint64_t Began;
    std::uint64_t Last  = 0;
    std::size_t   Width = 79;
    bool          Shown = false;
};

struct Summed {
    std::string Label;
    std::string Sum;
};

/* The sum of one file, a chunk at a time. Fails with the reason said, and
 * Summed.Sum left empty. */
bool SumFile(file_id_t Id, const checksum::Kind &Kind, std::uint8_t *Chunk, Summed &Done)
{
    FileMetadata Metadata;
    if (FileMetadataRead(Id, &Metadata) != 0) {
        Say("checksum: could not look at a file; it may have been erased");
        return false;
    }
    Done.Label            = Metadata.Filename ? Metadata.Filename : "a file with no name";
    std::uint64_t Size    = Metadata.Size;
    FileMetadataRelease(&Metadata);

    std::unique_ptr<checksum::Hasher> Hasher = Kind.Make();
    Progress                          Shown(Kind.Name, Done.Label, Size);
    std::uint64_t                     At = 0;
    while (At < Size) {
        std::uint64_t Want = Size - At > ChunkBytes ? ChunkBytes : Size - At;
        std::int64_t  Got  = fread(Id, At, Chunk, static_cast<std::size_t>(Want));
        if (Got <= 0) {
            Shown.Clear();
            if (Got < 0) Say(std::format("checksum: {} would not be read at byte {}: {}", Done.Label, Grouped(At), Why(Got)));
            else         Say(std::format("checksum: {} was cut short while it was read; it ends at byte {}", Done.Label, Grouped(At)));
            return false;
        }
        Hasher->Add(Chunk, static_cast<std::size_t>(Got));
        At += static_cast<std::uint64_t>(Got);
        Shown.Update(At);
    }
    Shown.Clear();
    Done.Sum = Hasher->Finish();
    return true;
}

void ShowLine(const Summed &Done)
{
    Put(Done.Sum);
    Put("  ");
    Put(Done.Label, NameColor);
    Put("\n");
}

/* The sum a file should have, as lowercase hex of the kind's length, or the
 * reason it cannot be one. */
std::string ExpectedSum(std::string_view Typed, const checksum::Kind &Kind, std::string &Sum)
{
    for (char Letter : Typed) {
        bool Digit = Letter >= '0' && Letter <= '9';
        bool Upper = Letter >= 'A' && Letter <= 'F';
        bool Lower = Letter >= 'a' && Letter <= 'f';
        if (!Digit && !Upper && !Lower)
            return std::format("'{}' is not a sum: a sum is written with the digits 0 to 9 and the letters a to f. "
                               "The kind comes first, then the files, then the sum", Typed);
        Sum += Upper ? char(Letter - 'A' + 'a') : Letter;
    }
    if (Sum.size() != Kind.HexLength)
        return std::format("a {} sum is {} digits long, and the one given is {}", Kind.Name, Kind.HexLength, Sum.size());
    return {};
}

/* True when the file has the sum it should. */
bool ShowVerdict(const Summed &Done, const checksum::Kind &Kind, const std::string &Expected)
{
    if (Done.Sum == Expected) {
        Put(std::format("{} matches its {}\n", Done.Label, Kind.Name), COLOR_GREEN);
        return true;
    }
    Put(std::format("{} does NOT match: it is {}, not {}\n", Done.Label, Done.Sum, Expected), COLOR_RED);
    return false;
}

void SayWeak(std::string_view Name)
{
    Say(std::string(Name) + " still shows a file that changed by accident, but anyone can make two different files "
        "with the same " + std::string(Name) + " sum, so it is no proof against someone who means to deceive.");
}

struct Request {
    const checksum::Kind *Kind = nullptr;
    std::span<const file_id_t> Files;
    std::string               Expected;
};

/* The kind named, or why none was: the kinds that exist, and the nearest name
 * when one is close. */
const checksum::Kind *KindNamed(std::string_view Typed)
{
    if (const checksum::Kind *Found = checksum::FindKind(Typed)) return Found;
    std::string Refusal = std::format("checksum: '{}' is not a kind of sum. The kinds are {}.", Typed, checksum::KindNames());
    if (const checksum::Kind *Near = checksum::NearestKind(Typed)) Refusal += std::format(" Did you mean {}?", Near->Name);
    Say(Refusal);
    return nullptr;
}

bool ReadRequest(Request &Asked)
{
    const char *const *Words;
    std::size_t        WordCount;
    const file_id_t   *Files;
    std::size_t        FileCount;
    if (form_words(KindField, &Words, &WordCount) != 0 || WordCount == 0) return false;
    Asked.Kind = KindNamed(Words[0]);
    if (!Asked.Kind) return false;
    if (form_things(FilesField, &Files, &FileCount) != 0 || FileCount == 0) return false;
    Asked.Files = {Files, FileCount};

    if (form_words(SumField, &Words, &WordCount) != 0 || WordCount == 0) return true;
    if (WordCount > 1) {
        Say(std::format("checksum: {} words follow the files, and only one sum can be checked: the sum a file should have", WordCount));
        return false;
    }
    if (FileCount != 1) {
        Say(std::format("checksum: a sum to check is for one file, and {} files were given", FileCount));
        return false;
    }
    std::string Refusal = ExpectedSum(Words[0], *Asked.Kind, Asked.Expected);
    if (!Refusal.empty()) {
        Say("checksum: " + Refusal);
        return false;
    }
    return true;
}

int Run()
{
    Request Asked;
    if (!ReadRequest(Asked)) return 1;

    std::unique_ptr<std::uint8_t[]> Chunk(new std::uint8_t[ChunkBytes]);
    std::vector<file_id_t>          Vouched;
    std::size_t                     Computed = 0;
    bool                            AllWell  = true;
    for (file_id_t Id : Asked.Files) {
        Summed Done;
        if (!SumFile(Id, *Asked.Kind, Chunk.get(), Done)) {
            AllWell = false;
            continue;
        }
        Computed++;
        if (Asked.Expected.empty()) {
            ShowLine(Done);
            Vouched.push_back(Id);
        } else if (ShowVerdict(Done, *Asked.Kind, Asked.Expected)) {
            Vouched.push_back(Id);
        } else {
            AllWell = false;
        }
    }
    if (Asked.Kind->Weak && Computed != 0) SayWeak(Asked.Kind->Name);
    if (!Vouched.empty()) souvenir_things(Vouched.data(), Vouched.size());
    return AllWell ? 0 : 1;
}

}

int main()
{
    try {
        return Run();
    } catch (const std::bad_alloc &) {
        Say("checksum: there is no memory for this");
        return 1;
    }
}
