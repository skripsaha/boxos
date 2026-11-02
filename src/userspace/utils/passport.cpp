/* passport — a BoxOS machine portrait.
 *
 * The screen path owns one finished TextCell frame and hands it to the glass
 * with one vga_paint. The logo is a two-pixel-high raster per console cell:
 * the CP437 upper-half block carries one colour in its foreground and one in
 * its background. Under an arrow the same report is emitted as text, because
 * a painted frame is deliberately not part of the console text stream.
 */

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <new>
#include <string>
#include <string_view>
#include <vector>

#include "box/clock.h"
#include "box/color.h"
#include "box/error.h"
#include "box/form.h"
#include "box/print.h"
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
constexpr Color PupilGlow   = COLOR_RGB(0x3A, 0x3A, 0x40);

constexpr Color PanelBackground = COLOR_RGB(0x12, 0x09, 0x12);
constexpr Color TitleInk        = BubbleLight;
constexpr Color LabelInk        = BubbleLight;
constexpr Color ValueInk        = COLOR_LIGHT_GRAY;
constexpr Color AccentInk       = BubblePink;
constexpr Color StatusInk       = BubbleMid;

constexpr std::size_t LogoPixelWidth  = 32;
constexpr std::size_t LogoPixelHeight = 32;
constexpr std::size_t LogoCellRows    = LogoPixelHeight / 2;
constexpr std::size_t LogoOriginX     = 1;
constexpr std::size_t LogoOriginY     = 2;

constexpr std::size_t PanelOriginX = 37;
constexpr std::size_t PanelOriginY = 2;
constexpr std::size_t PanelWidth   = 42;
constexpr std::size_t PanelHeight  = 15;
constexpr std::size_t PanelInner   = PanelWidth - 2;
constexpr std::size_t ValueColumn  = 13;
constexpr std::size_t MinimumCols  = PanelOriginX + PanelWidth;
constexpr std::size_t MinimumRows  = PanelOriginY + PanelHeight + 1;

constexpr char UpperHalfBlock = static_cast<char>(0xDF);

struct ReportField {
    std::string Label;
    std::string Value;
    Color       Ink;
};

struct Report {
    std::vector<ReportField> Fields;
};

struct ReportSample {
    system_info_t      Info{};
    system_resources_t Before{};
    system_resources_t After{};
    bool               ResourcesKnown = false;
    bool               LoadKnown      = false;
};

TextCell MakeCell(char Character, Color Foreground, Color Background)
{
    TextCell Cell{};
    Cell.ch = Character;
    Cell.fg = Foreground;
    Cell.bg = Background;
    return Cell;
}

void PutCell(std::vector<TextCell> &Frame, std::size_t Rows, std::size_t Cols,
             std::size_t Row, std::size_t Col, char Character,
             Color Foreground, Color Background)
{
    if (Row >= Rows || Col >= Cols) return;
    Frame[Row * Cols + Col] = MakeCell(Character, Foreground, Background);
}

void FillCellRange(std::vector<TextCell> &Frame, std::size_t Rows, std::size_t Cols,
                   std::size_t Row, std::size_t First, std::size_t Last,
                   char Character, Color Foreground, Color Background)
{
    if (Row >= Rows || First >= Cols) return;
    Last = std::min(Last, Cols);
    for (std::size_t Col = First; Col < Last; Col++)
        PutCell(Frame, Rows, Cols, Row, Col, Character, Foreground, Background);
}

