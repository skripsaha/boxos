/* passport — a BoxOS machine portrait.
 *
 * The bubble on the left, and beside it a card of what this machine is and
 * what it is doing: the Use Context first, then memory, the volume and the
 * load as gauges, then the cores and their clocks, then the console's sixteen
 * colours. The card is built once as lines of coloured runs; the screen path
 * paints them into one Painter frame and presents that regional surface, and
 * under an arrow the same lines are written as text, because a painted frame
 * is deliberately not part of the console text stream.
 *
 * The logo is a two-pixel-high raster per console cell: the upper-half block
 * carries one colour in its foreground and one in its background. */

#include <algorithm>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cuchar>
#include <format>
#include <new>
#include <string>
#include <string_view>
#include <vector>

#include "box/cxx/whose.h"
#include "box/clock.h"
#include "box/error.h"
#include "box/file.h"
#include "box/form.h"
#include "box/memory.h"
#include "box/print.h"
#include "box/sync.h"
#include "box/system.h"

#include "keep.h"
#include "painter/core/color.h"
#include "painter/tui/glyph.h"
#include "painter/tui/console_surface.h"
#include "painter/tui/text.h"

BOX_FORM("gives text\n");

namespace {

using PainterColor = painter::core::Color;

constexpr PainterColor BubbleDark  = PainterColor::Rgb(0x81, 0x26, 0x5C);
constexpr PainterColor BubbleMid   = PainterColor::Rgb(0xB8, 0x3E, 0x79);
constexpr PainterColor BubblePink  = PainterColor::Rgb(0xE8, 0x5C, 0x93);
constexpr PainterColor BubbleLight = PainterColor::Rgb(0xFF, 0xE2, 0xF0);
constexpr PainterColor BubbleBlack = PainterColor::Rgb(0x0F, 0x0F, 0x12);
constexpr PainterColor PupilGlow   = PainterColor::Rgb(0x3A, 0x3A, 0x40);

constexpr PainterColor Screen          = PainterColor::Screen();
constexpr PainterColor PanelBackground = Screen;
constexpr PainterColor TitleInk        = BubbleLight;
constexpr PainterColor LabelInk        = BubbleLight;
constexpr PainterColor ValueInk        = painter::core::Colors::LightGray;
constexpr PainterColor QuietInk        = PainterColor::Rgb(0x8C, 0x8C, 0x8C);
constexpr PainterColor AccentInk       = BubblePink;
constexpr PainterColor StatusInk       = BubbleMid;
constexpr PainterColor FrameInk        = BubbleMid;

/* A gauge is green while there is room, amber when it is getting full, red
 * when it nearly is; what is left of it is dots. */
constexpr PainterColor GaugeGood  = PainterColor::Rgb(0x50, 0xD0, 0x60);
constexpr PainterColor GaugeTight = painter::core::Colors::Amber;
constexpr PainterColor GaugeFull  = PainterColor::Rgb(0xFF, 0x50, 0x50);
constexpr PainterColor GaugeLeft  = PainterColor::Rgb(0x4A, 0x4A, 0x4A);

constexpr std::size_t LogoPixelWidth  = 32;
constexpr std::size_t LogoPixelHeight = 32;
constexpr std::size_t LogoCellRows    = LogoPixelHeight / 2;
constexpr std::size_t LogoOriginX     = 1;
constexpr std::size_t TopMargin       = 1;

constexpr std::size_t PanelOriginX = 35;
constexpr std::size_t PanelWidth   = 45;
constexpr std::size_t PanelInner   = PanelWidth - 2;
constexpr std::size_t ContentWidth = PanelInner - 2;   /* a blank inside each side */
constexpr std::size_t LabelWidth   = 11;
constexpr std::size_t ValueWidth   = ContentWidth - LabelWidth;
constexpr std::size_t GaugeWidth   = ValueWidth - 5;   /* and " 100%" after it */
constexpr std::size_t MinimumCols  = PanelOriginX + PanelWidth;

namespace Glyph {
constexpr char32_t UpperHalf = U'\u2580';   /* ▀ */
constexpr char32_t LowerHalf = U'\u2584';   /* ▄ */
constexpr char32_t Across    = U'\u2500';   /* ─ */
constexpr char32_t Down      = U'\u2502';   /* │ */
constexpr char32_t TopLeft   = U'\u250C';   /* ┌ */
constexpr char32_t TopRight  = U'\u2510';   /* ┐ */
constexpr char32_t LowLeft   = U'\u2514';   /* └ */
constexpr char32_t LowRight  = U'\u2518';   /* ┘ */
constexpr char32_t TeeLeft   = U'\u251C';   /* ├ */
constexpr char32_t TeeRight  = U'\u2524';   /* ┤ */
constexpr char32_t Filled    = U'\u25A0';   /* ■ a gauge's share */
constexpr char32_t Left      = U'\u00B7';   /* · and what is left of it */
}

/* Count times the character Shown, as UTF-8. */
std::string Repeated(char32_t Shown, std::size_t Count)
{
    char              Spelled[MB_LEN_MAX];
    std::mbstate_t    State{};
    const std::size_t Length = std::c32rtomb(Spelled, Shown, &State);
    std::string       Out;
    for (std::size_t Each = 0; Each < Count; Each++) Out.append(Spelled, Length);
    return Out;
}

/* ---- the card, as lines of coloured runs ---------------------------------- */

struct Run {
    std::string Text;
    PainterColor       Ink;
};

enum class LineKind { Words, Rule, Swatches };

struct Line {
    LineKind         Kind  = LineKind::Words;
    PainterColor            Paper = PanelBackground;
    std::vector<Run> Runs;
};

/* The console's sixteen, in its own order. */
constexpr PainterColor Swatches[] = {painter::core::Colors::Black,     painter::core::Colors::DarkBlue, painter::core::Colors::DarkGreen, painter::core::Colors::DarkCyan,
                              painter::core::Colors::DarkRed,  painter::core::Colors::DarkMagenta, painter::core::Colors::Brown,   painter::core::Colors::LightGray,
                              painter::core::Colors::DarkGray, painter::core::Colors::Blue,      painter::core::Colors::Green,      painter::core::Colors::Cyan,
                              painter::core::Colors::Red,       painter::core::Colors::Magenta,   painter::core::Colors::Yellow,     painter::core::Colors::White};
constexpr std::size_t SwatchWidth = 2;

struct ReportSample {
    system_info_t      Info{};
    system_resources_t Before{};
    system_resources_t After{};
    bool               ResourcesKnown = false;
    bool               LoadKnown      = false;
    std::string        Context;
    int                FilesAll    = -1;   /* on the volume; < 0 unknown */
    int                FilesInView = -1;   /* in the Use Context */
};

std::string FixedText(const char *Bytes, std::size_t Capacity)
{
    std::size_t Length = 0;
    while (Length < Capacity && Bytes[Length] != '\0') Length++;
    return std::string(Bytes, Length);
}

/* 3.1 MiB, 64 MiB: a decimal while it says something. */
std::string Size(std::uint64_t Value)
{
    static const char *const Units[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    double Amount = static_cast<double>(Value);
    int    Unit   = 0;
    while (Amount >= 1024.0 && Unit < 4) {
        Amount /= 1024.0;
        Unit++;
    }
    if (Unit == 0) return std::format("{} B", Value);
    return Amount < 10.0 ? std::format("{:.1f} {}", Amount, Units[Unit]) : std::format("{:.0f} {}", Amount, Units[Unit]);
}

std::string Plural(std::uint64_t Count, std::string_view One)
{
    return std::format("{} {}{}", Count, One, Count == 1 ? "" : "s");
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
    if (KiloHertz >= 1000000) return std::format("{}.{:02} GHz", KiloHertz / 1000000, (KiloHertz % 1000000) / 10000);
    if (KiloHertz >= 1000) return std::format("{} MHz", KiloHertz / 1000);
    return std::format("{} kHz", KiloHertz);
}

std::string TickSource(const system_info_t &Info)
{
    switch (Info.tick_source) {
    case SYSINFO_TICK_SOURCE_HPET_LEGACY: return std::format("{} Hz from HPET timer 0", Info.tick_freq_hz);
    case SYSINFO_TICK_SOURCE_8254:        return std::format("{} Hz from the 8254", Info.tick_freq_hz);
    default:                              return "no IRQ0 source";
    }
}

/* The share of the processor that was busy over the sample window, or -1. */
int CpuLoad(const system_resources_t &Before, const system_resources_t &After)
{
    if (After.cpu_total_tsc <= Before.cpu_total_tsc || After.cpu_busy_tsc < Before.cpu_busy_tsc) return -1;
    uint64_t Total = After.cpu_total_tsc - Before.cpu_total_tsc;
    uint64_t Busy  = After.cpu_busy_tsc - Before.cpu_busy_tsc;
    return static_cast<int>(std::min<uint64_t>(100, Busy * 100 / Total));
}

void WaitForSampleWindow(uint32_t Milliseconds)
{
    volatile uint64_t WaitWord __attribute__((aligned(8))) = 0;
    error_t Rc = addr_park(&WaitWord, 0, Milliseconds);
    if (Rc == OK || Rc == ERR_TIMEOUT) return;

    uint64_t Started = clock_uptime_ms();
    while (clock_uptime_ms() - Started < Milliseconds) yield();
}

int CountFiles(bool Everywhere)
{
    file_id_t *Ids   = nullptr;
    int        Count = Everywhere ? query_all_everywhere(box::tagfs::person_tags, nullptr, nullptr, &Ids) : query_all(box::tagfs::person_tags, nullptr, nullptr, &Ids);
    free(Ids);
    return Count;
}

Line Field(std::string_view Label, std::string Value, PainterColor Ink)
{
    std::string Head(Label);
    Head.resize(LabelWidth, ' ');
    return Line{LineKind::Words, PanelBackground, {{std::move(Head), LabelInk}, {std::move(Value), Ink}}};
}

/* A gauge under its label, or under the line above when Label is empty: the
 * share as squares, the rest as dots, the share in figures. */
Line Gauge(std::string_view Label, int Percent)
{
    Percent = std::clamp(Percent, 0, 100);
    std::size_t Lit   = (GaugeWidth * static_cast<std::size_t>(Percent) + 50) / 100;
    PainterColor       Ink   = Percent >= 90 ? GaugeFull : Percent >= 70 ? GaugeTight : GaugeGood;
    std::string Head(Label);
    Head.resize(LabelWidth, ' ');
    return Line{LineKind::Words, PanelBackground,
                {{std::move(Head), LabelInk},
                 {Repeated(Glyph::Filled, Lit), Ink},
                 {Repeated(Glyph::Left, GaugeWidth - Lit), GaugeLeft},
                 {std::format(" {:>3}%", Percent), ValueInk}}};
}

Line Rule()
{
    return Line{LineKind::Rule, PanelBackground, {}};
}

int Share(std::uint64_t Part, std::uint64_t Whole)
{
    return Whole == 0 ? 0 : static_cast<int>(std::min<std::uint64_t>(100, Part * 100 / Whole));
}

/* "code,cpp" as it is said: use code cpp. */
std::string Spoken(std::string_view Tags)
{
    std::string Out;
    for (char Ch : Tags) {
        if (Ch == ',') {
            if (!Out.empty() && Out.back() != ' ') Out += ' ';
        } else if (Ch != ' ' || (!Out.empty() && Out.back() != ' ')) {
            Out += Ch;
        }
    }
    return Out;
}

/* What the person is doing and what holds the machine up for it. */
void AddLife(std::vector<Line> &Card, const ReportSample &Sample)
{
    const system_info_t &Info = Sample.Info;
    Card.push_back(Sample.Context.empty() ? Field("context", "none: the whole volume", QuietInk)
                                          : Field("context", "use " + Spoken(Sample.Context), AccentInk));
    if (Sample.FilesAll >= 0) {
        std::string Files = Plural(static_cast<std::uint64_t>(Sample.FilesAll), "file");
        if (!Sample.Context.empty() && Sample.FilesInView >= 0) Files += std::format(", {} in view", Sample.FilesInView);
        Card.push_back(Field("files", std::move(Files), ValueInk));
    }
    Card.push_back(Field("uptime", Uptime(Info.uptime_ns), ValueInk));
    Card.push_back(Field("processes", std::format("{} live", Info.process_count), ValueInk));
}

void AddGauges(std::vector<Line> &Card, const ReportSample &Sample)
{
    const system_info_t &Info = Sample.Info;
    Card.push_back(Field("memory", std::format("{} of {} in use", Size(Info.used_memory), Size(Info.total_memory)),
                         ValueInk));
    Card.push_back(Gauge("", Share(Info.used_memory, Info.total_memory)));

    const system_resources_t &Volume = Sample.Before;
    if (Sample.ResourcesKnown && Volume.volume_present) {
        std::string Name = FixedText(Volume.volume_name, sizeof(Volume.volume_name));
        if (Name.empty()) Name = "unnamed";
        Card.push_back(Field("volume", std::move(Name), ValueInk));
        if (Volume.volume_free_counted) {
            Card.push_back(Field("", std::format("{} of {} in use", Size(Volume.volume_used_bytes),
                                                 Size(Volume.volume_total_bytes)),
                                 ValueInk));
            Card.push_back(Gauge("", Share(Volume.volume_used_bytes, Volume.volume_total_bytes)));
        } else {
            Card.push_back(Field("", std::format("{}, free not counted", Size(Volume.volume_total_bytes)), ValueInk));
        }
    } else {
        Card.push_back(Field("volume", "none mounted", StatusInk));
    }

    int Load = Sample.LoadKnown ? CpuLoad(Sample.Before, Sample.After) : -1;
    Card.push_back(Load >= 0 ? Gauge("load", Load) : Field("load", "not measured", StatusInk));
}

void AddCores(std::vector<Line> &Card, const system_info_t &Info)
{
    Card.push_back(Field("cpu", std::format("{}: {} kernel, {} app", Plural(Info.cpu_total, "core"), Info.cpu_k_cores,
                                            Info.cpu_app_cores),
                         ValueInk));
    std::string Runtime = Info.multicore_active ? "AMP, cores apart" : "single-core";
    if (Info.has_waitpkg) Runtime += ", UMWAIT";
    Card.push_back(Field("runtime", std::move(Runtime), ValueInk));
    if (Info.tsc_freq_khz == 0) {
        Card.push_back(Field("clock", "TSC not calibrated", StatusInk));
    } else {
        std::string Clock = Frequency(Info.tsc_freq_khz) + " TSC";
        if (!Info.has_invariant_tsc) Clock += ", not invariant";
        Card.push_back(Field("clock", std::move(Clock), Info.has_invariant_tsc ? ValueInk : AccentInk));
    }
    Card.push_back(Field("tick", TickSource(Info), ValueInk));
}

std::vector<Line> MakeCard(const ReportSample &Sample)
{
    std::string Name  = FixedText(Sample.Info.name, sizeof(Sample.Info.name));
    std::string Title = Name;
    Title.resize(ContentWidth - 8, ' ');
    Title += "passport";

    std::vector<Line> Card;
    Card.push_back(Line{LineKind::Words, BubbleDark, {{std::move(Title), TitleInk}}});
    AddLife(Card, Sample);
    Card.push_back(Rule());
    AddGauges(Card, Sample);
    Card.push_back(Rule());
    AddCores(Card, Sample.Info);
    Card.push_back(Rule());
    Card.push_back(Line{LineKind::Swatches, PanelBackground, {}});
    return Card;
}

/* ---- the bubble ----------------------------------------------------------- */

int DistanceSquared(int X, int Y, int CenterX, int CenterY)
{
    int Dx = X - CenterX;
    int Dy = Y - CenterY;
    return Dx * Dx + Dy * Dy;
}

PainterColor SurfaceColor(int Distance, int RadiusSquared)
{
    int Weight = RadiusSquared == 0 ? 255 : (RadiusSquared - Distance) * 255 / RadiusSquared;
    Weight = std::clamp(Weight, 0, 255);
    return PainterColor::Between(BubbleDark, BubblePink, static_cast<uint8_t>(Weight));
}

PainterColor BubblePixel(int X, int Y)
{
    constexpr int Center = 15;
    constexpr int Outer  = 14 * 14;
    constexpr int Ring   = 9 * 9;
    constexpr int Pupil  = 6 * 6;

    int Distance = DistanceSquared(X, Y, Center, Center);
    if (Distance > Outer) return Screen;

    if (Distance <= Pupil) {
        if (X < 13 && Y < 13 && Distance > 15)
            return PainterColor::Between(BubbleBlack, PupilGlow, 90);
        return BubbleBlack;
    }

    if (Distance <= Ring) {
        int Weight = (Ring - Distance) * 255 / (Ring - Pupil);
        return PainterColor::Between(BubbleMid, BubbleLight, static_cast<uint8_t>(std::clamp(Weight, 0, 255)));
    }
    return SurfaceColor(Distance - Ring, Outer - Ring);
}

PainterColor SatellitePixel(int X, int Y, int CenterX, int CenterY, int Radius)
{
    int RadiusSquared = Radius * Radius;
    int Distance      = DistanceSquared(X, Y, CenterX, CenterY);
    return Distance <= RadiusSquared ? SurfaceColor(Distance, RadiusSquared) : Screen;
}

PainterColor LogoPixel(int X, int Y)
{
    PainterColor Main = BubblePixel(X, Y);
    if (Main != Screen) return Main;

    PainterColor TopLeft = SatellitePixel(X, Y, 3, 4, 3);
    if (TopLeft != Screen) return TopLeft;
    PainterColor Right = SatellitePixel(X, Y, 30, 12, 2);
    if (Right != Screen) return Right;
    return SatellitePixel(X, Y, 27, 28, 4);
}

std::string TextLogoLine(std::size_t Row)
{
    std::string Line;
    Line.reserve(LogoPixelWidth);
    for (std::size_t Col = 0; Col < LogoPixelWidth; Col++) {
        PainterColor Top    = LogoPixel(static_cast<int>(Col), static_cast<int>(Row * 2));
        PainterColor Bottom = LogoPixel(static_cast<int>(Col), static_cast<int>(Row * 2 + 1));
        if (Top == Screen && Bottom == Screen) Line += ' ';
        else if (Top == BubbleBlack || Bottom == BubbleBlack) Line += '@';
        else if (Top == BubbleLight || Bottom == BubbleLight) Line += 'o';
        else Line += '#';
    }
    return Line;
}

/* ---- the portrait on the glass -------------------------------------------- */

class Portrait {
public:
    Portrait(std::size_t Rows, std::size_t Cols, painter::tui::Frame FrameValue)
        : Rows(Rows), Cols(Cols), FrameBuffer(std::move(FrameValue))
    {
    }

    void Logo(std::size_t Top);
    void Card(std::size_t Top, const std::vector<Line> &Lines);
    const painter::tui::Frame &Canvas() const { return FrameBuffer; }

private:
    void Put(std::size_t Row, std::size_t Col, char32_t Ch, PainterColor Ink, PainterColor Paper)
    {
        if (Row >= Rows || Col >= Cols) return;
        FrameBuffer.Put({static_cast<std::int32_t>(Col), static_cast<std::int32_t>(Row)},
                        painter::tui::Cell{Ch, Ink, Paper});
    }

    void Edge(std::size_t Row, char32_t LeftCorner, char32_t Middle, char32_t RightCorner);
    void Words(std::size_t Row, const Line &One);
    void Strip(std::size_t Row);

    std::size_t           Rows, Cols;
    painter::tui::Frame FrameBuffer;
};

void Portrait::Logo(std::size_t Top)
{
    for (std::size_t PixelRow = 0; PixelRow < LogoPixelHeight; PixelRow += 2) {
        for (std::size_t PixelCol = 0; PixelCol < LogoPixelWidth; PixelCol++) {
            PainterColor Upper = LogoPixel(static_cast<int>(PixelCol), static_cast<int>(PixelRow));
            PainterColor Lower = LogoPixel(static_cast<int>(PixelCol), static_cast<int>(PixelRow + 1));
            std::size_t Row = Top + PixelRow / 2;
            std::size_t Col = LogoOriginX + PixelCol;
            if (Upper == Screen && Lower == Screen) Put(Row, Col, ' ', Screen, Screen);
            else if (Upper == Screen) Put(Row, Col, Glyph::LowerHalf, Lower, Screen);
            else Put(Row, Col, Glyph::UpperHalf, Upper, Lower);
        }
    }
}

void Portrait::Edge(std::size_t Row, char32_t LeftCorner, char32_t Middle, char32_t RightCorner)
{
    Put(Row, PanelOriginX, LeftCorner, FrameInk, PanelBackground);
    for (std::size_t Col = 1; Col + 1 < PanelWidth; Col++) Put(Row, PanelOriginX + Col, Middle, FrameInk, PanelBackground);
    Put(Row, PanelOriginX + PanelWidth - 1, RightCorner, FrameInk, PanelBackground);
}

void Portrait::Words(std::size_t Row, const Line &One)
{
    Put(Row, PanelOriginX, Glyph::Down, FrameInk, PanelBackground);
    for (std::size_t Col = 1; Col + 1 < PanelWidth; Col++) Put(Row, PanelOriginX + Col, ' ', ValueInk, One.Paper);
    Put(Row, PanelOriginX + PanelWidth - 1, Glyph::Down, FrameInk, PanelBackground);

    std::size_t Col = 0;
    for (const Run &Piece : One.Runs) {
        for (std::size_t Byte = 0; Byte < Piece.Text.size();) {
            if (Col >= ContentWidth) return;
            const painter::tui::Glyph Shape = painter::tui::GlyphAt(Piece.Text, Byte);
            Put(Row, PanelOriginX + 2 + Col++, Shape.Odd ? U'?' : Shape.Character, Piece.Ink, One.Paper);
            Byte += Shape.Length;
        }
    }
}

void Portrait::Strip(std::size_t Row)
{
    Words(Row, Line{});
    std::size_t Wide = SwatchWidth * (sizeof(Swatches) / sizeof(Swatches[0]));
    std::size_t Col  = PanelOriginX + 1 + (PanelInner - Wide) / 2;
    for (PainterColor Swatch : Swatches)
        for (std::size_t At = 0; At < SwatchWidth; At++) Put(Row, Col++, ' ', Swatch, Swatch);
}

void Portrait::Card(std::size_t Top, const std::vector<Line> &Lines)
{
    Edge(Top, Glyph::TopLeft, Glyph::Across, Glyph::TopRight);
    for (std::size_t At = 0; At < Lines.size(); At++) {
        std::size_t Row = Top + 1 + At;
        switch (Lines[At].Kind) {
        case LineKind::Rule:     Edge(Row, Glyph::TeeLeft, Glyph::Across, Glyph::TeeRight); break;
        case LineKind::Swatches: Strip(Row); break;
        case LineKind::Words:    Words(Row, Lines[At]); break;
        }
    }
    Edge(Top + 1 + Lines.size(), Glyph::LowLeft, Glyph::Across, Glyph::LowRight);
}

std::size_t LogoTop(std::size_t CardRows)
{
    return TopMargin + (CardRows > LogoCellRows ? (CardRows - LogoCellRows) / 2 : 0);
}

bool PaintReport(const std::vector<Line> &Lines)
{
    std::size_t CardRows   = Lines.size() + 2;
    std::size_t ReportRows = TopMargin + std::max(LogoCellRows, CardRows);
    constexpr std::size_t PromptRows = 1;
    std::expected<painter::tui::ConsoleSurface, painter::tui::StageError> Opened =
        painter::tui::ConsoleSurface::Open(MinimumCols, static_cast<std::int32_t>(ReportRows + PromptRows));
    if (!Opened) {
        print_to_screen((Opened.error().Describe("passport") + '\n').c_str());
        return false;
    }
    painter::tui::ConsoleSurface Output = std::move(*Opened);
    const painter::core::Size Dimensions = Output.Size();

    std::expected<painter::core::Point, box::error> Cursor = Output.Cursor();
    if (!Cursor) return false;
    std::size_t PaintTop = static_cast<std::size_t>(Cursor->Y);
    if (PaintTop + ReportRows + PromptRows > static_cast<std::size_t>(Dimensions.Height)) {
        std::size_t BottomTop = static_cast<std::size_t>(Dimensions.Height) - ReportRows - PromptRows;
        for (std::size_t Scrolls = PaintTop - BottomTop; Scrolls > 0; Scrolls--) {
            if (!Output.ScrollUp()) {
                print_to_screen("passport: the screen would not scroll for its portrait\n");
                return false;
            }
        }
        PaintTop = BottomTop;
    }

    std::expected<painter::tui::Frame, box::error> Created =
        painter::tui::Frame::Create({static_cast<std::int32_t>(MinimumCols), static_cast<std::int32_t>(ReportRows)});
    if (!Created) return false;
    Created->Clear({Screen, Screen});
    Portrait Picture(ReportRows, MinimumCols, std::move(*Created));
    Picture.Logo(LogoTop(CardRows));
    Picture.Card(TopMargin, Lines);

    if (!Output.Paint({0, static_cast<std::int32_t>(PaintTop)}, Picture.Canvas())) return false;
    if (!Output.SetCursor({0, static_cast<std::int32_t>(PaintTop + ReportRows)})) return false;
    return static_cast<bool>(Output.Commit());
}

/* ---- the portrait as text ------------------------------------------------- */

/* A line of the card in plain characters: squares and dots are # and . */
std::string TextCardLine(const Line &One)
{
    if (One.Kind == LineKind::Rule) return "+" + std::string(PanelInner, '-') + "+";
    std::string Content;
    for (const Run &Piece : One.Runs) {
        for (std::size_t Byte = 0; Byte < Piece.Text.size();) {
            const painter::tui::Glyph Shape = painter::tui::GlyphAt(Piece.Text, Byte);
            if (Shape.Character == Glyph::Filled) Content += '#';
            else if (Shape.Character == Glyph::Left) Content += '.';
            else if (Shape.Odd) Content += '?';
            else Content.append(Piece.Text, Byte, Shape.Length);
            Byte += Shape.Length;
        }
    }
    Content.resize(painter::tui::BytesWithin(Content, ContentWidth));
    Content.append(ContentWidth - painter::tui::CellsOf(Content), ' ');
    return "| " + Content + " |";
}

std::string TextReport(const std::vector<Line> &Lines)
{
    std::vector<std::string> Card;
    Card.push_back("+" + std::string(PanelInner, '-') + "+");
    /* The colours are no text, and neither is the rule over them. */
    for (std::size_t At = 0; At < Lines.size(); At++) {
        bool OverSwatches = At + 1 < Lines.size() && Lines[At + 1].Kind == LineKind::Swatches;
        if (Lines[At].Kind == LineKind::Swatches || (Lines[At].Kind == LineKind::Rule && OverSwatches)) continue;
        Card.push_back(TextCardLine(Lines[At]));
    }
    Card.push_back(Card.front());

    std::size_t LogoAt = LogoTop(Card.size()) - TopMargin;
    /* No margin before the bubble: a line as wide as an 80-column console
     * would wrap there and leave a blank line after every one. */
    std::string Output;
    for (std::size_t Row = 0; Row < std::max(LogoAt + LogoCellRows, Card.size()); Row++) {
        bool Logo = Row >= LogoAt && Row < LogoAt + LogoCellRows;
        Output += Logo ? TextLogoLine(Row - LogoAt) : std::string(LogoPixelWidth, ' ');
        Output.append(PanelOriginX - LogoOriginX - LogoPixelWidth, ' ');
        if (Row < Card.size()) Output += Card[Row];
        Output += '\n';
    }
    return Output;
}

int Portray()
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
    Sample.Context  = KeepContext();
    Sample.FilesAll = CountFiles(true);
    if (!Sample.Context.empty()) Sample.FilesInView = CountFiles(false);

    std::vector<Line> Lines = MakeCard(Sample);
    if (form_screen_file() != 0) {
        std::string Text = TextReport(Lines);
        print_bytes(Text.data(), Text.size());
        return 0;
    }
    return PaintReport(Lines) ? 0 : 1;
}

} // namespace

int main()
{
    try {
        return Portray();
    } catch (const std::bad_alloc &) {
        print_to_screen("passport: there is not enough memory for its portrait\n");
        return 1;
    }
}
