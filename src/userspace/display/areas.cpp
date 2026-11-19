/*
 * areas.cpp — the display's side of the items' language: programs' areas and
 * blocks, their questions, messages and events, and each frame of them. See
 * areas.h.
 */

#include "areas.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cuchar>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "box/clock.h"
#include "box/color.h"
#include "box/core/cabin.h"
#include "box/cxx/bay.h"
#include "box/debug.h"
#include "box/error.h"
#include "box/ipc.h"
#include "box/touch.h"

#include "front.h"
#include "input.h"
#include "item_reception.h"
#include "measure.h"
#include "painter/core/system_fonts.h"

namespace {

using painter::core::Rect;

/* The names of shared memory a family of things is made under — "display:
 * area:N" — reused: the volume keeps every tag it is told, so it learns as
 * many as were ever held at once, not one for every area opened. The pool has
 * room for every number there is, made when the number is, so giving one
 * back never allocates. */
class Names {
public:
    explicit Names(const char *Prefix) noexcept : Prefix(Prefix) {}

    // Memory of Bytes made for a program alone under a free name, its number
    // in Number. A pooled name whose memory a program still holds is passed
    // over and kept for later; so is a fresh one somebody else made first.
    // Throws std::bad_alloc only.
    std::expected<box::bay<std::byte>, box::error> Make(std::size_t Bytes, const AskFrom &For, std::uint32_t &Number)
    {
        const auto Try = [&](std::uint32_t Candidate) {
            char Tag[DISPLAY_ITEM_TAG_MAX];
            Spell(Candidate, Tag);
            return box::bay<std::byte>::create_for(Tag, Bytes, For.program_pid, For.program_generation);
        };
        for (std::size_t I = Free.size(); I-- > 0;) {
            auto Made = Try(Free[I]);
            if (Made) {
                Number = Free[I];
                Free.erase(Free.begin() + static_cast<std::ptrdiff_t>(I));
                return Made;
            }
            if (Made.error().raw() != ERR_ALREADY_EXISTS) return Made;
        }
        for (;;) {
            const std::uint32_t Fresh = Next;
            Free.reserve(static_cast<std::size_t>(Fresh) + 1u);
            Next++;
            auto Made = Try(Fresh);
            if (Made) {
                Number = Fresh;
                return Made;
            }
            Free.push_back(Fresh);
            if (Made.error().raw() != ERR_ALREADY_EXISTS) return Made;
        }
    }
    void Give(std::uint32_t Number) noexcept { Free.push_back(Number); }
    void Spell(std::uint32_t Number, char (&Into)[DISPLAY_ITEM_TAG_MAX]) const noexcept
    {
        std::snprintf(Into, sizeof(Into), "%s%u", Prefix, Number);
    }

private:
    const char                *Prefix;
    std::vector<std::uint32_t> Free;
    std::uint32_t              Next = 0;
};

struct Block {
    std::uint32_t                                 Id     = 0;
    std::uint8_t                                  Kind   = 0;
    std::uint32_t                                 Width  = 0, Height = 0, Bytes = 0;
    std::uint32_t                                 Number = 0;
    box::bay<std::byte>                           Memory;
    std::shared_ptr<const painter::core::Picture> Picture;          // a picture's, made when it is said changed
    std::optional<painter::core::Canvas>          Shown;            // a canvas's pages as shown, kept here
    bool                                          Read = false;     // a font's, read into its program's book

    const std::uint8_t *Content() const noexcept
    {
        return reinterpret_cast<const std::uint8_t *>(Memory.data()) + sizeof(ItemBlockHeader);
    }
    ItemBlockHeader *Head() noexcept { return reinterpret_cast<ItemBlockHeader *>(Memory.data()); }
};

struct Program {
    std::uint32_t                       Pid = 0, Generation = 0;   // its founding strand's: the kernel's name for it
    items::ProgramState                 State;
    typecase::Fonts                     Book;                      // the system's faces and its own
    bool                                SystemsIn = false;         // the system's faces put in it
    std::vector<std::unique_ptr<Block>> Blocks;
    std::size_t                         BlockBytes = 0;
    std::size_t                         FontBytes  = 0;            // its own faces read, kept past their blocks
    std::uint32_t                       Areas      = 0;

    Block *Find(std::uint32_t Id) noexcept
    {
        for (const auto &Each : Blocks)
            if (Each->Id == Id) return Each.get();
        return nullptr;
    }
};

struct Area {
    std::uint32_t                     Id = 0;
    std::uint32_t                     OwnerPid = 0, OwnerGeneration = 0;   // the strand that speaks for it
    Program                          *Of = nullptr;
    std::uint8_t                      Placement = DISPLAY_AREA_OVER;
    bool                              Always    = false;   // shown even while its program lives behind
    ItemPoint                         Wish{};
    std::uint64_t                     Line      = 0;       // in the flow: its first row of the console, as RowsGone counts
    std::uint32_t                     Number = 0;
    box::bay<std::byte>               Memory;
    ItemAreaHeader                   *Head = nullptr;
    std::unique_ptr<items::AreaState> State;
    TouchTagPair                      Bell{};
    bool                              Rung    = false;   // events added since its bell last rang
    bool                              Stepped = false;   // a step taken since it was last told one was shown
    std::uint64_t                     Step    = 0;
    std::uint32_t                     Bytes = 0, Records = 0;

