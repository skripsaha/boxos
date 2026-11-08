#include "ring.h"

#include <cstdio>
#include <cstring>

#include "box/debug.h"
#include "box/screen.h"
#include "boxos_utf8.h"

bool RingFollower::CatchUp(ConsoleModel &Model, std::uint64_t End) noexcept
{
    bool Drew = false;
    char Bytes[LOG_SAY_MAX];
    std::uint8_t Tones[LOG_SAY_MAX];

    while (!Unreadable && Position < End) {
        const std::uint64_t Left = End - Position;
        const std::uint32_t Want = Left < LOG_SAY_MAX ? static_cast<std::uint32_t>(Left)
                                                      : LOG_SAY_MAX;
        std::uint64_t Oldest = 0, Written = 0;
        const int Copied = log_read_tones(Position, Bytes, Tones, Want, &Oldest, &Written);
        if (Copied < 0) {
            // Not a passing refusal: a read the kernel turns down is turned
            // down the same way the next time, and reading again on every
            // bell would keep this program awake for nothing.
            Unreadable = true;
            kdbg_print("[display] the log ring would not be read (%d); the "
                       "kernel's lines stop reaching the screen", Copied);
            return Drew;
        }

        if (Oldest > Position) {
            // The writers lapped this reader: what lay between is gone from
            // the ring. Said on the screen rather than splicing two ends of
            // the account together as if they met.
            char Note[120];
            const int Length = std::snprintf(
                Note, sizeof(Note),
                "\n[display] %llu bytes of the machine's log went by before they "
                "could be shown\n",
                static_cast<unsigned long long>(Oldest - Position));
            std::uint8_t NoteTones[sizeof(Note)];
            std::memset(NoteTones, LOG_TONE_WARNING, sizeof(NoteTones));
            if (Length > 0)
                Model.WriteKernel(Note, NoteTones,
                                  static_cast<std::size_t>(Length) < sizeof(Note)
                                      ? static_cast<std::size_t>(Length)
                                      : sizeof(Note) - 1u);
            Position = Oldest;
            Drew = true;
            continue;
        }
        if (Copied == 0) break;
        if (Position + static_cast<std::uint64_t>(Copied) > Seen)
            Seen = Position + static_cast<std::uint64_t>(Copied);

        // A kprintf may be half way through a character at the live end; it
        // is drawn whole once the bell says more was said.
        const std::size_t Whole = BoxUtf8Whole(Bytes, static_cast<std::size_t>(Copied));
        if (Whole == 0) break;

        // What programs said is shown, and of the kernel's own lines those
        // that hold an error (boxos_log_tone.h); the rest is the log's. A
        // kernel line is judged whole, so an error keeps the [TAG] it began
        // with: it ends at its newline, at a program's word, or at End, where
        // this display's own word follows. One still being said at the live
        // end waits unread for the bell; one longer than a whole read is
        // judged as it stands.
        std::size_t Done = 0;
        while (Done < Whole) {
            std::size_t Stop = Done + 1;
            if (Tones[Done] == LOG_TONE_SAID) {
                while (Stop < Whole && Tones[Stop] == LOG_TONE_SAID) Stop++;
                Model.WriteKernel(Bytes + Done, Tones + Done, Stop - Done);
                Drew = true;
                Done = Stop;
                continue;
            }
            bool Error = false;
            Stop = Done;
            while (Stop < Whole && Tones[Stop] != LOG_TONE_SAID) {
                Error |= Tones[Stop] == LOG_TONE_ERROR;
                if (Bytes[Stop++] == '\n') break;
            }
            const bool Ended = Bytes[Stop - 1] == '\n' || Stop < Whole || Position + Stop == End ||
                               (Done == 0 && static_cast<std::uint32_t>(Copied) == Want);
            if (!Ended) break;
            if (Error) {
                Model.WriteKernel(Bytes + Done, Tones + Done, Stop - Done);
                Drew = true;
            }
            Done = Stop;
        }
        Position += Done;
        if (Done < Whole) break;
    }
    return Drew;
}

bool RingFollower::CatchUp(ConsoleModel &Model) noexcept
{
    if (!Due) return false;
    Due = false;
    return CatchUp(Model, UINT64_MAX);
}

bool RingFollower::Say(const char *Bytes, std::uint32_t Count, std::uint64_t &Start,
                       std::uint64_t &End) noexcept
{
    const int Code = log_say(Bytes, Count, &Start, &End);
    if (Code == 0) return true;
    static bool Said = false;
    if (!Said) {
        Said = true;
        kdbg_print("[display] the log ring would not take the programs' text (%d); "
                   "the screen shows it, the serial line and logsave will not", Code);
    }
    return false;
}

void RingFollower::Passed(std::uint64_t End) noexcept
{
    if (End > Position) Position = End;
    if (End > Seen) Seen = End;
}

bool RingFollower::HangBell() noexcept
{
    if (Unreadable) return false;
    bool News = false;
    const int Code = log_bell(Seen, &News);
    if (Code == 0) {
        Due |= News;
        return News;
    }
    static bool Said = false;
    if (!Said) {
        Said = true;
        kdbg_print("[display] the log ring's bell could not be hung (%d); the "
                   "kernel's lines reach the screen only with other news", Code);
    }
    return false;
}
