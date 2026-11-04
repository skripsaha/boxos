/* pulse — the machine's pulse, at the top of the screen beside the clock.
 *
 * Twice a second: how busy the processor was since the last look, as a
 * figure and as the last few looks drawn small, each the taller and the
 * warmer the busier it was; on a screen wide enough, how much memory is in
 * use too. It wears the clock's colours, light on a dark ground, and stands
 * one column to the left of it, so the two read as one strip.
 *
 *   cpu ___▄█▄_▄▄_  23%  mem 41%
 *
 * It asks the kernel's cumulative counters (system_resources) and compares
 * two looks, as passport and taskcenter do; between looks it sleeps. */

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <format>
#include <string>
#include <thread>
#include <vector>

#include "box/color.h"
#include "box/system.h"
#include "box/vga.h"

namespace {

constexpr auto          Beat        = std::chrono::milliseconds(500);
constexpr std::uint8_t  ClockWidth  = 8;    /* clock.cpp: HH:MM:SS at the right edge */
constexpr std::uint8_t  Gap         = 1;
constexpr std::size_t   HistoryMost = 10;
constexpr std::uint8_t  WideScreen  = 120;  /* from here on memory is shown too */

/* clock.cpp's ground and ink; the labels as quiet as its colons. Every
 * colour keeps its family on a sixteen-colour console: the ground black, the
 * figures white, the gauge light green, yellow and light red. */
constexpr Color Ink    = COLOR_RGB(0xE6, 0xE6, 0xE6);
constexpr Color Ground = COLOR_RGB(0x26, 0x26, 0x26);
constexpr Color Quiet  = COLOR_RGB(0x8C, 0x8C, 0x8C);
constexpr Color Calm   = COLOR_RGB(0x50, 0xD0, 0x60);
constexpr Color Busy   = COLOR_AMBER;
constexpr Color Hot    = COLOR_RGB(0xFF, 0x50, 0x50);

constexpr char LowerHalf = static_cast<char>(0xDC);
constexpr char Full      = static_cast<char>(0xDB);

Color Warmth(int Percent)
{
    return Percent >= 85 ? Hot : Percent >= 50 ? Busy : Calm;
}

/* A look drawn as one cell: a line on the floor, half, full. A quiet
 * machine is a flat line, not nothing: the strip is seen to be alive. */
char Height(int Percent)
{
    if (Percent < 35) return '_';
    if (Percent < 70) return LowerHalf;
    return Full;
}

/* The strip, cell by cell, laid right to left from the clock. */
class Strip {
public:
    void Text(std::string_view Words, Color Fore)
    {
        for (char Ch : Words) Cell(Ch, Fore);
    }

    void Cell(char Ch, Color Fore)
    {
        TextCell One{};
        One.ch = Ch;
        One.fg = Fore;
        One.bg = Ground;
        Cells.push_back(One);
    }

    /* On row 0, ending one column short of the clock. */
    void Paint(const vga_dimensions_t &Glass) const
    {
        if (Glass.rows == 0) return;
        int Width = static_cast<int>(Cells.size());
        int Right = static_cast<int>(Glass.cols) - ClockWidth - Gap;
        if (Width == 0 || Right - Width < 0) return;
        (void)vga_paint(0, static_cast<std::uint8_t>(Right - Width), 1, static_cast<std::uint8_t>(Width), Cells.data());
    }

    std::vector<TextCell> Cells;
};

class Pulse {
public:
    /* A look at the counters; false when the kernel would not give them. */
    bool Look()
    {
        system_resources_t Now{};
        if (system_resources(&Now) != 0) return false;
        if (Seen && Now.cpu_total_tsc > Last.cpu_total_tsc && Now.cpu_busy_tsc >= Last.cpu_busy_tsc) {
            std::uint64_t Total = Now.cpu_total_tsc - Last.cpu_total_tsc;
            std::uint64_t Busy  = Now.cpu_busy_tsc - Last.cpu_busy_tsc;
            int           Load  = static_cast<int>(std::min<std::uint64_t>(100, Busy * 100 / Total));
            if (History.size() == HistoryMost) History.erase(History.begin());
            History.push_back(Load);
        }
        Last = Now;
        Seen = true;

        system_info_t Info{};
        if (sysinfo(&Info) == 0 && Info.total_memory > 0)
            Memory = static_cast<int>(std::min<std::uint64_t>(100, Info.used_memory * 100 / Info.total_memory));
        return true;
    }

    Strip Draw(bool Wide) const
    {
        Strip Out;
        Out.Text(" cpu ", Quiet);
        std::size_t Shown = Wide ? HistoryMost : HistoryMost - 2;
        for (std::size_t Pad = History.size(); Pad < Shown; Pad++) Out.Cell(' ', Ink);
        for (std::size_t At = History.size() > Shown ? History.size() - Shown : 0; At < History.size(); At++)
            Out.Cell(Height(History[At]), Warmth(History[At]));
        if (History.empty()) Out.Text("   --%", Quiet);
        else Out.Text(std::format(" {:>3}%", History.back()), Warmth(History.back()) == Calm ? Ink : Warmth(History.back()));
        if (Wide && Memory >= 0) {
            Out.Text("  mem ", Quiet);
            Out.Text(std::format("{:>3}%", Memory), Memory >= 85 ? Hot : Ink);
        }
        Out.Cell(' ', Ink);
        return Out;
    }

private:
    system_resources_t Last{};
    bool               Seen   = false;
    std::vector<int>   History;
    int                Memory = -1;
};

}

int main()
{
    Pulse Machine;
    auto  Next = std::chrono::steady_clock::now();
    for (;;) {
        vga_dimensions_t Glass{};
        if (Machine.Look() && vga_getdimensions(&Glass) == 0) Machine.Draw(Glass.cols >= WideScreen).Paint(Glass);
        Next += Beat;
        std::this_thread::sleep_until(Next);
    }
}