    std::span<const std::uint8_t> Ring() const noexcept
    {
        return {reinterpret_cast<const std::uint8_t *>(Memory.data()) + sizeof(ItemAreaHeader), ITEM_RING_BYTES};
    }
    std::span<std::uint8_t> Events() noexcept
    {
        return {reinterpret_cast<std::uint8_t *>(Memory.data()) + sizeof(ItemAreaHeader) + ITEM_RING_BYTES, ITEM_EVENT_BYTES};
    }
};

/* The largest side of a picture or canvas a program may give. */
constexpr std::uint32_t BlockSideMost = 16384;

std::vector<std::unique_ptr<Program>> g_programs;
std::vector<std::unique_ptr<Area>>    g_areas;     // in the order they opened
Names                                 g_area_names(DISPLAY_AREA_TAG_PREFIX);
Names                                 g_block_names(DISPLAY_BLOCK_TAG_PREFIX);
std::uint32_t                         g_next_area = 1, g_next_block = 1;
AreasGlass                            g_glass;
CellsLook                             g_no_cells;      // the look until the glass is known
items::Theme                          g_no_theme;
std::uint32_t                         g_scale     = 256;
std::uint64_t                         g_rows_gone = 0;
bool                                  g_left      = false;   // an area went: the frame looks again
items::Motion                         g_motion;                // how the person wants things to move

float Scale() noexcept { return static_cast<float>(g_scale) / 256.0f; }

/* An event told to an area; its bell rung by Ring. */
void Tell(Area &To, std::uint16_t Kind, std::uint32_t Item, const void *Payload, std::uint32_t Bytes) noexcept
{
    if (items::AddEvent(*To.Head, To.Events(), Kind, Item, Payload, Bytes)) To.Rung = true;
}

void Ring(Area &Of) noexcept
{
    if (!Of.Rung) return;
    Of.Rung = false;
    const std::uint32_t Id = Of.Id;
    (void)touch_send(Of.Bell, &Id, sizeof(Id), 0);
}

/* How the input part tells an area: an event, its bell rung at once. */
InputTeller TellerOf(Area &Of)
{
    Area *To = &Of;
    return [To](std::uint16_t Kind, std::uint32_t Item, const void *Payload, std::uint32_t Bytes) {
        Tell(*To, Kind, Item, Payload, Bytes);
        Ring(*To);
    };
}

const CellsLook    &Cells() noexcept { return g_glass.Cells ? *g_glass.Cells : g_no_cells; }
const items::Theme &Look() noexcept { return g_glass.Look ? *g_glass.Look : g_no_theme; }

/* An area's height in pixels, as wished: a part of the screen when none is
 * wished, no taller than the screen — no taller than the console in its flow. */
std::int32_t HeightOf(const Area &Of) noexcept
{
    const Rect         Screen = g_glass.Screen;
    const std::int32_t Most   = Of.Placement == DISPLAY_AREA_FLOW ? Cells().Grid.Height - Cells().Height : Screen.Height;
    const std::int32_t Wished = Of.Wish.Y ? static_cast<std::int32_t>(std::lround(static_cast<float>(Of.Wish.Y) / ITEM_POINT * Scale()))
                                          : Screen.Height * 3 / 5;
    return std::clamp(Wished, 1, std::max(Most, 1));
}

/* The rows of the console an area in its flow takes. */
std::uint32_t RowsOf(const Area &Of) noexcept
{
    return static_cast<std::uint32_t>((HeightOf(Of) + Cells().Height - 1) / Cells().Height);
}

/* Where an area lies on the glass, in pixels (a text page's eighths and
 * sixteenths of a cell): the display's choice. */
Rect PlaceOf(const Area &Of) noexcept
{
    const Rect Screen = g_glass.Screen;
    if (Of.Placement == DISPLAY_AREA_WHOLE) return Screen;
    const std::int32_t H = HeightOf(Of);
    if (Of.Placement == DISPLAY_AREA_FLOW) {
        /* On its rows of the console, wherever they are now: above the glass
         * once they have gone up off it. */
        const Rect         Grid = Cells().Grid;
        const std::int64_t Row  = static_cast<std::int64_t>(Of.Line) - static_cast<std::int64_t>(g_rows_gone);
        const std::int64_t Top  = std::clamp<std::int64_t>(Grid.Y + Row * Cells().Height, INT32_MIN / 2, INT32_MAX / 2);
        return Rect{Grid.X, static_cast<std::int32_t>(Top), Grid.Width, H};
    }
    const std::int32_t W =
        std::clamp(Of.Wish.X ? static_cast<std::int32_t>(std::lround(static_cast<float>(Of.Wish.X) / ITEM_POINT * Scale()))
                             : Screen.Width * 3 / 5,
                   1, Screen.Width);
    return Rect{(Screen.Width - W) / 2, (Screen.Height - H) / 2, W, H};
}

bool ShownNow(const Area &Of) noexcept { return Of.Always || !FrontBehind(Of.Of->Pid, Of.Of->Generation); }

ItemAreaNews NewsOf(const Area &Of) noexcept
{
    const Rect  Place = Of.State->Place;
    const float S     = Scale();
    return ItemAreaNews{static_cast<std::int32_t>(std::lround(static_cast<float>(Place.Width) / S * ITEM_POINT)),
                        static_cast<std::int32_t>(std::lround(static_cast<float>(Place.Height) / S * ITEM_POINT)),
                        g_scale, static_cast<std::uint8_t>(g_glass.Text ? ITEM_GLASS_TEXT : ITEM_GLASS_PIXELS),
                        static_cast<std::uint8_t>(Of.State->Shown), g_glass.Mosaics,
                        static_cast<std::uint8_t>(g_motion.Less ? ITEM_MOTION_LESS : 0), g_motion.Speed, 0};
}

/* A program's book, the system's faces put in it first — when it first lays a
 * text or reads a font of its own, not before: a program of cells needs none.
 * The faces were read as the areas started (AreasStart). Throws
 * std::bad_alloc only. */
typecase::Fonts &BookOf(Program &Of)
{
    if (!Of.SystemsIn) {
        if (const std::shared_ptr<const typecase::Fonts> System = painter::core::SystemFonts()) {
            for (const auto &Face : System->Faces()) Of.Book.Add(Face);
            Of.Book.SetFallback(System->Fallbacks());
            Of.Book.SetDefault(System->Default());
        }
        Of.SystemsIn = true;
    }
    return Of.Book;
}

/* The program From names, made when it has none yet: its fonts the system's
 * and its own, its pictures and canvases its blocks. Throws std::bad_alloc
 * only. */
Program &ProgramOf(const AskFrom &From)
{
    for (const auto &Each : g_programs)
        if (Each->Pid == From.program_pid && Each->Generation == From.program_generation) return *Each;
    auto Made        = std::make_unique<Program>();
    Made->Pid        = From.program_pid;
    Made->Generation = From.program_generation;
    Program *Of                = Made.get();
    Made->State.From.Look      = &Look();
    Made->State.From.Scale     = Scale();
    Made->State.From.Book      = [Of]() -> const typecase::Fonts & { return BookOf(*Of); };
    Made->State.From.Cells     = &Cells();
    Made->State.From.PictureOf = [Of](std::uint32_t Id) -> std::shared_ptr<const painter::core::Picture> {
        const Block *Found = Of->Find(Id);
        return Found ? Found->Picture : nullptr;
    };
    Made->State.From.CanvasOf = [Of](std::uint32_t Id) -> const painter::core::Canvas * {
        const Block *Found = Of->Find(Id);
        return Found && Found->Shown ? &*Found->Shown : nullptr;
    };
    g_programs.push_back(std::move(Made));
    return *g_programs.back();
}

/* A program that holds nothing any more, forgotten — its own fonts with it. */
void Forget(Program &Of) noexcept
{
    if (Of.Areas || !Of.Blocks.empty()) return;
    for (auto It = g_programs.begin(); It != g_programs.end(); ++It)
        if (It->get() == &Of) {
            g_programs.erase(It);
            return;
        }
}

template <typename Work> void ForEachArea(const Program &Of, Work &&Each)
{
    for (const auto &One : g_areas)
        if (One->Of == &Of) Each(*One);
}

/* A canvas block's page, as its program said it changed: the pixels it names
 * taken into the display's own copy, the page given back. A pixel whose light
 * passes its opacity is no canvas pixel: its light is held to it. */
void TakePage(Block &Of, const ItemBlockChanged &Where)
{
    const Clocked       Spent{Part::Intake};
    const std::uint32_t Page = Where.Page & 1u;
    if (!Of.Shown) Of.Shown = painter::core::Canvas::Make(Of.Width, Of.Height);
    if (Of.Shown) {
        const Rect  Whole{0, 0, static_cast<std::int32_t>(Of.Width), static_cast<std::int32_t>(Of.Height)};
        const Rect  Taken  = Rect{Where.X, Where.Y, Where.Width, Where.Height}.Intersect(Whole);
        const auto *Pixels = reinterpret_cast<const painter::core::Pixel *>(Of.Content() + static_cast<std::size_t>(Page) * Of.Head()->PageBytes);
        for (std::int32_t Y = Taken.Y; Y < Taken.Y + Taken.Height; Y++) {
            const painter::core::Pixel *From = Pixels + static_cast<std::size_t>(Y) * Of.Width + static_cast<std::size_t>(Taken.X);
            painter::core::Pixel       *Into = Of.Shown->Row(static_cast<std::uint32_t>(Y)) + Taken.X;
            for (std::int32_t X = 0; X < Taken.Width; X++) {
                painter::core::Pixel P = From[X];
                P.Red = std::min(P.Red, P.Alpha), P.Green = std::min(P.Green, P.Alpha), P.Blue = std::min(P.Blue, P.Alpha);
                Into[X] = P;
            }
        }
    }
    __atomic_store_n(&Of.Head()->PageFree[Page], 1u, __ATOMIC_RELEASE);
}

/* A font block read into its program's book, once: why not, when it is no
 * font or past what the program may keep. */
error_t ReadFont(Program &Of, Block &Font)
{
    if (Font.Read) return OK;
    if (Of.FontBytes + Font.Bytes > ITEM_BLOCKS_BYTES_MOST) return ERR_QUOTA_EXCEEDED;
    std::expected<typecase::Font, typecase::Fault> Face =
        typecase::Font::Read(std::vector<std::uint8_t>(Font.Content(), Font.Content() + Font.Bytes));
    if (!Face) return ERR_CORRUPTED;
    BookOf(Of).Add(std::move(*Face));
    Of.FontBytes += Font.Bytes;
    Font.Read = true;
    return OK;
}

/* A block's content changed, as its program said in a step of Said's: a
 * picture made again, a canvas's page taken and given back, a font read —
 * and every item showing it drawn again. Throws std::bad_alloc only. */
void BlockChanged(Area &Said, std::uint32_t Id, const ItemBlockChanged &Where)
{
    Program   &Of     = *Said.Of;
    Block     *Found  = Of.Find(Id);
    const auto Refuse = [&](error_t Why) {
        const ItemRefusal Refusal{ITEM_OP_BLOCK, 0, static_cast<std::int32_t>(Why)};
        Tell(Said, ITEM_EVENT_REFUSED, Id, &Refusal, sizeof(Refusal));
    };
    if (!Found) return Refuse(ERR_OBJECT_NOT_FOUND);
    switch (Found->Kind) {
    case ITEM_BLOCK_PICTURE:
        Found->Picture = painter::core::Picture::FromRgba(std::span(Found->Content(), static_cast<std::size_t>(Found->Width) * Found->Height * 4u),
                                                          Found->Width, Found->Height, static_cast<std::size_t>(Found->Width) * 4u);
        if (!Found->Picture) return Refuse(ERR_NO_MEMORY);
        break;
    case ITEM_BLOCK_CANVAS: {
        TakePage(*Found, Where);
        const ItemPageFree Free{Where.Page & 1u, 0};
        Tell(Said, ITEM_EVENT_FREE, Id, &Free, sizeof(Free));
        /* What of it changed, drawn again: no more. */
        const Rect Part{Where.X, Where.Y, Where.Width, Where.Height};
        ForEachArea(Of, [Id, Part](Area &Each) { Each.State->Items.BlockChanged(Id, Part); });
        return;
    }
    case ITEM_BLOCK_FONT:
        if (const error_t Why = ReadFont(Of, *Found); Why != OK) return Refuse(Why);
        ForEachArea(Of, [](Area &Each) { Each.State->Items.RedrawAll(); });
        return;
    default:
        return;
    }
    ForEachArea(Of, [Id](Area &Each) { Each.State->Items.BlockChanged(Id); });
}

void Drop(std::size_t Index) noexcept
{
    Area &Gone = *g_areas[Index];
    InputAreaGone(*Gone.State);
    if (Gone.State->WasShown && g_glass.Vacated) g_glass.Vacated(Gone.State->Was);
    g_left = true;
    g_area_names.Give(Gone.Number);
    Program *Of = Gone.Of;
    Of->Areas--;
    g_areas.erase(g_areas.begin() + static_cast<std::ptrdiff_t>(Index));
    Forget(*Of);
}

void DropBlock(Program &Of, std::uint32_t Id) noexcept
{
    for (auto It = Of.Blocks.begin(); It != Of.Blocks.end(); ++It) {
        if ((*It)->Id != Id) continue;
        Of.BlockBytes -= (*It)->Memory.size_bytes();
        g_block_names.Give((*It)->Number);
        Of.Blocks.erase(It);
        ForEachArea(Of, [Id](Area &Each) { Each.State->Items.BlockChanged(Id); });
        return;
    }
}

/* The area the strand (Sender, Generation) speaks for by that number; the
 * count of areas for none. */
std::size_t AreaIndex(std::uint32_t Sender, std::uint32_t Generation, std::uint32_t Id) noexcept
{
    for (std::size_t I = 0; I < g_areas.size(); I++)
        if (g_areas[I]->Id == Id && g_areas[I]->OwnerPid == Sender && g_areas[I]->OwnerGeneration == Generation) return I;
    return g_areas.size();
}

/* An area made for the asker, its memory headed, its place chosen: null, and
 * Why, when it cannot be. Throws std::bad_alloc only. */
std::unique_ptr<Area> MakeArea(const Result &Asked, const AskFrom &From, const DisplayAreaOpenRequest &Request,
                               Program &Of, error_t &Why)
{
    auto Made             = std::make_unique<Area>();
    Made->OwnerPid        = Asked.sender_pid;
    Made->OwnerGeneration = From.generation;
    Made->Of              = &Of;
    Made->Placement       = Request.placement;
    Made->Always          = (Request.flags & DISPLAY_AREA_ALWAYS) != 0;
    Made->Wish            = ItemPoint{Request.width, Request.height};
    auto Memory = g_area_names.Make(sizeof(ItemAreaHeader) + ITEM_RING_BYTES + ITEM_EVENT_BYTES, From, Made->Number);
    if (!Memory) {
        Why = Memory.error().raw();
        return nullptr;
    }
    Made->Memory = std::move(*Memory);
    Made->Head   = reinterpret_cast<ItemAreaHeader *>(Made->Memory.data());
    std::memset(Made->Head, 0, sizeof(ItemAreaHeader));
    Made->Head->Magic      = ITEM_AREA_MAGIC;
    Made->Head->Bytes      = ITEM_RING_BYTES;
    Made->Head->EventBytes = ITEM_EVENT_BYTES;
    Made->State            = std::make_unique<items::AreaState>(Of.State, g_motion);
    Made->State->Place     = PlaceOf(*Made);
    Made->State->Shown     = ShownNow(*Made);
    Made->State->Paged     = Made->Placement == DISPLAY_AREA_WHOLE;
    Area *Raw              = Made.get();
    Made->State->Sized     = [Raw](std::uint32_t Item, ItemPoint Size) { Tell(*Raw, ITEM_EVENT_SIZE, Item, &Size, sizeof(Size)); };
    if (g_next_area == 0) g_next_area = 1;
    Made->Id = g_next_area++;
    return Made;
}

/* Every whole step of the area's ring applied: false when the ring breaks
 * the language — the area closed then, its program told. */
bool ReadRing(std::size_t Index) noexcept
{
    Area          &Of = *g_areas[Index];
    items::Reading Read;
    try {
        items::Listener Listen;
        Listen.Refused = [&Of](std::uint32_t Item, std::uint16_t Op, std::uint16_t Property, error_t Reason) {
            const ItemRefusal Why{Op, Property, static_cast<std::int32_t>(Reason)};
            Tell(Of, ITEM_EVENT_REFUSED, Item, &Why, sizeof(Why));
        };
        Listen.BlockChanged = [&Of](std::uint32_t Block2, const ItemBlockChanged &Where) { BlockChanged(Of, Block2, Where); };
        items::Animator &Moving = Of.State->Moving;
        Listen.Animate = [&Moving](std::uint32_t Item, const ItemAnimation &How, std::uint16_t Property,
                                   std::span<const std::uint8_t> Value) { return Moving.Said(Item, How, Property, Value); };
        Listen.Cut = [&Moving](std::uint32_t Item, std::uint16_t Property) { Moving.Cut(Item, Property); };
        Listen.Focus = [&Of](std::uint32_t Item) { InputFocusAsked(*Of.State, Item, TellerOf(Of)); };
        Moving.Reading(clock_uptime_us());
        Read = items::ReadSteps(*Of.Head, Of.Ring(), Of.State->Items, Listen);
        Moving.StepWhole();
    } catch (...) {
        /* No room to read it: the steps wait in the ring, read at the next
         * message. */
        return true;
    }
    if (Read.Broken) {
        kdbg_print("[display] area %u of program %u broke the items' language; it is closed", Of.Id, Of.Of->Pid);
        const ItemRefusal Why{0, 0, ERR_CORRUPTED};
        Tell(Of, ITEM_EVENT_REFUSED, 0, &Why, sizeof(Why));
        Ring(Of);
        Drop(Index);
        return false;
    }
    if (Read.Stepped) Of.Step = Read.Step, Of.Stepped = true;
    Of.Bytes += Read.Bytes;
    Of.Records += Read.Records;
    Ring(Of);
    return true;
}

#if defined(BOX_MEASURE)
/* In a build made to measure: each area's frames of the second that composed
 * pixels of its own, those pixels, and its frames with something moving. */
struct Measured {
    std::uint32_t Pid = 0, Frames = 0, Moving = 0;
    std::uint64_t Pixels = 0;
};
std::vector<std::pair<const Area *, Measured>> g_measured;
#endif

}  // namespace

