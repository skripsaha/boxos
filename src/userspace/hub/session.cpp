/* session.cpp — the prompt, the line typed at it, the line kept and run. */

#include "session.h"

#include <chrono>
#include <optional>
#include <string>
#include <utility>

#include "box/memory.h"
#include "box/print.h"
#include "box/cxx/settings.h"

#include "context.h"
#include "hub.h"
#include "line.h"
#include "news.h"
#include "prompt.h"
#include "say.h"
#include "whisper.h"

namespace hub {

namespace {

// The volume would not keep a line in the history: said once, after what the
// line's command said, so its words are never cut in two.
void SayUnkept(const Hub &Self, const History::Unkept &Why)
{
    if (Why.Rc < 0)
        SayIn(Self.Shown.RefusalColor,
              "history: the volume would not keep the line ({}); this hub remembers it until it ends\n",
              Reason(Why.Rc));
    else
        SayIn(Self.Shown.RefusalColor,
              "history: the volume kept only part of the line; this hub remembers it whole until it ends\n");
}

// The line typed at the prompt: kept, run, how long it took said when it took
// long, and what the history could not keep said last. Right after a command
// was not found, `yes` stands for the name guessed for it and the rest of that
// line: that is what is said, kept and run; `yes` alone of several names is
// asked which, and offered again.
void Typed(Hub &Self, std::string_view Line, std::string_view Context)
{
    const std::optional<nearest::Guess> Guessed = std::exchange(Self.Guessed, std::nullopt);
    const nearest::Answer               Answer  = Guessed ? nearest::Answered(*Guessed, Line) : nearest::Answer{};
    if (Answer.Is == nearest::Answer::Kind::Which) {
        SayIn(Self.Shown.NewsColor, "which one? {}\n", nearest::Said(*Guessed, true));
        Self.Guessed = Guessed;
        return;
    }
    const bool             Meant = Answer.Is == nearest::Answer::Kind::Meant;
    const std::string_view Run   = Meant ? std::string_view(Answer.Line) : Line;
    if (Meant) Say("{}\n", Run);
    const std::optional<History::Unkept> Unkept = Self.Lines.Keep(Run, Context, Self.Shown.History);
    const auto                           Began  = std::chrono::steady_clock::now();
    RunLine(Self, Run);
    news::Took(Self.Shown, std::chrono::steady_clock::now() - Began);
    if (Unkept) SayUnkept(Self, *Unkept);
}

}  // namespace

bool Converse(Hub &Self)
{
    box::settings::watch Watch;
    while (Self.Going) {
        // While one from behind is on the screen the prompt waits: the screen
        // is that program's, and the hub's again once it goes.
        Self.Started.AwaitScreen();
        if (Watch.changed()) {
            Self.Shown = ReadLook();
            Self.Started.Show(Self.Shown);
        }
        const std::string Context = context::Whole().value_or(std::string());
        ShowPrompt(Self, Context);

        // The line grows as it is typed: no ceiling but memory. The Whisper
        // sees the volume as it is when this line begins.
        char *Input  = nullptr;
        int   Length = 0;
        {
            Whisper Hints(Self, Context);
            Length = readline_whispered(&Input, Hints.Hook());
        }
        // Asked to finish (Ctrl+Alt+End over it, said by its keeper): the hub
        // ends as it ends at `exit`.
        if (Length == READLINE_ASKED_TO_END) return true;
        if (Length < 0) return false;
        const HeldHeap<char> Held(Input);
        if (Length > 0) Typed(Self, std::string_view(Input, static_cast<std::size_t>(Length)), Context);
    }
    return true;
}

}  // namespace hub
