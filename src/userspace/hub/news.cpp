/* news.cpp — the hub's sentences about its programs. */

#include "news.h"

#include <string>

#include "box/system.h"
#include "proc_exit.h"

#include "say.h"

namespace hub::news {

void Ended(const Look &Shown, std::string_view Who, const Ending &End)
{
    if (!Shown.Ended || !End.Seen || End.Code == 0) return;
    const std::string Where = End.Fell && !End.In.empty() ? " in " + End.In : std::string();
    if (End.Code > 0) SayIn(Shown.NewsColor, "{} ended: {}\n", Who, static_cast<int>(End.Code));
    else if (End.Code == PROC_EXIT_KILLED) SayIn(Shown.NewsColor, "{} was ended by another program\n", Who);
    else if (End.Code == PROC_EXIT_STACK_OVERFLOW) SayIn(Shown.NewsColor, "{} stopped: its stack ran out{}\n", Who, Where);
    else if (End.Fell) SayIn(Shown.NewsColor, "{} stopped: {}{}\n", Who, process_fault_said(End.Vector), Where);
    else SayIn(Shown.NewsColor, "{} stopped: the system stopped it\n", Who);
}

void Took(const Look &Shown, std::chrono::steady_clock::duration Spent)
{
    if (Shown.LongCommand <= 0 || Spent < std::chrono::seconds(Shown.LongCommand)) return;
    const auto Seconds = std::chrono::duration_cast<std::chrono::seconds>(Spent).count();
    if (Seconds < 60) SayIn(Shown.NewsColor, "took {} s\n", Seconds);
    else if (Seconds < 3600) SayIn(Shown.NewsColor, "took {} min {} s\n", Seconds / 60, Seconds % 60);
    else SayIn(Shown.NewsColor, "took {} h {} min\n", Seconds / 3600, Seconds % 3600 / 60);
}

}  // namespace hub::news