#if defined(BOX_MEASURE)
void AreasMeasured() noexcept
{
    char Line[256];
    int  Length = std::snprintf(Line, sizeof Line, "areas:");
    for (const auto &[Of, Tally] : g_measured)
        if (Tally.Frames != 0 && Length < static_cast<int>(sizeof Line) - 48)
            Length += std::snprintf(Line + Length, sizeof Line - static_cast<std::size_t>(Length), " pid %u %u frames %llu px %u moving;",
                                    Tally.Pid, Tally.Frames, static_cast<unsigned long long>(Tally.Pixels), Tally.Moving);
    if (Length > 6) kdbg_print("[measure] %s", Line);
    g_measured.clear();
}
#endif

void AreasShown(std::uint32_t Micros, std::uint64_t Pixels) noexcept
{
    g_left = false;
#if defined(BOX_MEASURE)
    for (const auto &Each : g_areas) {
        if (Pixels == 0 || Each->State->Composed == 0) continue;
        auto Found = std::find_if(g_measured.begin(), g_measured.end(), [&](const auto &One) { return One.first == Each.get(); });
        if (Found == g_measured.end()) Found = g_measured.insert(g_measured.end(), {Each.get(), Measured{Each->OwnerPid, 0, 0, 0}});
        Found->second.Frames++;
        Found->second.Pixels += Each->State->Composed;
        if (Each->State->Moving.Due(Each->State->Shown) != 0) Found->second.Moving++;
    }
#endif
    for (const auto &Each : g_areas) {
        if (Each->Stepped) {
            const ItemShown Shown{Each->Step,  Micros, static_cast<std::uint32_t>(std::min<std::uint64_t>(Pixels, UINT32_MAX)),
                                  Each->Bytes, Each->Records,
                                  static_cast<std::uint32_t>(std::min<std::uint64_t>(Pixels ? Each->State->Composed : 0, UINT32_MAX)), 0};
            Tell(*Each, ITEM_EVENT_SHOWN, 0, &Shown, sizeof(Shown));
            Each->Stepped = false;
            Each->Bytes = Each->Records = 0;
        }
        Ring(*Each);
    }
}

