/* said — what this machine has said, without loading anything to ask.
 *
 * `logsave` and `lastsaid` do this too, and both are still here — as files on
 * the volume, which is exactly the assumption this command drops: on a board
 * whose volume mounts and whose files cannot be found, every command comes
 * back unknown, including the two that would have written the log. This one
 * is the hub's own: it needs no lookup, no start and no volume to run; it
 * needs the volume only when asked for a file, and when the volume refuses it
 * says so and prints instead.
 *
 *   said                  what this boot has said, to the screen
 *   said NAME             the same, into NAME on the volume
 *   said before           what the boot before this one said
 *   said before NAME      the same, into NAME
 *
 * "before" reads the carry-over window (klib_logring.h). Measured on the
 * board: that window is EMPTY on an i5-9400F/B365, because the firmware
 * rewrites memory on every start, warm reset included. It survives in QEMU and
 * on firmware that does not retrain; where it does not, this says so in one
 * sentence instead of handing back an empty file. */

#include <array>
#include <string>

#include "box/current.h"
#include "box/error.h"
#include "box/print.h"

#include "builtins.h"
#include "say.h"
#include "words.h"

namespace hub {

namespace {

// One page: what the kernel hands over a call, whatever is asked.
constexpr std::size_t Pail = 4096;

// The log, until it is drained, onto the screen or into Out; how many bytes,
// and false when it stopped being readable or Out stopped taking bytes.
bool Pour(Current *Log, Current *Out, const std::string &Name, const char *What, std::uint64_t &Poured)
{
    std::array<char, Pail> Bytes;
    for (;;) {
        int Read = current_read(Log, Bytes.data(), Bytes.size());
        if (Read == CURRENT_CLOSED) return true;
        if (Read < 0) {
            Say("\n{} stopped being readable after {} byte(s) ({})\n", What, Poured, Reason(Read));
            return false;
        }
        if (!Out) {
            // As it is: the log carries its own line ends, and adding any
            // would change what the machine said.
            print_bytes(Bytes.data(), static_cast<std::size_t>(Read));
            Poured += static_cast<std::uint64_t>(Read);
            continue;
        }
        for (int At = 0; At < Read;) {
            int Wrote = current_write(Out, Bytes.data() + At, static_cast<std::size_t>(Read - At));
            if (Wrote <= 0) {
                Say("{} stopped taking bytes after {}{}\n", Name, Poured,
                    Wrote < 0 ? " (" + std::string(Reason(Wrote)) + ")" : std::string());
                return false;
            }
            At += Wrote;
            Poured += static_cast<std::uint64_t>(Wrote);
        }
    }
}

}  // namespace

bool Said(Hub &, const Words &Typed)
{
    const bool        Before = Typed.Count() > 1 && Typed[1] == "before";
    const char       *Tag    = Before ? "log:previous" : "log:file";
    const char       *What   = Before ? "what the run before this one said" : "what this boot has said";
    const std::size_t At     = Before ? 2 : 1;
    const std::string Name   = Typed.Count() > At ? std::string(Typed[At]) : std::string();

    error_t  Why = OK;
    Current *Log = current_open_ex(Tag, CURRENT_READ, 0, 0, &Why);
    if (!Log) {
        if (Why == ERR_UNSUPPORTED)
            Say("This kernel does not keep what it says. Build it with PRINTTOFILE=on and it will be here.\n");
        else Say("{} could not be opened ({})\n", What, Reason(Why));
        return false;
    }

    // Named, and then it prints anyway: a volume that will not take the file
    // is the commonest reason anybody types this at all, and refusing to show
    // the log because it cannot also store it would fail exactly then.
    Current *Out     = nullptr;
    error_t  Refused = OK;
    if (!Name.empty()) {
        Out = current_open_ex(("file:" + Name).c_str(), CURRENT_WRITE, 0, CURRENT_CREATE | CURRENT_FRESH, &Why);
        if (!Out) {
            Refused = Why;
            Say("{} could not be written ({}) — printing it instead\n", Name, Reason(Why));
        }
    }

    std::uint64_t Poured = 0;
    const bool    Whole  = Pour(Log, Out, Name, What, Poured);
    // Asked BEFORE the log is let go: a gap that reads as continuous is worse
    // than one that is announced, because it will be believed.
    const std::uint64_t Lost = current_lost(Log);
    if (Out) current_release(Out);
    current_release(Log);

    if (Lost)
        Say("\n{} byte(s) had already been overwritten in the ring before this could read them.\n", Lost);
    if (!Whole) return false;
    if (Poured == 0) {
        // Zero is an answer, and for the run before it has three ordinary
        // causes worth naming, so nobody hunts a fourth.
        Say("Nothing to read: {} is empty. For a previous run that means either this is the first one, or power "
            "was removed rather than the machine reset, or this firmware rewrites memory on every start.\n",
            What);
        return true;
    }
    if (Out) {
        Say("{} byte(s) of {} written to {}\n", Poured, What, Name);
        return true;
    }
    Say("\n-- {} byte(s): {} --\n", Poured, What);
    // Said again at the bottom: the refusal printed above is a thousand lines
    // up by now, scrolled off the glass before anyone with a camera reads it.
    if (Refused != OK) Say("-- and {} was NOT written: {} --\n", Name, Reason(Refused));
    return true;
}

}  // namespace hub
