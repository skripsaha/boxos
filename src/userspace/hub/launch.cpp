/* launch.cpp — a filled Form started, its end heard, its gift kept. */

#include "launch.h"

#include <optional>
#include <utility>

#include "fill.h"
#include "hub.h"
#include "news.h"
#include "remake.h"
#include "say.h"

namespace hub::launch {

namespace {

// The program started as filled and stood by until it is gone: how it ended,
// said when it did not end well. Nothing when the kernel would not start it,
// and why was said.
std::optional<Ending> Started(Hub &Self, const fill::Filled &What, std::string_view Who)
{
    std::expected<Ending, int> Ran = Self.Started.Run(What.Program, What.Luggage);
    if (!Ran) {
        Say("{}: is there and would not start ({})\n", Who, Reason(Ran.error()));
        return std::nullopt;
    }
    news::Ended(Self.Shown, Who, *Ran);
    return std::move(*Ran);
}

}  // namespace

bool Program(Hub &Self, file_id_t Program, std::string_view Who, std::string_view Line)
{
    if (!remake::WhenChanged(Self, Program, Who)) return false;
    std::optional<fill::Filled> What = fill::FromLine(Self, Program, Who, Line);
    if (!What) return false;
    std::optional<Ending> End = Started(Self, *What, Who);
    if (!End) return false;

    if (What->Screen != 0) Self.Given.Keep({What->Screen});
    else if (What->Souvenir && End->Collected) Self.Given.Keep(std::move(End->Given));
    return End->Seen && End->Code == 0;
}

bool OnThing(Hub &Self, file_id_t Program, std::string_view Who, file_id_t Thing, file_id_t &Given)
{
    Given = 0;
    const std::optional<fill::Filled> What = fill::OnThing(Program, Who, Thing);
    if (!What) return false;
    const std::optional<Ending> End = Started(Self, *What, Who);
    if (!End || !End->Seen || End->Code != 0) return false;
    if (What->Souvenir && End->Collected && !End->Given.empty()) Given = End->Given.front();
    return true;
}

}  // namespace hub::launch
