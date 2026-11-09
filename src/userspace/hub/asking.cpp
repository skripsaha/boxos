/* asking.cpp — a question, and the line that answers it. */

#include "asking.h"
#include "say.h"

#include "box/file.h"
#include "box/memory.h"
#include "box/print.h"

namespace hub::asking {

namespace {

bool Blank(char C)
{
    return C == ' ' || C == '\t';
}

// The answer is the word, in either case.
bool Says(std::string_view Typed, std::string_view Word)
{
    if (Typed.size() != Word.size()) return false;
    for (std::size_t I = 0; I < Typed.size(); I++) {
        char C = Typed[I];
        if (C >= 'A' && C <= 'Z') C = static_cast<char>(C - 'A' + 'a');
        if (C != Word[I]) return false;
    }
    return true;
}

// The tags a file wears, in brackets, a comma and a blank between.
void SayTags(std::string_view Tags)
{
    print("  (");
    for (bool First = true; !Tags.empty(); First = false) {
        const std::size_t Comma = Tags.find(',');
        if (!First) print(", ");
        const std::string_view Tag = Tags.substr(0, Comma);
        print_bytes(Tag.data(), Tag.size());
        if (Comma == std::string_view::npos) break;
        Tags.remove_prefix(Comma + 1);
    }
    print(")");
}

void SayChoice(std::size_t Number, file_id_t File)
{
    FileMetadata Metadata;
    if (FileMetadataRead(File, &Metadata) != 0) {
        Say("  {}  file #{}\n", Number, static_cast<unsigned long long>(File));
        return;
    }
    Say("  {}  {}", Number, Metadata.Filename ? Metadata.Filename : "");
    if (Metadata.Tags) SayTags(Metadata.Tags);
    print("\n");
    FileMetadataRelease(&Metadata);
}

// The number typed, 1 to Count, or 0 when the answer is not one.
std::size_t Number(std::string_view Typed, std::size_t Count)
{
    std::size_t Value = 0;
    for (char C : Typed) {
        if (C < '0' || C > '9') return 0;
        Value = Value * 10 + static_cast<std::size_t>(C - '0');
        if (Value > Count) return 0;
    }
    return Value;
}

}  // namespace

Answer Line(std::string_view Prompt, std::string &Text)
{
    Text.clear();
    print_bytes(Prompt.data(), Prompt.size());
    io_flush();

    char *Read   = nullptr;
    int   Length = readline_grow(&Read);
    if (Length < 0) return Answer::NoOne;
    std::string_view Typed(Read, static_cast<std::size_t>(Length));
    while (!Typed.empty() && Blank(Typed.front())) Typed.remove_prefix(1);
    while (!Typed.empty() && Blank(Typed.back())) Typed.remove_suffix(1);
    Text.assign(Typed);
    free(Read);
    return Text.empty() ? Answer::Empty : Answer::Given;
}

Answer Which(std::string_view Word, std::span<const file_id_t> Ids, file_id_t &Chosen)
{
    Say("{} could be any of these:\n", Word);
    for (std::size_t I = 0; I < Ids.size(); I++) SayChoice(I + 1, Ids[I]);

    for (;;) {
        std::string Typed;
        Answer      Asked = Line("which one? (a number) ", Typed);
        if (Asked != Answer::Given) return Asked;
        const std::size_t Chose = Number(Typed, Ids.size());
        if (Chose != 0) {
            Chosen = Ids[Chose - 1];
            return Answer::Given;
        }
        Say("that is not one of the numbers above, 1 to {}\n", Ids.size());
    }
}

Answer Sure(std::size_t Count, bool &Yes)
{
    Say("all is the whole volume here: {} {}. ", Count, Count == 1 ? "file" : "files");
    for (;;) {
        std::string Typed;
        Answer      Asked = Line("sure? (yes or no) ", Typed);
        if (Asked != Answer::Given) return Asked;
        const bool SaidYes = Says(Typed, "yes") || Says(Typed, "y");
        const bool SaidNo  = Says(Typed, "no") || Says(Typed, "n");
        if (SaidYes || SaidNo) {
            Yes = SaidYes;
            return Answer::Given;
        }
        print("please answer yes or no. ");
    }
}

void SayCancelled(std::string_view Who, Answer Ended)
{
    Say("{}: {}, so nothing was started\n", Who,
        Ended == Answer::NoOne ? "there is no one to ask" : "no answer given");
}

}  // namespace hub::asking