void AreasStart(const AreasGlass &Glass)
{
    g_glass         = Glass;
    g_motion.Instant = Glass.Text;
    if (Glass.Text) {
        g_scale = 256;
    } else {
        /* The system's fonts read now, before the scene plays on: a text
         * page lays no text in a font, and a framebuffer's should not wait
         * for the volume — nor the scene stand still while a program's first
         * text has them read. Read here and not on a strand of the display's
         * own: every strand carries the display's tag, and a question with
         * two carriers is refused (ERR_ROUTE_AMBIGUOUS). */
        (void)painter::core::SystemFonts();
        /* Chosen: a point a pixel on a screen 1080 tall, larger by quarters
         * as the screen is taller, four at most. */
        const auto          Tall   = static_cast<std::uint32_t>(Glass.Screen.Height);
        const std::uint32_t Chosen = std::clamp<std::uint32_t>((Tall * 4u + 540u) / 1080u, 4u, 16u) * 64u;
        g_scale                    = Glass.Scale ? Glass.Scale : Chosen;
    }
}

bool AreasOpen(const Result &Asked, const AskFrom &From, const DisplayAreaOpenRequest &Request, ConsoleModel &Console) noexcept
{
    DisplayAreaOpenReply Reply{};
    Reply.command     = DISP_CMD_AREA_OPEN;
    Reply.glass       = static_cast<std::uint8_t>(g_glass.Text ? ITEM_GLASS_TEXT : ITEM_GLASS_PIXELS);
    Reply.mosaics     = g_glass.Mosaics;
    Reply.display_pid = cabin_info()->pid;
    Reply.caret_col   = Console.CursorX();
    Reply.caret_row   = Console.CursorY();
    const auto Refuse = [&](error_t Why) {
        Reply.error = static_cast<std::int32_t>(Why);
        (void)answer(&Asked, &Reply, sizeof(Reply));
        return false;
    };
    if (Request.placement < DISPLAY_AREA_OVER || Request.placement > DISPLAY_AREA_FLOW ||
        (Request.flags & ~static_cast<std::uint16_t>(DISPLAY_AREA_ALWAYS)) != 0 || Request.width < 0 || Request.height < 0 ||
        Request.width > ITEM_REACH || Request.height > ITEM_REACH)
        return Refuse(ERR_INVALID_ARGUMENT);
    try {
        Program &Of = ProgramOf(From);
        if (Of.Areas >= ITEM_AREAS_MOST) {
            Forget(Of);
            return Refuse(ERR_QUOTA_EXCEEDED);
        }
        g_areas.reserve(g_areas.size() + 1);
        error_t               Why  = OK;
        std::unique_ptr<Area> Made = MakeArea(Asked, From, Request, Of, Why);
        if (!Made) {
            Forget(Of);
            return Refuse(Why);
        }
        if (Made->Placement == DISPLAY_AREA_FLOW) {
            /* Its rows of the console, after the text so far. */
            Made->Line         = Console.Reserve(RowsOf(*Made));
            g_rows_gone        = Console.RowsGone();
            Made->State->Place = PlaceOf(*Made);
        }
        g_area_names.Spell(Made->Number, Reply.bay_tag);
        Made->Bell              = touch_intern(Reply.bay_tag);
        const ItemAreaNews News = NewsOf(*Made);
        Reply.area              = Made->Id;
        Reply.width             = News.Width;
        Reply.height            = News.Height;
        Reply.scale             = News.Scale;
        Reply.motion            = News.Motion;
        Reply.speed             = News.Speed;
        Tell(*Made, ITEM_EVENT_AREA, 0, &News, sizeof(News));
        Of.Areas++;
        g_areas.push_back(std::move(Made));
        /* An area whose answer reached nobody — its asker died first — is
         * nobody's. */
        if (answer(&Asked, &Reply, sizeof(Reply)) != 0) {
            Drop(g_areas.size() - 1);
            return false;
        }
        return true;
    } catch (...) {
        return Refuse(ERR_NO_MEMORY);
    }
}

