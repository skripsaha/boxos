
#include <cassert>
#include <cstdlib>
#include <csignal>
#include <fstream>
#include <string>

#include "box/print.h"
#include "box/system.h"

namespace {

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

}

int main()
{
    const std::string mode = ReadMode();

    if (mode == "raise") {
        print("[exitpaths] raising SIGABRT\n");
        io_flush();
        std::raise(SIGABRT);
        print("[exitpaths] raise returned — default disposition did not kill\n");
        io_flush();
        return 1;
    }

    if (mode == "throw") {
        print("[exitpaths] expected-fatal: the uncaught throw below is the test\n");
        print("[exitpaths] throwing uncaught\n");
        io_flush();
        throw 42;
    }

    if (mode == "quickexit") {
        std::at_quick_exit([] {
            print("[exitpaths] at_quick_exit handler ran\n");
            io_flush();
            _Exit(88);
        });
        print("[exitpaths] quick_exit\n");
        io_flush();
        std::quick_exit(0);
    }

    if (mode == "assert") {
        std::signal(SIGABRT, SIG_IGN);
        print("[exitpaths] asserting with SIGABRT ignored\n");
        io_flush();
        assert(1 == 2);
        print("[exitpaths] assert returned — the process should be gone\n");
        io_flush();
        return 1;
    }

    print("[exitpaths] returning from main\n");
    io_flush();
    return 0;
}