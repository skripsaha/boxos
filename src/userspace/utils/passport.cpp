/* passport — a BoxOS machine portrait.
 *
 * It is deliberately a report, not a clone of a Linux fetch command: the
 * left side is BoxOS's pink bubble mark and the right side names the machine
 * through the information the existing system and resource decks publish.
 * The same block can be read by a person or sent through `->` to a file.
 */

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <string>
#include <string_view>
#include <vector>

#include "box/clock.h"
#include "box/color.h"
#include "box/form.h"
#include "box/print.h"
#include "box/string.h"
#include "box/sync.h"
#include "box/system.h"
#include "box/vga.h"

BOX_FORM("gives text\n");

namespace {

constexpr Color BubbleDark  = COLOR_RGB(0x81, 0x26, 0x5C);
constexpr Color BubbleMid   = COLOR_RGB(0xB8, 0x3E, 0x79);
constexpr Color BubblePink  = COLOR_RGB(0xE8, 0x5C, 0x93);
constexpr Color BubbleLight = COLOR_RGB(0xFF, 0xE2, 0xF0);
constexpr Color BubbleBlack = COLOR_RGB(0x0F, 0x0F, 0x12);

constexpr Color TitleInk    = BubbleLight;
constexpr Color LabelInk    = COLOR_SLATE;
constexpr Color ValueInk    = COLOR_LIGHT_GRAY;
constexpr Color AccentInk   = BubblePink;
constexpr Color StatusInk   = BubbleMid;

constexpr std::size_t LogoWidth = 38;

struct Run {
    Color       Ink;
    std::string Text;
};

using Line = std::vector<Run>;

void Add(Line &Output, Color Ink, std::string_view Text)
{
    if (Text.empty()) return;
    if (!Output.empty() && Output.back().Ink == Ink) {
        Output.back().Text += Text;
        return;
    }
    Output.push_back({Ink, std::string(Text)});
}

void AddBlock(Line &Output, char Mark, std::size_t Count)
{
    static constexpr char Block[] = "\xE2\x96\x88";
    if (Mark == ' ') {
        Add(Output, COLOR_BLACK, std::string(Count, ' '));
        return;
    }

    Color Ink = BubbleMid;
    switch (Mark) {
    case 'p': Ink = BubblePink; break;
    case 'P': Ink = BubbleMid; break;
    case 'l': Ink = BubbleLight; break;
    case 'k': Ink = BubbleBlack; break;
    case 's': Ink = BubbleDark; break;
    case 'S': Ink = BubblePink; break;
    default:  break;
    }
    for (std::size_t I = 0; I < Count; I++) Add(Output, Ink, Block);
}

/* One row of a small terminal portrait. The letters are colour cells:
 * p/P are the bubble, l the inner ring, k the pupil, and s/S the satellites.
 * Keeping the mark as text means an arrow still produces a useful report. */
Line LogoLine(std::string_view Shape)
{
    Line Output;
    char Previous = '\0';
    std::size_t Count = 0;

    auto Flush = [&] {
        if (Count != 0) AddBlock(Output, Previous, Count);
        Count = 0;
    };

    for (char Mark : Shape) {
        if (Mark == Previous) {
            Count++;
        } else {
            Flush();
            Previous = Mark;
            Count = 1;
        }
    }
    Flush();

    if (Shape.size() < LogoWidth) Add(Output, COLOR_BLACK, std::string(LogoWidth - Shape.size(), ' '));
    return Output;
}

std::string FixedText(const char *Bytes, std::size_t Capacity)
{
    std::size_t Length = 0;
    while (Length < Capacity && Bytes[Length] != '\0') Length++;
    return std::string(Bytes, Length);
}

std::string CompactBytes(uint64_t Value)
{
    constexpr uint64_t KiB = 1ull << 10;
    constexpr uint64_t MiB = 1ull << 20;
    constexpr uint64_t GiB = 1ull << 30;

    if (Value >= GiB) return std::format("{} GiB", Value / GiB);
    if (Value >= MiB) return std::format("{} MiB", Value / MiB);
    if (Value >= KiB) return std::format("{} KiB", Value / KiB);
    return std::format("{} B", Value);
}

std::string Uptime(uint64_t Nanoseconds)
{
    uint64_t Seconds = Nanoseconds / 1000000000ull;
    uint64_t Days    = Seconds / 86400;
    Seconds %= 86400;
    uint64_t Hours   = Seconds / 3600;
    Seconds %= 3600;
    uint64_t Minutes = Seconds / 60;
    Seconds %= 60;

    if (Days != 0) return std::format("{}d {:02}h {:02}m", Days, Hours, Minutes);
    if (Hours != 0) return std::format("{}h {:02}m {:02}s", Hours, Minutes, Seconds);
    if (Minutes != 0) return std::format("{}m {:02}s", Minutes, Seconds);
    return std::format("{}s", Seconds);
}

std::string Frequency(uint64_t KiloHertz)
{
    if (KiloHertz == 0) return "unavailable";
    if (KiloHertz >= 1000000) {
        uint64_t Whole = KiloHertz / 1000000;
        uint64_t Frac  = (KiloHertz % 1000000) / 10000;
        return std::format("{}.{:02} GHz", Whole, Frac);
    }
    return std::format("{} kHz", KiloHertz);
}

std::string TickSource(const system_info_t &Info)
{
    switch (Info.tick_source) {
    case SYSINFO_TICK_SOURCE_HPET_LEGACY:
        return std::format("{} Hz / HPET timer 0", Info.tick_freq_hz);
    case SYSINFO_TICK_SOURCE_8254:
        return std::format("{} Hz / 8254 channel 0", Info.tick_freq_hz);
    default:
        return "no IRQ0 source";
    }
}

std::string CpuLoad(const system_resources_t &Before, const system_resources_t &After)
{
    if (After.cpu_total_tsc < Before.cpu_total_tsc || After.cpu_busy_tsc < Before.cpu_busy_tsc)
        return "unavailable";

    uint64_t Total = After.cpu_total_tsc - Before.cpu_total_tsc;
    uint64_t Busy  = After.cpu_busy_tsc - Before.cpu_busy_tsc;
    if (Total == 0) return "unavailable";
    if (Busy >= Total) return "100%";
    return std::format("{}%", (Busy * 100) / Total);
}

void WaitForSampleWindow(uint32_t Milliseconds)
{
    volatile uint64_t WaitWord __attribute__((aligned(8))) = 0;
    error_t Rc = addr_park(&WaitWord, 0, Milliseconds);
    if (Rc == OK || Rc == ERR_TIMEOUT) return;

    uint64_t Started = clock_uptime_ms();
    while (clock_uptime_ms() - Started < Milliseconds) yield();
}

void Field(Line &Output, std::string_view Label, std::string_view Value, Color Ink = ValueInk)
{
    Add(Output, LabelInk, "  ");
    Add(Output, LabelInk, Label);
    Add(Output, LabelInk, "  ");
    Add(Output, Ink, Value);
}

void Emit(const std::vector<Line> &Lines)
{
    vga_begin();
    set_color_bg(COLOR_BLACK);
    for (const Line &One : Lines) {
        for (const Run &Part : One) {
            set_color(Part.Ink);
            print_bytes(Part.Text.data(), Part.Text.size());
        }
        set_color(COLOR_DEFAULT);
        print_bytes("\n", 1);
    }
    set_color(COLOR_DEFAULT);
    set_color_bg(COLOR_BLACK);
    vga_commit();
}

std::vector<Line> BuildReport(const system_info_t &Info,
                              bool ResourcesKnown,
                              const system_resources_t &Resources,
                              bool LoadKnown,
                              const system_resources_t &After)
{
    static constexpr std::array<std::string_view, 15> Logo = {
        "  sss       ppppppppppp               ",
        " sSSss   ppppppppppppppppp            ",
        " sssss ppppppppppppppppppppp          ",
        "  sss ppppppppppppppppppppppp         ",
        "    ppppppplllllllllllllppppppp       ",
        "   ppppppllllkkkkkkkkkllllpppppp   sss",
        "  pppppllllkkkkkkkkkkkkkllllppppp sSSs",
        "  pppplllkkkkkkkkkkkkkkkkklllpppp ssss",
        "  pppppllllkkkkkkkkkkkkkllllppppp  sss",
        "   ppppppllllkkkkkkkkkllllpppppp      ",
        "    ppppppplllllllllllllppppppp       ",
        "      ppppppppppppppppppppppp sssss   ",
        "       ppppppppppppppppppppp sssssss  ",
        "         ppppppppppppppppp   ssSSSss  ",
        "            ppppppppppp       sssss   ",
    };

    std::vector<Line> Report;
    std::vector<Line> Details;

    Line Title;
    Add(Title, TitleInk, "passport");
    Details.push_back(std::move(Title));

    Line Subtitle;
    Add(Subtitle, LabelInk, "used / free / total memory");
    Details.push_back(std::move(Subtitle));

    Details.push_back({});

    Line System;
    Field(System, "system", FixedText(Info.version, sizeof(Info.version)), AccentInk);
    Details.push_back(std::move(System));

    Line Up;
    Field(Up, "uptime", Uptime(Info.uptime_ns));
    Details.push_back(std::move(Up));

    Line Memory;
    Memory.reserve(4);
    Field(Memory, "memory",
          std::format("{} / {} / {}", CompactBytes(Info.used_memory), CompactBytes(Info.free_memory),
                      CompactBytes(Info.total_memory)));
    Details.push_back(std::move(Memory));

    Line Cpu;
    Field(Cpu, "cpu",
          std::format("{} cores / {} K / {} App", Info.cpu_total, Info.cpu_k_cores, Info.cpu_app_cores));
    Details.push_back(std::move(Cpu));

    Line Load;
    Field(Load, "load", LoadKnown ? CpuLoad(Resources, After) : "unavailable",
          LoadKnown ? AccentInk : StatusInk);
    Details.push_back(std::move(Load));

    Line Volume;
    if (ResourcesKnown && Resources.volume_present) {
        std::string Name = FixedText(Resources.volume_name, sizeof(Resources.volume_name));
        std::string Text = Name.empty() ? "unnamed volume" : Name;
        if (Resources.volume_free_counted)
            Text += std::format(" / {} free", CompactBytes(Resources.volume_free_bytes));
        else
            Text += " / free ground not counted";
        Field(Volume, "volume", Text);
    } else {
        Field(Volume, "volume", "unavailable", StatusInk);
    }
    Details.push_back(std::move(Volume));

    Line Processes;
    Field(Processes, "processes", std::format("{} live", Info.process_count));
    Details.push_back(std::move(Processes));

    Line Runtime;
    std::string Mode = Info.multicore_active ? "AMP" : "single-core";
    if (Info.has_waitpkg) Mode += " / UMWAIT";
    Field(Runtime, "runtime", Mode);
    Details.push_back(std::move(Runtime));

    Line Timing;
    std::string TimingText = Frequency(Info.tsc_freq_khz);
    if (!Info.has_invariant_tsc) TimingText += " / non-invariant";
    Field(Timing, "timing", TimingText, Info.has_invariant_tsc ? ValueInk : StatusInk);
    Details.push_back(std::move(Timing));

    Line Tick;
    Field(Tick, "tick", TickSource(Info));
    Details.push_back(std::move(Tick));

    std::size_t Rows = Logo.size() > Details.size() ? Logo.size() : Details.size();
    Report.reserve(Rows + 1);
    for (std::size_t Row = 0; Row < Rows; Row++) {
        Line Combined;
        if (Row < Logo.size()) {
            Line Picture = LogoLine(Logo[Row]);
            for (Run &Part : Picture) Combined.push_back(std::move(Part));
        } else {
            Add(Combined, COLOR_BLACK, std::string(LogoWidth, ' '));
        }

        Add(Combined, COLOR_BLACK, "    ");
        if (Row < Details.size()) {
            for (Run &Part : Details[Row]) Combined.push_back(std::move(Part));
        }
        Report.push_back(std::move(Combined));
    }

    Line Foot;
    Add(Foot, LabelInk, "  read-only snapshot / BoxOS native");
    Report.push_back(std::move(Foot));
    return Report;
}

} // namespace

int main()
{
    system_info_t Info{};
    if (sysinfo(&Info) != 0) {
        print_to_screen("passport: the system would not provide its information\n");
        return 1;
    }

    system_resources_t Before{};
    system_resources_t After{};
    bool ResourcesKnown = system_resources(&Before) == 0;
    bool LoadKnown      = false;
    if (ResourcesKnown) {
        WaitForSampleWindow(100);
        LoadKnown = system_resources(&After) == 0;
    }

    Emit(BuildReport(Info, ResourcesKnown, Before, LoadKnown, After));
    return 0;
}
