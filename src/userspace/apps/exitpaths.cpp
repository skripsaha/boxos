/*
 * exitpaths — the child half of cxxtest's Phase203.
 *
 * It answers one question that cannot be answered from inside a living
 * process: does TEARDOWN run on the way out? [support.start.term] draws a hard
 * line — exit() runs the __cxa_finalize callbacks and the .fini_array,
 * abort() runs neither — and until Ф41 boxcxx crossed that line on every fatal
 * path, because its Panic ended with exit(134): the status of an abort reached
 * by the machinery of a normal return.
 *
 * The witness is the exit status, which needs no file, no message and no
 * clock. A namespace-scope object's destructor calls _Exit(77), so:
 *
 *     teardown ran     ->  the process dies 77, and nothing else can produce it
 *     teardown skipped ->  the process dies with whatever killed it (134)
 *
 * The mode arrives in a TagFS file called "exitmode", written by the parent
 * just before the spawn. BoxOS has no argv — a process is a cabin, not a
 * command line — and a one-word file is the smallest channel that already
 * exists. An unreadable or absent file means "normal", so a broken fixture
 * fails the control case loudly instead of passing the interesting ones
 * vacuously.
 */

#include <cassert>
#include <csignal>
#include <fstream>
#include <string>

#include "box/print.h"
#include "box/system.h"

namespace {

// The teardown witness. Its destructor runs from __cxa_finalize, which exit()
// calls and abort() does not.
struct TeardownWitness {
    ~TeardownWitness()
    {
        print("[exitpaths] teardown ran\n");
        io_flush();
        _Exit(77);
    }
};

TeardownWitness g_witness;

std::string ReadMode()
{
    std::ifstream in("exitmode");
    if (!in) return "normal";
    std::string mode;
    in >> mode;
    if (mode.empty()) return "normal";
    return mode;
}

} // namespace

int main()
{
    const std::string mode = ReadMode();

    if (mode == "raise") {
        // Default disposition: <csignal> ends the process with 128 + SIGABRT
        // and runs no teardown.
        print("[exitpaths] raising SIGABRT\n");
        io_flush();
        std::raise(SIGABRT);
        print("[exitpaths] raise returned — default disposition did not kill\n");
        io_flush();
        return 1;
    }

    if (mode == "throw") {
        // Uncaught: the personality routine finds no handler, calls
        // std::terminate, whose default handler must reach abort().
        //
        // That path prints "[boxcxx] FATAL: std::terminate() called", which the
        // phase-matrix runner counts as a death — correctly, and for a reason
        // it learned the hard way (see the comment in cxx_phase_matrix.sh: a
        // real crash once read as a slow config because FATAL was not in its
        // pattern). A death that is the POINT of the run has to announce
        // itself, so the runner can subtract exactly this one and keep
        // counting every other.
        print("[exitpaths] expected-fatal: the uncaught throw below is the test\n");
        print("[exitpaths] throwing uncaught\n");
        io_flush();
        throw 42;
    }

    if (mode == "assert") {
        // Two things at once, and neither is reachable any other way from a
        // program that only includes public headers.
        //
        // First: a failed assertion must end the process without teardown.
        // Second, and the reason SIG_IGN is installed: [support.start.term]/9
        // says abort() terminates EVEN IF SIGABRT is ignored or the handler
        // returns. With the default disposition, abort() never gets past its
        // raise() — the raise itself ends the process — so the fallback after
        // it is only exercised when the raise comes back. It comes back here.
        std::signal(SIGABRT, SIG_IGN);
        print("[exitpaths] asserting with SIGABRT ignored\n");
        io_flush();
        assert(1 == 2);
        print("[exitpaths] assert returned — the process should be gone\n");
        io_flush();
        return 1;
    }

    // "normal": fall out of main, exit() runs the witness, status 77.
    print("[exitpaths] returning from main\n");
    io_flush();
    return 0;
}
