/* passport — a BoxOS machine portrait.
 *
 * The bubble on the left, and beside it a card of what this machine is and
 * what it is doing: the Use Context first, then memory, the volume and the
 * load as gauges, then the cores and their clocks, then the console's sixteen
 * colours. The card is built once as lines of coloured runs; the screen path
 * paints them into one TextCell frame and hands it to the glass with one
 * vga_paint, and under an arrow the same lines are written as text, because a
 * painted frame is deliberately not part of the console text stream.
 *
 * The logo is a two-pixel-high raster per console cell: the CP437 upper-half
 * block carries one colour in its foreground and one in its background. */

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
#include "box/file.h"
#include "box/form.h"
#include "box/memory.h"
#include "box/print.h"
#include "box/sync.h"
#include "box/system.h"
#include "box/vga.h"

#include "keep.h"

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
constexpr Color QuietInk        = COLOR_RGB(0x8C, 0x8C, 0x8C);
constexpr Color AccentInk       = BubblePink;
constexpr Color StatusInk       = BubbleMid;
constexpr Color FrameInk        = BubbleMid;

/* A gauge is green while there is room, amber when it is getting full, red
 * when it nearly is; what is left of it is dots. */
constexpr Color GaugeGood  = COLOR_RGB(0x50, 0xD0, 0x60);
constexpr Color GaugeTight = COLOR_AMBER;
constexpr Color GaugeFull  = COLOR_RGB(0xFF, 0x50, 0x50);
constexpr Color GaugeLeft  = COLOR_RGB(0x4A, 0x4A, 0x4A);

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
constexpr char UpperHalf = static_cast<char>(0xDF);
constexpr char Across    = static_cast<char>(0xC4);
constexpr char Down      = static_cast<char>(0xB3);
constexpr char TopLeft   = static_cast<char>(0xDA);
constexpr char TopRight  = static_cast<char>(0xBF);
constexpr char LowLeft   = static_cast<char>(0xC0);
constexpr char LowRight  = static_cast<char>(0xD9);
constexpr char TeeLeft   = static_cast<char>(0xC3);
constexpr char TeeRight  = static_cast<char>(0xB4);
constexpr char Filled    = static_cast<char>(0xFE);
constexpr char Left      = static_cast<char>(0xFA);
}

/* ---- the card, as lines of coloured runs ---------------------------------- */

struct Run {
    std::string Text;
    Color       Ink;
};

enum class LineKind { Words, Rule, Swatches };

struct Line {
    LineKind         Kind  = LineKind::Words;
    Color            Paper = PanelBackground;
    std::vector<Run> Runs;
};

/* The console's sixteen, in its own order. */
constexpr Color Swatches[] = {COLOR_BLACK,     COLOR_DARK_BLUE, COLOR_DARK_GREEN, COLOR_DARK_CYAN,
                              COLOR_DARK_RED,  COLOR_DARK_MAGENTA, COLOR_BROWN,   COLOR_LIGHT_GRAY,
                              COLOR_DARK_GRAY, COLOR_BLUE,      COLOR_GREEN,      COLOR_CYAN,
                              COLOR_RED,       COLOR_MAGENTA,   COLOR_YELLOW,     COLOR_WHITE};
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
    int        Count = Everywhere ? query_all_everywhere(nullptr, &Ids) : query_all(nullptr, &Ids);
    free(Ids);
    return Count;
}

