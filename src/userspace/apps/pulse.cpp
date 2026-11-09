/* pulse — the machine's pulse, as a notice layer beside the clock. */

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <format>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "box/error.h"
#include "box/life.h"
#include "box/system.h"

#include "painter/core/color.h"
#include "painter/tui/frame.h"
#include "painter/tui/glyph.h"
#include "painter/tui/surface.h"

BOX_LIFE("stays");

namespace {

using PainterColor = painter::core::Color;

constexpr auto         Beat        = std::chrono::milliseconds(500);
/* The clock's corner at its widest — 12:59:59 PM, as the person may set it
 * (settings clock): pulse stands left of all of it, however the clock is set. */
constexpr std::int32_t ClockWidth  = 11;
constexpr std::int32_t Gap         = 1;
constexpr std::size_t  HistoryMost = 10;
constexpr std::int32_t WideScreen  = 120;

constexpr PainterColor Ink    = PainterColor::Rgb(0xE6, 0xE6, 0xE6);
constexpr PainterColor Ground = PainterColor::Rgb(0x26, 0x26, 0x26);
constexpr PainterColor Quiet  = PainterColor::Rgb(0x8C, 0x8C, 0x8C);
constexpr PainterColor Calm   = PainterColor::Rgb(0x50, 0xD0, 0x60);
constexpr PainterColor Busy   = painter::core::Colors::Amber;
constexpr PainterColor Hot    = PainterColor::Rgb(0xFF, 0x50, 0x50);

PainterColor Warmth(int Percent)
{
    return Percent >= 85 ? Hot : Percent >= 50 ? Busy : Calm;
}

char Height(int Percent)
{
    if (Percent < 35) return '_';
    if (Percent < 70) return painter::tui::Glyphs::LowerHalf;
    return painter::tui::Glyphs::Full;
}

class Strip {
public:
    void Text(std::string_view Words, PainterColor Fore)
    {
        for (char Ch : Words) Cell(Ch, Fore);
    }

    void Cell(char Glyph, PainterColor Fore)
    {
        Cells.push_back(painter::tui::Cell{Glyph, Fore, Ground});
    }

    std::expected<painter::tui::Frame, box::error> Frame() const
    {
        std::expected<painter::tui::Frame, box::error> Created =
            painter::tui::Frame::Create({static_cast<std::int32_t>(Cells.size()), 1});
        if (!Created) return std::unexpected(Created.error());
        painter::tui::Frame Result = std::move(*Created);
        for (std::size_t Col = 0; Col < Cells.size(); Col++)
            Result.Put({static_cast<std::int32_t>(Col), 0}, Cells[Col]);
        return Result;
    }

    std::size_t Size() const noexcept { return Cells.size(); }

private:
    std::vector<painter::tui::Cell> Cells;
};

class Pulse {
public:
    bool Look()
    {
        system_resources_t Now{};
        if (system_resources(&Now) != 0) return false;
        if (Seen && Now.cpu_total_tsc > Last.cpu_total_tsc &&
            Now.cpu_busy_tsc >= Last.cpu_busy_tsc) {
            const std::uint64_t Total = Now.cpu_total_tsc - Last.cpu_total_tsc;
            const std::uint64_t Busy  = Now.cpu_busy_tsc - Last.cpu_busy_tsc;
            const int Load = static_cast<int>(std::min<std::uint64_t>(100, Busy * 100 / Total));
            if (History.size() == HistoryMost) History.erase(History.begin());
            History.push_back(Load);
        }
        Last = Now;
        Seen = true;

        system_info_t Info{};
        if (sysinfo(&Info) == 0 && Info.total_memory > 0)
            Memory = static_cast<int>(std::min<std::uint64_t>(100,
                                                               Info.used_memory * 100 / Info.total_memory));
        return true;
    }

    Strip Draw(bool Wide) const
    {
        Strip Out;
        Out.Text(" cpu ", Quiet);
        const std::size_t Shown = Wide ? HistoryMost : HistoryMost - 2;
        for (std::size_t Pad = History.size(); Pad < Shown; Pad++) Out.Cell(' ', Ink);
        const std::size_t First = History.size() > Shown ? History.size() - Shown : 0;
        for (std::size_t At = First; At < History.size(); At++)
            Out.Cell(Height(History[At]), Warmth(History[At]));
        if (History.empty()) {
            Out.Text("   --%", Quiet);
        } else {
            const PainterColor Figure = Warmth(History.back()) == Calm ? Ink : Warmth(History.back());
            Out.Text(std::format(" {:>3}%", History.back()), Figure);
        }
        if (Wide && Memory >= 0) {
            Out.Text("  mem ", Quiet);
            Out.Text(std::format("{:>3}%", Memory), Memory >= 85 ? Hot : Ink);
        }
        Out.Cell(' ', Ink);
        return Out;
    }

private:
    system_resources_t Last{};
    bool               Seen = false;
    std::vector<int>   History;
    int                Memory = -1;
};

}  // namespace

int main()
{
    std::expected<painter::tui::Surface, painter::tui::StageError> Opened =
        painter::tui::Surface::Open(12, 1, painter::tui::SurfaceRole::Notice);
    if (!Opened) return 1;
    painter::tui::Surface Screen = std::move(*Opened);
    const painter::core::Size Dimensions = Screen.Size();
    if (Dimensions.Width <= ClockWidth + Gap || Dimensions.Height == 0) return 1;

    Pulse Machine;
    auto Next = std::chrono::steady_clock::now();
    for (;;) {
        if (Machine.Look()) {
            Strip Picture = Machine.Draw(Dimensions.Width >= WideScreen);
            std::expected<painter::tui::Frame, box::error> Canvas = Picture.Frame();
            const int Right = Dimensions.Width - ClockWidth - Gap;
            if (!Canvas || static_cast<int>(Picture.Size()) > Right) break;
            if (!Screen.Clear() ||
                !Screen.Composite({Right - static_cast<int>(Picture.Size()), 0}, *Canvas))
                break;
        }
        Next += Beat;
        std::this_thread::sleep_until(Next);
    }
    return 1;
}