void AreasBlockOpen(const Result &Asked, const AskFrom &From, const DisplayBlockOpenRequest &Request) noexcept
{
    DisplayBlockOpenReply Reply{};
    Reply.command     = DISP_CMD_BLOCK_OPEN;
    const auto Refuse = [&](error_t Why) {
        Reply.error = static_cast<std::int32_t>(Why);
        (void)answer(&Asked, &Reply, sizeof(Reply));
    };
    /* A block is its program's, asked through an area its strand speaks for. */
    const std::size_t Through = AreaIndex(Asked.sender_pid, From.generation, Request.area);
    if (Through == g_areas.size()) return Refuse(ERR_INVALID_STATE);
    Program      &Of    = *g_areas[Through]->Of;
    std::uint64_t Bytes = 0;
    switch (Request.kind) {
    case ITEM_BLOCK_PICTURE:
    case ITEM_BLOCK_CANVAS:
        if (Request.width == 0 || Request.height == 0 || Request.width > BlockSideMost || Request.height > BlockSideMost)
            return Refuse(ERR_OUT_OF_RANGE);
        Bytes = std::uint64_t{Request.width} * Request.height *
                (Request.kind == ITEM_BLOCK_PICTURE ? 4u : 2u * sizeof(painter::core::Pixel));
        break;
    case ITEM_BLOCK_FONT:
        if (Request.bytes == 0) return Refuse(ERR_OUT_OF_RANGE);
        Bytes = Request.bytes;
        break;
    default:
        return Refuse(ERR_INVALID_ARGUMENT);
    }
    Bytes += sizeof(ItemBlockHeader);
    if (Bytes > ITEM_BLOCK_BYTES_MOST || Of.Blocks.size() >= ITEM_BLOCKS_MOST ||
        Of.BlockBytes + Of.FontBytes + Bytes > ITEM_BLOCKS_BYTES_MOST)
        return Refuse(ERR_QUOTA_EXCEEDED);
    try {
        Of.Blocks.reserve(Of.Blocks.size() + 1);
        auto Made    = std::make_unique<Block>();
        Made->Kind   = Request.kind;
        Made->Width  = Request.width;
        Made->Height = Request.height;
        Made->Bytes  = Request.bytes;
        auto Memory  = g_block_names.Make(static_cast<std::size_t>(Bytes), From, Made->Number);
        if (!Memory) return Refuse(Memory.error().raw());
        Made->Memory          = std::move(*Memory);
        ItemBlockHeader &Head = *Made->Head();
        std::memset(&Head, 0, sizeof(Head));
        Head.Magic  = ITEM_BLOCK_MAGIC;
        Head.Kind   = Request.kind;
        Head.Width  = Request.width;
        Head.Height = Request.height;
        Head.Bytes  = Request.bytes;
        if (Request.kind == ITEM_BLOCK_CANVAS) {
            Head.PageBytes   = Request.width * Request.height * static_cast<std::uint32_t>(sizeof(painter::core::Pixel));
            Head.PageFree[0] = Head.PageFree[1] = 1;
        }
        if (g_next_block == 0) g_next_block = 1;
        Made->Id    = g_next_block++;
        Reply.block = Made->Id;
        g_block_names.Spell(Made->Number, Reply.bay_tag);
        Of.BlockBytes += Made->Memory.size_bytes();
        Of.Blocks.push_back(std::move(Made));
        if (answer(&Asked, &Reply, sizeof(Reply)) != 0) DropBlock(Of, Reply.block);
    } catch (...) {
        Refuse(ERR_NO_MEMORY);
    }
}

