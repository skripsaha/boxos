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

// "  2  notes.txt  (text, draft)": a choice as the person tells it from the others.
void SayChoice(std::size_t Number, file_id_t File)
{
    const std::expected<volume::Record, int> Held = volume::Record::Of(File);
    if (!Held) {
        Say("  {}  file #{}\n", Number, static_cast<unsigned long long>(File));
        return;
    }
    std::string Line = std::format("  {}  {}", Number, Held->Name());
    const std::vector<std::string_view> Tags = tags::Split(Held->Tags());
    for (std::size_t I = 0; I < Tags.size(); I++) Line.append(I == 0 ? "  (" : ", ").append(Tags[I]);
    if (!Tags.empty()) Line += ')';
    Say(Line + "\n");
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
    if (Length < 0) return Answer::NoOne;
    Text.assign(text::Trimmed(std::string_view(Read, static_cast<std::size_t>(Length))));
    free(Read);
    return Text.empty() ? Answer::Empty : Answer::Given;
}

Answer Which(std::string_view Word, std::span<const file_id_t> Ids, file_id_t &Chosen)
{
    Say("{} could be any of these:\n", Word);
    for (std::size_t I = 0; I < Ids.size(); I++) SayChoice(I + 1, Ids[I]);

    for (;;) {
        std::string  Typed;
        const Answer Asked = Line("which one? (a number) ", Typed);
        if (Asked != Answer::Given) return Asked;
        const std::size_t Chose = Number(Typed, Ids.size());
        if (Chose != 0) {
            Chosen = Ids[Chose - 1];
            return Answer::Given;
        }
        Say("that is not one of the numbers above, 1 to {}\n", Ids.size());
    }
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
    Say("{}: {}, so nothing was started\n", Who,
        Ended == Answer::NoOne ? "there is no one to ask" : "no answer given");
}

}  // namespace hub::asking
