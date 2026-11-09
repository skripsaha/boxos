/* asking.cpp — a question, and the line that answers it. */

#include "asking.h"

#include "box/memory.h"
#include "box/print.h"

#include "say.h"
#include "tags.h"
#include "text.h"
#include "volume.h"

namespace hub::asking {

namespace {

// "notes.txt  (text, draft)": a file as the person tells it from the others.
std::string FileChoice(file_id_t File)
{
    const std::expected<volume::Record, int> Held = volume::Record::Of(File);
    if (!Held) return std::format("file #{}", static_cast<unsigned long long>(File));
    std::string                         Line(Held->Name());
    const std::vector<std::string_view> Tags = tags::Split(Held->Tags());
    for (std::size_t I = 0; I < Tags.size(); I++) Line.append(I == 0 ? "  (" : ", ").append(Tags[I]);
    if (!Tags.empty()) Line += ')';
    return Line;
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
    Say(Prompt);
    io_flush();

    char     *Read   = nullptr;
    const int Length = readline_grow(&Read);
    if (Length == READLINE_ASKED_TO_END) return Answer::Ended;
    if (Length < 0) return Answer::NoOne;
    Text.assign(text::Trimmed(std::string_view(Read, static_cast<std::size_t>(Length))));
    free(Read);
    return Text.empty() ? Answer::Empty : Answer::Given;
}

Answer Choose(std::string_view Heading, std::span<const std::string> Choices, std::size_t &Chosen)
{
    Say("{}\n", Heading);
    for (std::size_t I = 0; I < Choices.size(); I++) Say("  {}  {}\n", I + 1, Choices[I]);

    for (;;) {
        std::string  Typed;
        const Answer Asked = Line("which one? (a number) ", Typed);
        if (Asked != Answer::Given) return Asked;
        const std::size_t Chose = Number(Typed, Choices.size());
        if (Chose != 0) {
            Chosen = Chose - 1;
            return Answer::Given;
        }
        Say("that is not one of the numbers above, 1 to {}\n", Choices.size());
    }
}

Answer Which(std::string_view Word, std::span<const file_id_t> Ids, file_id_t &Chosen)
{
    std::vector<std::string> Choices;
    Choices.reserve(Ids.size());
    for (file_id_t File : Ids) Choices.push_back(FileChoice(File));
    std::size_t  Index = 0;
    const Answer Asked = Choose(std::format("{} could be any of these:", Word), Choices, Index);
    if (Asked == Answer::Given) Chosen = Ids[Index];
    return Asked;
}

Answer YesNo(std::string_view Question, bool &Yes)
{
    const std::string Prompt = std::string(Question) + " (yes or no) ";
    for (;;) {
        std::string  Typed;
        const Answer Asked = Line(Prompt, Typed);
        if (Asked != Answer::Given) return Asked;
        const bool SaidYes = text::Says(Typed, "yes") || text::Says(Typed, "y");
        const bool SaidNo  = text::Says(Typed, "no") || text::Says(Typed, "n");
        if (SaidYes || SaidNo) {
            Yes = SaidYes;
            return Answer::Given;
        }
        Say("please answer yes or no. ");
    }
}

Answer Sure(std::size_t Count, bool &Yes)
{
    Say("all is the whole volume here: {} {}. ", Count, Count == 1 ? "file" : "files");
    return YesNo("sure?", Yes);
}

void SayCancelled(std::string_view Who, Answer Ended)
{
    if (Ended == Answer::Ended) return;
    Say("{}: {}, so nothing was started\n", Who,
        Ended == Answer::NoOne ? "there is no one to ask" : "no answer given");
}

}  // namespace hub::asking
