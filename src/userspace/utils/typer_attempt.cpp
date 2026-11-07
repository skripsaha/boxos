/* typer_attempt.cpp — the judging. A key is right when it is the character the
 * text has at that place, wrong otherwise, and either way the cursor moves on:
 * the mistake stays where it was made, to be seen and taken back.
 *
 * Accuracy is the accuracy of keystrokes, so a mistake taken back still counts
 * against it; speed counts only what stands right at the end, so a mistake left
 * standing costs speed as well. */

#include <algorithm>

#include "typer.h"

namespace {

constexpr std::uint64_t SpeedFloorMs = 1000;

unsigned WordsPerMinute(unsigned Right, std::uint64_t Ms)
{
    if (Ms < SpeedFloorMs) Ms = SpeedFloorMs;
    return static_cast<unsigned>(static_cast<double>(Right) * 12000.0 / static_cast<double>(Ms) + 0.5);
}

}

Attempt::Attempt(std::string Text, unsigned Seconds)
    : Chars(std::move(Text)), Marks(Chars.size(), Mark::Untyped), Seconds(Seconds)
{
}

bool Attempt::Finished(std::uint64_t NowMs) const
{
    if (Ended || Place >= Chars.size()) return true;
    return Began && Timed() && NowMs - StartMs >= LimitMs();
}

bool Attempt::Type(char Key, std::uint64_t NowMs)
{
    if (Finished(NowMs)) return false;
    if (!Began) {
        Began   = true;
        StartMs = NowMs;
    }

    char Wanted = Chars[Place];
    bool Right  = Key == Wanted;
    Marks[Place] = Right ? Mark::Right : Mark::Wrong;
    KeyCount++;
    if (Right) {
        RightStanding++;
    } else {
        WrongKeys++;
        MissCounts[static_cast<unsigned char>(Wanted)]++;
    }
    Strokes.push_back(Stroke{static_cast<std::uint32_t>(NowMs - StartMs), Right});
    Place++;
    return true;
}

void Attempt::Back()
{
    if (Ended || Place == 0) return;
    Place--;
    if (Marks[Place] == Mark::Right) RightStanding--;
    Marks[Place] = Mark::Untyped;
    BackCount++;
}

void Attempt::Append(std::string_view More)
{
    Chars.append(More);
    Marks.resize(Chars.size(), Mark::Untyped);
}

void Attempt::Stop(std::uint64_t NowMs)
{
    if (Ended) return;
    Ended = true;
    EndMs = NowMs;
    if (Began && Timed() && EndMs - StartMs > LimitMs()) EndMs = StartMs + LimitMs();
}

std::uint64_t Attempt::ElapsedMs(std::uint64_t NowMs) const
{
    if (!Began) return 0;
    std::uint64_t End = Ended ? EndMs : NowMs;
    std::uint64_t Taken = End > StartMs ? End - StartMs : 0;
    return Timed() ? std::min(Taken, LimitMs()) : Taken;
}

unsigned Attempt::Wpm(std::uint64_t NowMs) const
{
    return Began ? WordsPerMinute(RightStanding, ElapsedMs(NowMs)) : 0;
}

unsigned Attempt::Accuracy() const
{
    return KeyCount == 0 ? 100 : (KeyCount - WrongKeys) * 100 / KeyCount;
}

std::vector<Missed> Attempt::MostMissed(std::size_t Count) const
{
    std::vector<Missed> All;
    for (unsigned Key = 0; Key < 256; Key++)
        if (MissCounts[Key] != 0) All.push_back(Missed{static_cast<char>(Key), MissCounts[Key]});

    std::sort(All.begin(), All.end(), [](const Missed &A, const Missed &B) {
        return A.Count != B.Count ? A.Count > B.Count : static_cast<unsigned char>(A.Key) < static_cast<unsigned char>(B.Key);
    });
    if (All.size() > Count) All.resize(Count);
    return All;
}

std::vector<unsigned> Attempt::SpeedBars(unsigned SliceSeconds, std::uint64_t ElapsedMs) const
{
    std::uint64_t SliceMs = static_cast<std::uint64_t>(SliceSeconds == 0 ? 1 : SliceSeconds) * 1000;
    std::uint64_t Bars    = (ElapsedMs + SliceMs - 1) / SliceMs;
    if (Bars > 1 && ElapsedMs - (Bars - 1) * SliceMs < SpeedFloorMs) Bars--;
    if (Bars == 0) return {};

    std::vector<unsigned> Right(Bars, 0);
    for (const Stroke &Hit : Strokes) {
        if (!Hit.Right) continue;
        std::uint64_t Index = std::min<std::uint64_t>(Hit.Ms / SliceMs, Bars - 1);
        Right[Index]++;
    }

    std::vector<unsigned> Speeds(Bars, 0);
    for (std::uint64_t Index = 0; Index < Bars; Index++) {
        std::uint64_t Length = Index + 1 < Bars ? SliceMs : ElapsedMs - Index * SliceMs;
        Speeds[Index] = WordsPerMinute(Right[Index], Length);
    }
    return Speeds;
}

unsigned SliceSecondsFor(std::uint64_t ElapsedMs, unsigned MaxBars)
{
    if (MaxBars == 0) MaxBars = 1;
    unsigned Slice = 5;
    while ((ElapsedMs + Slice * 1000ull - 1) / (Slice * 1000ull) > MaxBars) Slice += 5;
    return Slice;
}