void AreasStep(std::uint32_t Sender, std::uint32_t Generation, std::uint32_t Id) noexcept
{
    const std::size_t Index = AreaIndex(Sender, Generation, Id);
    if (Index != g_areas.size()) (void)ReadRing(Index);
}

void AreasClose(std::uint32_t Sender, std::uint32_t Generation, std::uint32_t Id) noexcept
{
    const std::size_t Index = AreaIndex(Sender, Generation, Id);
    if (Index != g_areas.size()) Drop(Index);
}

void AreasBlockClose(std::uint32_t Sender, std::uint32_t Generation, std::uint32_t Id) noexcept
{
    /* Only a strand that speaks for an area of the block's program closes it. */
    for (const auto &Each : g_areas) {
        if (Each->OwnerPid != Sender || Each->OwnerGeneration != Generation || !Each->Of->Find(Id)) continue;
        Program &Of = *Each->Of;
        DropBlock(Of, Id);
        Forget(Of);
        return;
    }
}

void AreasDeath(std::uint32_t Pid, std::uint32_t Generation) noexcept
{
    for (std::size_t I = g_areas.size(); I-- > 0;) {
        const Area &Each = *g_areas[I];
        if ((Each.OwnerPid == Pid && Each.OwnerGeneration == Generation) || (Each.Of->Pid == Pid && Each.Of->Generation == Generation))
            Drop(I);
    }
    for (std::size_t I = g_programs.size(); I-- > 0;) {
        Program &Each = *g_programs[I];
        if (Each.Pid != Pid || Each.Generation != Generation) continue;
        while (!Each.Blocks.empty()) DropBlock(Each, Each.Blocks.back()->Id);
        Forget(Each);
    }
}

bool AreasSeen() noexcept
{
    bool Changed = false;
    for (const auto &Each : g_areas) {
        const bool Shown = ShownNow(*Each);
        if (Shown == Each->State->Shown) continue;
        Each->State->Shown      = Shown;
        const ItemAreaNews News = NewsOf(*Each);
        Tell(*Each, ITEM_EVENT_AREA, 0, &News, sizeof(News));
        Ring(*Each);
        Changed = true;
    }
    return Changed;
}

void AreasFlow(std::uint64_t RowsGone) noexcept
{
    if (RowsGone == g_rows_gone) return;
    g_rows_gone = RowsGone;
    for (const auto &Each : g_areas)
        if (Each->Placement == DISPLAY_AREA_FLOW) Each->State->Place = PlaceOf(*Each);
}

void AreasTheme() noexcept
{
    for (const auto &Each : g_areas) Each->State->Items.RedrawAll();
}