void PutText(std::vector<TextCell> &Frame, std::size_t Rows, std::size_t Cols,
             std::size_t Row, std::size_t Col, std::string_view Text,
             Color Foreground, Color Background, std::size_t Limit)
{
    std::size_t Count = std::min(Text.size(), Limit);
    for (std::size_t I = 0; I < Count; I++) {
        unsigned char Byte = static_cast<unsigned char>(Text[I]);
        char Character = (Byte >= 0x20 && Byte < 0x7F) ? static_cast<char>(Byte) : '?';
        PutCell(Frame, Rows, Cols, Row, Col + I, Character, Foreground, Background);
    }
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

Report MakeReport(const ReportSample &Sample)
{
    const system_info_t &Info = Sample.Info;
    Report               Result;
    Result.Fields.reserve(10);

    Result.Fields.push_back({"system", FixedText(Info.version, sizeof(Info.version)), AccentInk});
    Result.Fields.push_back({"uptime", Uptime(Info.uptime_ns), ValueInk});
    Result.Fields.push_back({
        "memory",
        std::format("{} / {} / {}", CompactBytes(Info.used_memory), CompactBytes(Info.free_memory),
                    CompactBytes(Info.total_memory)),
        ValueInk});
    Result.Fields.push_back({
        "cpu",
        std::format("{} cores / {} K / {} App", Info.cpu_total, Info.cpu_k_cores, Info.cpu_app_cores),
        ValueInk});
    Result.Fields.push_back({"load", Sample.LoadKnown ? CpuLoad(Sample.Before, Sample.After) : "unavailable",
                             Sample.LoadKnown ? AccentInk : StatusInk});

    if (Sample.ResourcesKnown && Sample.Before.volume_present) {
        std::string Volume = FixedText(Sample.Before.volume_name, sizeof(Sample.Before.volume_name));
        if (Volume.empty()) Volume = "unnamed volume";
        if (Sample.Before.volume_free_counted)
            Volume += std::format(" / {} free", CompactBytes(Sample.Before.volume_free_bytes));
        else
            Volume += " / free ground not counted";
        Result.Fields.push_back({"volume", std::move(Volume), ValueInk});
    } else {
        Result.Fields.push_back({"volume", "unavailable", StatusInk});
    }

    Result.Fields.push_back({"processes", std::format("{} live", Info.process_count), ValueInk});

    std::string Runtime = Info.multicore_active ? "AMP" : "single-core";
    if (Info.has_waitpkg) Runtime += " / UMWAIT";
    Result.Fields.push_back({"runtime", std::move(Runtime), ValueInk});

    std::string Timing = Frequency(Info.tsc_freq_khz);
    if (!Info.has_invariant_tsc) Timing += " / non-invariant";
    Result.Fields.push_back({"timing", std::move(Timing), Info.has_invariant_tsc ? ValueInk : AccentInk});
    Result.Fields.push_back({"tick", TickSource(Info), ValueInk});
    return Result;
}

int DistanceSquared(int X, int Y, int CenterX, int CenterY)
{
    int Dx = X - CenterX;
    int Dy = Y - CenterY;
    return Dx * Dx + Dy * Dy;
}

Color SurfaceColor(int Distance, int RadiusSquared)
{
    int Weight = RadiusSquared == 0 ? 255 : (RadiusSquared - Distance) * 255 / RadiusSquared;
    Weight = std::clamp(Weight, 0, 255);
    return color_mix(BubbleDark, BubblePink, static_cast<uint8_t>(Weight));
}

Color BubblePixel(int X, int Y)
{
    constexpr int Center = 15;
    constexpr int Outer  = 14 * 14;
    constexpr int Ring   = 9 * 9;
    constexpr int Pupil  = 6 * 6;

    int Distance = DistanceSquared(X, Y, Center, Center);
    if (Distance > Outer) return COLOR_BLACK;

    if (Distance <= Pupil) {
        if (X < 13 && Y < 13 && Distance > 15)
            return color_mix(BubbleBlack, PupilGlow, 90);
        return BubbleBlack;
    }

    if (Distance <= Ring) {
        int Weight = (Ring - Distance) * 255 / (Ring - Pupil);
        return color_mix(BubbleMid, BubbleLight, static_cast<uint8_t>(std::clamp(Weight, 0, 255)));
    }
    return SurfaceColor(Distance - Ring, Outer - Ring);
}

Color SatellitePixel(int X, int Y, int CenterX, int CenterY, int Radius)
{
    int RadiusSquared = Radius * Radius;
    int Distance      = DistanceSquared(X, Y, CenterX, CenterY);
    return Distance <= RadiusSquared ? SurfaceColor(Distance, RadiusSquared) : COLOR_BLACK;
}

Color LogoPixel(int X, int Y)
{
    Color Main = BubblePixel(X, Y);
    if (Main != COLOR_BLACK) return Main;

    Color TopLeft = SatellitePixel(X, Y, 3, 4, 3);
    if (TopLeft != COLOR_BLACK) return TopLeft;
    Color Right = SatellitePixel(X, Y, 30, 12, 2);
    if (Right != COLOR_BLACK) return Right;
    return SatellitePixel(X, Y, 27, 28, 4);
}

void PaintLogo(std::vector<TextCell> &Frame, std::size_t Rows, std::size_t Cols)
{
    for (std::size_t PixelRow = 0; PixelRow < LogoPixelHeight; PixelRow += 2) {
        for (std::size_t PixelCol = 0; PixelCol < LogoPixelWidth; PixelCol++) {
            Color Top    = LogoPixel(static_cast<int>(PixelCol), static_cast<int>(PixelRow));
            Color Bottom = LogoPixel(static_cast<int>(PixelCol), static_cast<int>(PixelRow + 1));
            char Character = Top == COLOR_BLACK && Bottom == COLOR_BLACK ? ' ' : UpperHalfBlock;
            PutCell(Frame, Rows, Cols, LogoOriginY + PixelRow / 2, LogoOriginX + PixelCol,
                    Character, Top, Bottom);
        }
    }
}

std::string FitText(std::string_view Text, std::size_t Width)
{
    std::string Result(Text.substr(0, std::min(Text.size(), Width)));
    Result.resize(Width, ' ');
    return Result;
}

std::string PanelLine(std::string_view Content)
{
    return "|" + FitText(Content, PanelInner) + "|";
}

std::vector<std::string> MakePanelLines(const Report &ReportData)
{
    std::vector<std::string> Lines;
    Lines.reserve(PanelHeight);
    Lines.push_back("+" + std::string(PanelInner, '-') + "+");
    Lines.push_back(PanelLine(" passport  /  machine portrait"));
    Lines.push_back(PanelLine(std::string(PanelInner, '-')));

    for (const ReportField &Field : ReportData.Fields) {
        std::string Content = " ";
        Content += Field.Label;
        Content.resize(ValueColumn - 1, ' ');
        Content += Field.Value;
        Lines.push_back(PanelLine(Content));
    }

    Lines.push_back(PanelLine(std::string(PanelInner, '-')));
    Lines.push_back("+" + std::string(PanelInner, '-') + "+");
    return Lines;
}

void PaintPanel(std::vector<TextCell> &Frame, std::size_t Rows, std::size_t Cols,
                const Report &ReportData)
{
    for (std::size_t Row = PanelOriginY; Row < PanelOriginY + PanelHeight; Row++)
        FillCellRange(Frame, Rows, Cols, Row, PanelOriginX, PanelOriginX + PanelWidth,
                      ' ', ValueInk, PanelBackground);

    std::size_t Left = PanelOriginX;
    std::size_t Right = PanelOriginX + PanelWidth - 1;
    std::size_t Top = PanelOriginY;
    std::size_t Bottom = PanelOriginY + PanelHeight - 1;

    for (std::size_t Col = Left + 1; Col < Right; Col++) {
        PutCell(Frame, Rows, Cols, Top, Col, '-', BubbleMid, PanelBackground);
        PutCell(Frame, Rows, Cols, Bottom, Col, '-', BubbleMid, PanelBackground);
    }
    PutCell(Frame, Rows, Cols, Top, Left, '+', BubbleMid, PanelBackground);
    PutCell(Frame, Rows, Cols, Top, Right, '+', BubbleMid, PanelBackground);
    PutCell(Frame, Rows, Cols, Bottom, Left, '+', BubbleMid, PanelBackground);
    PutCell(Frame, Rows, Cols, Bottom, Right, '+', BubbleMid, PanelBackground);

    for (std::size_t Row = Top + 1; Row < Bottom; Row++) {
        PutCell(Frame, Rows, Cols, Row, Left, '|', BubbleMid, PanelBackground);
        PutCell(Frame, Rows, Cols, Row, Right, '|', BubbleMid, PanelBackground);
    }

    FillCellRange(Frame, Rows, Cols, Top + 1, Left + 1, Right, ' ', TitleInk, BubbleDark);
    PutCell(Frame, Rows, Cols, Top + 1, Left, '|', TitleInk, BubbleDark);
    PutCell(Frame, Rows, Cols, Top + 1, Right, '|', TitleInk, BubbleDark);
    PutText(Frame, Rows, Cols, Top + 1, Left + 2, "passport  /  machine portrait",
            TitleInk, BubbleDark, PanelInner - 2);

    FillCellRange(Frame, Rows, Cols, Top + 2, Left + 1, Right, '-', BubbleDark, PanelBackground);
    FillCellRange(Frame, Rows, Cols, Bottom - 1, Left + 1, Right, '-', BubbleDark, PanelBackground);

    for (std::size_t I = 0; I < ReportData.Fields.size(); I++) {
        const ReportField &Field = ReportData.Fields[I];
        std::size_t Row = Top + 3 + I;
        PutText(Frame, Rows, Cols, Row, Left + 2, Field.Label,
                LabelInk, PanelBackground, ValueColumn - 2);
        PutText(Frame, Rows, Cols, Row, Left + ValueColumn, Field.Value,
                Field.Ink, PanelBackground, PanelInner - ValueColumn + 1);
    }
}

std::string TextLogoLine(std::size_t Row)
{
    std::string Line;
    Line.reserve(LogoPixelWidth);
    for (std::size_t Col = 0; Col < LogoPixelWidth; Col++) {
        Color Top = LogoPixel(static_cast<int>(Col), static_cast<int>(Row * 2));
        Color Bottom = LogoPixel(static_cast<int>(Col), static_cast<int>(Row * 2 + 1));
        if (Top == COLOR_BLACK && Bottom == COLOR_BLACK) Line += ' ';
        else if (Top == BubbleBlack || Bottom == BubbleBlack) Line += '@';
        else if (Top == BubbleLight || Bottom == BubbleLight) Line += 'o';
        else Line += '#';
    }
    return Line;
}

std::string TextReport(const Report &ReportData)
{
    std::vector<std::string> Panel = MakePanelLines(ReportData);
    std::string Output;
    std::size_t Rows = std::max(LogoCellRows, Panel.size());
    for (std::size_t Row = 0; Row < Rows; Row++) {
        Output.append(LogoOriginX, ' ');
        Output += Row < LogoCellRows ? TextLogoLine(Row) : std::string(LogoPixelWidth, ' ');
        Output.append(PanelOriginX - LogoOriginX - LogoPixelWidth, ' ');
        if (Row < Panel.size()) Output += Panel[Row];
        Output += '\n';
    }
    return Output;
}

bool PaintReport(const Report &ReportData)
{
    vga_dimensions_t Dimensions{};
    if (vga_getdimensions(&Dimensions) != 0) {
        print_to_screen("passport: this screen would not say how big it is\n");
        return false;
    }
    if (Dimensions.cols < MinimumCols || Dimensions.rows < MinimumRows) {
        print_to_screen(std::format("passport: screen is {} columns by {} rows; needs at least {} by {}\n",
                                    Dimensions.cols, Dimensions.rows, MinimumCols, MinimumRows).c_str());
        return false;
    }

    vga_pos_t Cursor{};
    (void)vga_getcursor(&Cursor);

    constexpr std::size_t ReportRows = LogoOriginY + LogoCellRows;
    constexpr std::size_t ReportCols = PanelOriginX + PanelWidth;
    std::size_t PaintTop = Cursor.row;
    if (PaintTop + ReportRows > Dimensions.rows)
        PaintTop = Dimensions.rows > ReportRows ? Dimensions.rows - ReportRows : 0;

    std::vector<TextCell> Frame(ReportRows * ReportCols, MakeCell(' ', ValueInk, COLOR_BLACK));
    PaintLogo(Frame, ReportRows, ReportCols);
    PaintPanel(Frame, ReportRows, ReportCols, ReportData);

    std::size_t AfterReport = std::min<std::size_t>(Dimensions.rows - 1, PaintTop + ReportRows);
    vga_setcursor(static_cast<uint8_t>(AfterReport), 0);
    return vga_paint(static_cast<uint8_t>(PaintTop), 0,
                     static_cast<uint8_t>(ReportRows), static_cast<uint8_t>(ReportCols), Frame.data()) == 0;
}

int Run()
{
    ReportSample Sample;
    if (sysinfo(&Sample.Info) != 0) {
        print_to_screen("passport: the system would not provide its information\n");
        return 1;
    }

    Sample.ResourcesKnown = system_resources(&Sample.Before) == 0;
    if (Sample.ResourcesKnown) {
        WaitForSampleWindow(100);
        Sample.LoadKnown = system_resources(&Sample.After) == 0;
    }

    Report ReportData = MakeReport(Sample);
    if (form_screen_file() != 0) {
        std::string Text = TextReport(ReportData);
        print_bytes(Text.data(), Text.size());
        return 0;
    }
    return PaintReport(ReportData) ? 0 : 1;
}

} // namespace

int main()
{
    try {
        return Run();
    } catch (const std::bad_alloc &) {
        print_to_screen("passport: there is not enough memory for its portrait\n");
        return 1;
    }
}