Line Field(std::string_view Label, std::string Value, Color Ink)
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
    Color       Ink   = Percent >= 90 ? GaugeFull : Percent >= 70 ? GaugeTight : GaugeGood;
    std::string Head(Label);
    Head.resize(LabelWidth, ' ');
    return Line{LineKind::Words, PanelBackground,
                {{std::move(Head), LabelInk},
                 {std::string(Lit, Glyph::Filled), Ink},
                 {std::string(GaugeWidth - Lit, Glyph::Left), GaugeLeft},
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
    std::string Name  = FixedText(Sample.Info.version, sizeof(Sample.Info.version));
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

std::string TextLogoLine(std::size_t Row)
{
    std::string Line;
    Line.reserve(LogoPixelWidth);
    for (std::size_t Col = 0; Col < LogoPixelWidth; Col++) {
        Color Top    = LogoPixel(static_cast<int>(Col), static_cast<int>(Row * 2));
        Color Bottom = LogoPixel(static_cast<int>(Col), static_cast<int>(Row * 2 + 1));
        if (Top == COLOR_BLACK && Bottom == COLOR_BLACK) Line += ' ';
        else if (Top == BubbleBlack || Bottom == BubbleBlack) Line += '@';
        else if (Top == BubbleLight || Bottom == BubbleLight) Line += 'o';
        else Line += '#';
    }
    return Line;
}

/* ---- the portrait on the glass -------------------------------------------- */

class Portrait {
public:
    Portrait(std::size_t Rows, std::size_t Cols) : Rows(Rows), Cols(Cols), Frame(Rows * Cols, Blank()) {}

    void Logo(std::size_t Top);
    void Card(std::size_t Top, const std::vector<Line> &Lines);
    const TextCell *Cells() const { return Frame.data(); }

private:
    static TextCell Blank()
    {
        TextCell Cell{};
        Cell.ch = ' ';
        Cell.fg = ValueInk;
        Cell.bg = COLOR_BLACK;
        return Cell;
    }

    void Put(std::size_t Row, std::size_t Col, char Ch, Color Ink, Color Paper)
    {
        if (Row >= Rows || Col >= Cols) return;
        TextCell &Cell = Frame[Row * Cols + Col];
        Cell.ch = Ch;
        Cell.fg = Ink;
        Cell.bg = Paper;
    }

    void Edge(std::size_t Row, char LeftCorner, char Middle, char RightCorner);
    void Words(std::size_t Row, const Line &One);
    void Strip(std::size_t Row);

    std::size_t           Rows, Cols;
    std::vector<TextCell> Frame;
};

void Portrait::Logo(std::size_t Top)
{
    for (std::size_t PixelRow = 0; PixelRow < LogoPixelHeight; PixelRow += 2) {
        for (std::size_t PixelCol = 0; PixelCol < LogoPixelWidth; PixelCol++) {
            Color Upper = LogoPixel(static_cast<int>(PixelCol), static_cast<int>(PixelRow));
            Color Lower = LogoPixel(static_cast<int>(PixelCol), static_cast<int>(PixelRow + 1));
            char  Ch    = Upper == COLOR_BLACK && Lower == COLOR_BLACK ? ' ' : Glyph::UpperHalf;
            Put(Top + PixelRow / 2, LogoOriginX + PixelCol, Ch, Upper, Lower);
        }
    }
}

void Portrait::Edge(std::size_t Row, char LeftCorner, char Middle, char RightCorner)
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
        for (char Ch : Piece.Text) {
            if (Col >= ContentWidth) return;
            unsigned char Byte = static_cast<unsigned char>(Ch);
            char Shown = Byte >= 0x20 && Byte != 0x7F ? Ch : '?';
            Put(Row, PanelOriginX + 2 + Col++, Shown, Piece.Ink, One.Paper);
        }
    }
}

void Portrait::Strip(std::size_t Row)
{
    Words(Row, Line{});
    std::size_t Wide = SwatchWidth * (sizeof(Swatches) / sizeof(Swatches[0]));
    std::size_t Col  = PanelOriginX + 1 + (PanelInner - Wide) / 2;
    for (Color Swatch : Swatches)
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
    vga_dimensions_t Dimensions{};
    if (vga_getdimensions(&Dimensions) != 0) {
        print_to_screen("passport: this screen would not say how big it is\n");
        return false;
    }
    std::size_t CardRows   = Lines.size() + 2;
    std::size_t ReportRows = TopMargin + std::max(LogoCellRows, CardRows);
    constexpr std::size_t PromptRows = 1;
    if (Dimensions.cols < MinimumCols || Dimensions.rows < ReportRows + PromptRows) {
        print_to_screen(std::format("passport: screen is {} columns by {} rows; needs at least {} by {}\n",
                                    Dimensions.cols, Dimensions.rows, MinimumCols, ReportRows + PromptRows).c_str());
        return false;
    }

    vga_pos_t Cursor{};
    (void)vga_getcursor(&Cursor);
    std::size_t PaintTop = Cursor.row;
    if (PaintTop + ReportRows + PromptRows > Dimensions.rows) {
        std::size_t BottomTop = Dimensions.rows - ReportRows - PromptRows;
        for (std::size_t Scrolls = PaintTop - BottomTop; Scrolls > 0; Scrolls--) {
            if (vga_scroll_up() != 0) {
                print_to_screen("passport: the screen would not scroll for its portrait\n");
                return false;
            }
        }
        PaintTop = BottomTop;
    }

    Portrait Picture(ReportRows, MinimumCols);
    Picture.Logo(LogoTop(CardRows));
    Picture.Card(TopMargin, Lines);

    vga_setcursor(static_cast<uint8_t>(PaintTop + ReportRows), 0);
    return vga_paint(static_cast<uint8_t>(PaintTop), 0, static_cast<uint8_t>(ReportRows),
                     static_cast<uint8_t>(MinimumCols), Picture.Cells()) == 0;
}

/* ---- the portrait as text ------------------------------------------------- */

/* A line of the card in plain characters: squares and dots are # and . */
std::string TextCardLine(const Line &One)
{
    if (One.Kind == LineKind::Rule) return "+" + std::string(PanelInner, '-') + "+";
    std::string Content;
    for (const Run &Piece : One.Runs) {
        for (char Ch : Piece.Text) {
            unsigned char Byte = static_cast<unsigned char>(Ch);
            Content += Ch == Glyph::Filled ? '#' : Ch == Glyph::Left ? '.' : Byte >= 0x20 && Byte < 0x7F ? Ch : '?';
        }
    }
    Content.resize(ContentWidth, ' ');
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