void AreasInOrder(std::vector<items::AreaState *> &Into)
{
    Into.clear();
    const auto Band = [&](auto &&In) {
        for (const auto &Each : g_areas)
            if (In(*Each)) Into.push_back(Each->State.get());
    };
    Band([](const Area &Of) { return !Of.Always && Of.Placement == DISPLAY_AREA_FLOW; });
    Band([](const Area &Of) { return !Of.Always && Of.Placement == DISPLAY_AREA_WHOLE; });
    Band([](const Area &Of) { return !Of.Always && Of.Placement == DISPLAY_AREA_OVER; });
    Band([](const Area &Of) { return Of.Always; });
    /* The display's own drawing for the keyboard over every area. */
    if (items::AreaState *Own = InputOverlay()) Into.push_back(Own);
}

std::optional<InputArea> AreasKeyed() noexcept
{
    /* The topmost seen area of the program in front holding a thing the
     * keyboard reaches: over everything first, then the whole screen, then
     * the console's flow — never one shown always. */
    for (const std::uint8_t Band : {DISPLAY_AREA_OVER, DISPLAY_AREA_WHOLE, DISPLAY_AREA_FLOW})
        for (std::size_t I = g_areas.size(); I-- > 0;) {
            Area &Each = *g_areas[I];
            if (Each.Always || Each.Placement != Band || !Each.State->Shown || !InputReaches(*Each.State)) continue;
            try {
                return InputArea{Each.State.get(), TellerOf(Each)};
            } catch (...) {
                return std::nullopt;
            }
        }
    return std::nullopt;
}

float AreasScale() noexcept { return Scale(); }

void AreasAnimate(std::uint64_t Now) noexcept
{
    for (const auto &Each : g_areas) {
        items::Animator    &Moving = Each->State->Moving;
        const std::uint64_t Due    = Moving.Due(Each->State->Shown);
        if (Due == 0 || Due > Now) continue;
        Area &Of = *Each;
        Moving.Advance(Now, Look(), [&Of](std::uint32_t Item, const ItemAnimationEnd &End) {
            Tell(Of, ITEM_EVENT_ANIMATION_END, Item, &End, sizeof(End));
        });
        Ring(Of);
    }
}

std::uint64_t AreasDue() noexcept
{
    std::uint64_t Next = 0;
    for (const auto &Each : g_areas)
        if (const std::uint64_t Due = Each->State->Moving.Due(Each->State->Shown); Due != 0) Next = Next == 0 ? Due : std::min(Next, Due);
    return Next;
}

void AreasMotion(bool Less, std::uint32_t Speed) noexcept
{
    if (g_motion.Less == Less && g_motion.Speed == Speed) return;
    g_motion.Less  = Less;
    g_motion.Speed = Speed;
    /* Every program told, so what it moves itself moves as the person wants. */
    for (const auto &Each : g_areas) {
        const ItemAreaNews News = NewsOf(*Each);
        Tell(*Each, ITEM_EVENT_AREA, 0, &News, sizeof(News));
        Ring(*Each);
    }
}

bool AreasWant() noexcept
{
    if (g_left) return true;
    for (const auto &Each : g_areas) {
        items::AreaState &State = *Each->State;
        if (State.Items.Marked() || !State.Items.Vacated().empty() || State.Place != State.Was || State.Shown != State.WasShown)
            return true;
    }
    return false;
}

namespace {

/* A cell's character as the log ring keeps it: its UTF-8, a cell that shows
 * nothing as a space, and what is no character to show — a control, a
 * surrogate, a number past U+10FFFF — as U+FFFD, the box the glass draws. */
void Spell(std::string &Text, std::uint32_t Character)
{
    if (Character == 0) {
        Text += ' ';
        return;
    }
    char           Spelled[MB_LEN_MAX];
    std::mbstate_t State{};
    const bool        Control = Character < 0x20u || (Character >= 0x7Fu && Character < 0xA0u);
    const std::size_t Length  = Control ? static_cast<std::size_t>(-1) : std::c32rtomb(Spelled, static_cast<char32_t>(Character), &State);
    Text += Length == static_cast<std::size_t>(-1) ? std::string_view{"\uFFFD"} : std::string_view{Spelled, Length};
}

/* The fold's text, as the log ring will have it: what the cells themselves
 * show — the glyph where a cell shows something, a blank where it lets the
 * text beneath through, for that text is in the account already — each row
 * without its trailing blanks and on a line of its own, the rows that show
 * nothing above and below left out, and the first on a line of its own too
 * when the caret stands inside a line; then the caret's row, when the cells
 * put it below them, as the glass does. A caret left inside the picture has
 * no place a line can go back to, and what follows it is said after its last
 * row. Throws std::bad_alloc only. */
std::string FoldText(const AreaFold &Fold, std::uint32_t ConsoleCaretX)
{
    const auto Blank = [](std::uint32_t Character) { return Character == U' ' || Character == 0 || Character == U'\u00A0'; };
    const auto At    = [&](std::uint32_t Col, std::uint32_t Row) -> const ItemCell & {
        return Fold.Cells[static_cast<std::size_t>(Row) * Fold.Width + Col];
    };
    const auto Shows = [&](std::uint32_t Row) {
        for (std::uint32_t Col = 0; Col < Fold.Width; Col++)
            if (CellShows(At(Col, Row)) && !Blank(At(Col, Row).Character)) return true;
        return false;
    };
    std::uint32_t First = 0;
    while (First < Fold.Height && !Shows(First)) First++;
    std::uint32_t Last = Fold.Height;
    while (Last > First && !Shows(Last - 1u)) Last--;

    std::string Text;
    if (First < Last && ConsoleCaretX != 0) Text += '\n';
    for (std::uint32_t Row = First; Row < Last; Row++) {
        if (Row != First) Text += '\n';
        std::size_t Kept = Text.size();
        for (std::uint32_t Col = 0; Col < Fold.Width; Col++) {
            const std::uint32_t Character = CellShows(At(Col, Row)) ? At(Col, Row).Character : U' ';
            Spell(Text, Character);
            if (!Blank(Character)) Kept = Text.size();
        }
        Text.resize(Kept);
    }
    const std::int64_t Below = static_cast<std::int64_t>(Fold.CaretY) - Fold.Y;
    if (First < Last && Fold.Caret && Below >= static_cast<std::int64_t>(Last)) {
        Text.append(static_cast<std::size_t>(Below - (Last - 1u)), '\n');
        Text.append(Fold.CaretX, ' ');
    }
    return Text;
}

}  // namespace

std::expected<AreaFold, error_t> AreasCommit(std::uint32_t Pid, std::uint32_t Generation,
                                             const DisplayAreaCommitRequest &Request, const ConsoleModel &Console) noexcept
{
    const std::size_t Index = AreaIndex(Pid, Generation, Request.area);
    if (Index == g_areas.size()) return std::unexpected(ERR_INVALID_STATE);
    /* What the program wrote before it asked: its last steps. */
    if (!ReadRing(Index)) return std::unexpected(ERR_CORRUPTED);
    Area &Of = *g_areas[Index];
    if (Request.scroll_rows > Console.Height()) return std::unexpected(ERR_INVALID_ARGUMENT);

    /* Every item the area holds is cells on the console's grid: where each
     * lies, in the console's cells. */
    const CellsLook &Look  = Cells();
    const float      S     = Scale();
    const Rect       Place = Of.State->Place;
    struct Laid {
        const items::Item *Item;
        std::int32_t       Col, Row;
    };
    std::vector<Laid> Laying;
    try {
        for (std::uint32_t Id : Of.State->Items.Top()) {
            const items::Item *Each = Of.State->Items.Find(Id);
            if (!Each || items::HiddenOf(Each->Said)) continue;
            if (Each->Kind != ITEM_CELLS || !Each->Children.empty()) return std::unexpected(ERR_UNSUPPORTED);
            if (!Each->Grid || (Each->Said.Turn && *Each->Said.Turn % ITEM_TURN != 0) ||
                (Each->Said.Scale && (Each->Said.Scale->X != ITEM_ONE || Each->Said.Scale->Y != ITEM_ONE)))
                return std::unexpected(ERR_UNSUPPORTED);
            painter::core::Point Corner{Place.X, Place.Y};
            if (Each->Said.Place)
                Corner = painter::core::Point{
                    Place.X + static_cast<std::int32_t>(std::floor(static_cast<float>(Each->Said.Place->X) / ITEM_POINT * S + 0.5f)),
                    Place.Y + static_cast<std::int32_t>(std::floor(static_cast<float>(Each->Said.Place->Y) / ITEM_POINT * S + 0.5f))};
            else
                Corner = OnTheGrid(Look, Corner);
            if ((Corner.X - Look.Grid.X) % Look.Width != 0 || (Corner.Y - Look.Grid.Y) % Look.Height != 0)
                return std::unexpected(ERR_UNSUPPORTED);
            Laying.push_back(Laid{Each, (Corner.X - Look.Grid.X) / Look.Width, (Corner.Y - Look.Grid.Y) / Look.Height});
        }
    } catch (...) {
        return std::unexpected(ERR_NO_MEMORY);
    }

    /* The console's cells they cover, and theirs laid there one over another. */
    std::int32_t Left = static_cast<std::int32_t>(Console.Width()), Top = static_cast<std::int32_t>(Console.Height()), Right = 0,
                 Foot = 0;
    for (const Laid &Each : Laying) {
        Left  = std::min(Left, std::max(Each.Col, 0));
        Top   = std::min(Top, std::max(Each.Row, 0));
        Right = std::max(Right, std::min<std::int32_t>(Each.Col + static_cast<std::int32_t>(Each.Item->Grid->Cols()),
                                                       static_cast<std::int32_t>(Console.Width())));
        Foot  = std::max(Foot, std::min<std::int32_t>(Each.Row + static_cast<std::int32_t>(Each.Item->Grid->Rows()),
                                                      static_cast<std::int32_t>(Console.Height())));
    }
    AreaFold Fold;
    try {
        if (Left < Right && Top < Foot) {
            Fold.X      = Left;
            Fold.Y      = Top;
            Fold.Width  = static_cast<std::uint32_t>(Right - Left);
            Fold.Height = static_cast<std::uint32_t>(Foot - Top);
            Fold.Cells.assign(static_cast<std::size_t>(Fold.Width) * Fold.Height, item_cell_empty());
            for (const Laid &Each : Laying) {
                const CellGrid &Grid = *Each.Item->Grid;
                for (std::uint32_t Row = 0; Row < Grid.Rows(); Row++)
                    for (std::uint32_t Col = 0; Col < Grid.Cols(); Col++) {
                        const std::int32_t X = Each.Col + static_cast<std::int32_t>(Col) - Left;
                        const std::int32_t Y = Each.Row + static_cast<std::int32_t>(Row) - Top;
                        if (X < 0 || Y < 0 || X >= static_cast<std::int32_t>(Fold.Width) || Y >= static_cast<std::int32_t>(Fold.Height))
                            continue;
                        const ItemCell &Above = Grid.At(Col, Row);
                        if (!CellShows(Above)) continue;
                        ItemCell &Into = Fold.Cells[static_cast<std::size_t>(Y) * Fold.Width + static_cast<std::size_t>(X)];
                        ItemCell  Shown;
                        if (CellOver(Above, Into, Look.Page, Shown) || Above.Mark != ITEM_MARK_NONE) Into = Shown;
                    }
                /* The topmost caret shown is the one the console goes on from. */
                const ItemCaret Caret = Grid.Caret();
                const std::int64_t CaretX = static_cast<std::int64_t>(Each.Col) + Caret.Col, CaretY = static_cast<std::int64_t>(Each.Row) + Caret.Row;
                if (Grid.CaretShown() && CaretX >= 0 && CaretY >= 0 && CaretX < Console.Width() && CaretY < Console.Height()) {
                    Fold.Caret  = true;
                    Fold.CaretX = static_cast<std::uint32_t>(CaretX);
                    Fold.CaretY = static_cast<std::uint32_t>(CaretY);
                }
            }
        }
        Fold.ScrollRows = Request.scroll_rows;
        Fold.Text       = FoldText(Fold, Console.CursorX());
    } catch (...) {
        return std::unexpected(ERR_NO_MEMORY);
    }
    Drop(Index);
    return Fold;
}
