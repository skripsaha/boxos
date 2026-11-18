/*
 * areas.cpp — the display's side of the items' language: programs' areas and
 * blocks, their questions, messages and events, and each frame of them. See
 * areas.h.
 */

#include "areas.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <utility>

#include "box/color.h"
#include "box/core/cabin.h"
#include "box/cxx/bay.h"
#include "box/debug.h"
#include "box/error.h"
#include "box/ipc.h"
#include "box/touch.h"

#include "compositor.h"
#include "front.h"
#include "item_reception.h"
#include "painter/core/system_fonts.h"
#include "typecase/text.h"

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
    ItemPoint                         Wish{};
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

/* A text page's cells as pixels, for an area's place: a point is a pixel of
 * an eighth of a cell across and a sixteenth down. */
constexpr std::int32_t PageCellWide = 8, PageCellTall = 16;

/* A side of the canvas a frame composes on, tile by tile. */
constexpr std::int32_t TileSide = 256;

/* The largest side of a picture or canvas a program may give. */
constexpr std::uint32_t BlockSideMost = 16384;

std::vector<std::unique_ptr<Program>> g_programs;
std::vector<std::unique_ptr<Area>>    g_areas;     // in the order they opened, bottom to top
std::vector<Rect>                     g_covered;   // where the shown areas lie
Names                                 g_area_names(DISPLAY_AREA_TAG_PREFIX);
Names                                 g_block_names(DISPLAY_BLOCK_TAG_PREFIX);
std::uint32_t                         g_next_area = 1, g_next_block = 1;
std::optional<items::Compositor>      g_compositor;
std::optional<painter::core::Canvas>  g_tile;
items::Theme                          g_theme;
Rect                                  g_screen{}, g_console{};
std::int32_t                          g_cell_tall = PageCellTall;
std::uint32_t                         g_scale     = 256;
bool                                  g_text      = false;
bool                                  g_left      = false;   // an area left a text page: its cells to be written again

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

/* Where an area lies on the glass, in pixels (a text page's eighths and
 * sixteenths of a cell): the display's choice. */
Rect PlaceOf(const Area &Of, std::uint32_t CaretRow) noexcept
{
    if (Of.Placement == DISPLAY_AREA_WHOLE) return g_screen;
    const float S  = Scale();
    const auto  Px = [S](std::int32_t Points, std::int32_t Chosen) {
        return Points ? static_cast<std::int32_t>(std::lround(static_cast<float>(Points) / ITEM_POINT * S)) : Chosen;
    };
    std::int32_t       W = std::clamp(Px(Of.Wish.X, g_screen.Width * 3 / 5), 1, g_screen.Width);
    const std::int32_t H = std::clamp(Px(Of.Wish.Y, g_screen.Height * 3 / 5), 1, g_screen.Height);
    if (Of.Placement == DISPLAY_AREA_FLOW) {
        W                      = g_console.Width;
        const std::int32_t Top = g_console.Y + static_cast<std::int32_t>(CaretRow) * g_cell_tall;
        return Rect{g_console.X, std::clamp(Top, 0, g_screen.Height - H), W, H};
    }
    return Rect{(g_screen.Width - W) / 2, (g_screen.Height - H) / 2, W, H};
}

ItemAreaNews NewsOf(const Area &Of) noexcept
{
    const Rect  Place = Of.State->Place;
    const float S     = Scale();
    return ItemAreaNews{static_cast<std::int32_t>(std::lround(static_cast<float>(Place.Width) / S * ITEM_POINT)),
                        static_cast<std::int32_t>(std::lround(static_cast<float>(Place.Height) / S * ITEM_POINT)),
                        g_scale, static_cast<std::uint8_t>(g_text ? ITEM_GLASS_TEXT : ITEM_GLASS_PIXELS),
                        static_cast<std::uint8_t>(Of.State->Shown), 0};
}

void Cover() noexcept
{
    g_covered.clear();   /* room made for every area when it opened */
    for (const auto &Each : g_areas)
        if (Each->State->Shown) g_covered.push_back(Each->State->Place);
}

/* The program From names, made when it has none yet: its fonts the system's,
 * its pictures and canvases its blocks. Throws std::bad_alloc only. */
Program &ProgramOf(const AskFrom &From)
{
    for (const auto &Each : g_programs)
        if (Each->Pid == From.program_pid && Each->Generation == From.program_generation) return *Each;
    auto Made        = std::make_unique<Program>();
    Made->Pid        = From.program_pid;
    Made->Generation = From.program_generation;
    if (const std::shared_ptr<const typecase::Fonts> System = painter::core::SystemFonts()) {
        for (const auto &Face : System->Faces()) Made->Book.Add(Face);
        Made->Book.SetFallback(System->Fallbacks());
        Made->Book.SetDefault(System->Default());
    }
    Program *Of                = Made.get();
    Made->State.From.Look      = &g_theme;
    Made->State.From.Scale     = Scale();
    Made->State.From.Book      = &Made->Book;
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
    Of.Book.Add(std::move(*Face));
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
        break;
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
    if (g_compositor && Gone.State->WasShown) g_compositor->Damage(Gone.State->Was);
    g_left = g_text;
    g_area_names.Give(Gone.Number);
    Program *Of = Gone.Of;
    Of->Areas--;
    g_areas.erase(g_areas.begin() + static_cast<std::ptrdiff_t>(Index));
    Forget(*Of);
    Cover();
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
                               Program &Of, std::uint32_t CaretRow, error_t &Why)
{
    auto Made             = std::make_unique<Area>();
    Made->OwnerPid        = Asked.sender_pid;
    Made->OwnerGeneration = From.generation;
    Made->Of              = &Of;
    Made->Placement       = Request.placement;
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
    Made->State            = std::make_unique<items::AreaState>(Of.State);
    Made->State->Place     = PlaceOf(*Made, CaretRow);
    Made->State->Shown     = !FrontBehind(Of.Pid, Of.Generation);
    Made->State->Paged     = Made->Placement == DISPLAY_AREA_WHOLE;
    Area *Raw              = Made.get();
    Made->State->Sized     = [Raw](std::uint32_t Item, ItemPoint Size) { Tell(*Raw, ITEM_EVENT_SIZE, Item, &Size, sizeof(Size)); };
    if (g_next_area == 0) g_next_area = 1;
    Made->Id = g_next_area++;
    return Made;
}

/* The canvas a framebuffer's frame composes on, and the compositor: made when
 * the first area opens. False: no room for them. */
bool Ready() noexcept
{
    if (g_text || g_compositor) return true;
    try {
        if (!g_tile) g_tile = painter::core::Canvas::Make(TileSide, TileSide);
        if (!g_tile) return false;
        g_compositor.emplace(g_screen);
        return true;
    } catch (...) {
        return false;
    }
}

/* Each area's step shown on the glass told to it, with what the frame cost,
 * and every bell rung that has news. */
void TellShown(std::uint32_t Micros, std::uint64_t Pixels) noexcept
{
    for (const auto &Each : g_areas) {
        if (Each->Stepped) {
            const ItemShown Shown{Each->Step, Micros, static_cast<std::uint32_t>(std::min<std::uint64_t>(Pixels, UINT32_MAX)),
                                  Each->Bytes, Each->Records};
            Tell(*Each, ITEM_EVENT_SHOWN, 0, &Shown, sizeof(Shown));
            Each->Stepped = false;
            Each->Bytes = Each->Records = 0;
        }
        Ring(*Each);
    }
}

}  // namespace

void AreasStart(const Easel &On, std::uint32_t Scale2)
{
    g_text = !On.Pixels();
    if (g_text) {
        g_screen    = Rect{0, 0, static_cast<std::int32_t>(On.Cols()) * PageCellWide, static_cast<std::int32_t>(On.Rows()) * PageCellTall};
        g_console   = g_screen;
        g_cell_tall = PageCellTall;
        g_scale     = 256;
    } else {
        g_screen    = Rect{0, 0, static_cast<std::int32_t>(On.PixelWidth()), static_cast<std::int32_t>(On.PixelHeight())};
        g_console   = On.Grid();
        g_cell_tall = On.CellHeight();
        /* Chosen: a point a pixel on a screen 1080 tall, larger by quarters
         * as the screen is taller, four at most. */
        const std::uint32_t Chosen = std::clamp<std::uint32_t>((On.PixelHeight() * 4u + 540u) / 1080u, 4u, 16u) * 64u;
        g_scale                    = Scale2 ? Scale2 : Chosen;
    }
    g_theme = items::ThemeOf(On.TextColor(), On.PageColor());
}

bool AreasOpen(const Result &Asked, const AskFrom &From, const DisplayAreaOpenRequest &Request, std::uint32_t CaretRow) noexcept
{
    DisplayAreaOpenReply Reply{};
    Reply.command     = DISP_CMD_AREA_OPEN;
    Reply.glass       = static_cast<std::uint8_t>(g_text ? ITEM_GLASS_TEXT : ITEM_GLASS_PIXELS);
    Reply.display_pid = cabin_info()->pid;
    const auto Refuse = [&](error_t Why) {
        Reply.error = static_cast<std::int32_t>(Why);
        (void)answer(&Asked, &Reply, sizeof(Reply));
        return false;
    };
    if (Request.placement < DISPLAY_AREA_OVER || Request.placement > DISPLAY_AREA_FLOW || Request.flags != 0 ||
        Request.width < 0 || Request.height < 0 || Request.width > ITEM_REACH || Request.height > ITEM_REACH)
        return Refuse(ERR_INVALID_ARGUMENT);
    if (!Ready()) return Refuse(ERR_NO_MEMORY);
    try {
        Program &Of = ProgramOf(From);
        if (Of.Areas >= ITEM_AREAS_MOST) {
            Forget(Of);
            return Refuse(ERR_QUOTA_EXCEEDED);
        }
        g_areas.reserve(g_areas.size() + 1);
        g_covered.reserve(g_areas.size() + 1);
        error_t               Why  = OK;
        std::unique_ptr<Area> Made = MakeArea(Asked, From, Request, Of, CaretRow, Why);
        if (!Made) {
            Forget(Of);
            return Refuse(Why);
        }
        g_area_names.Spell(Made->Number, Reply.bay_tag);
        Made->Bell              = touch_intern(Reply.bay_tag);
        const ItemAreaNews News = NewsOf(*Made);
        Reply.area              = Made->Id;
        Reply.width             = News.Width;
        Reply.height            = News.Height;
        Reply.scale             = News.Scale;
        Tell(*Made, ITEM_EVENT_AREA, 0, &News, sizeof(News));
        Of.Areas++;
        g_areas.push_back(std::move(Made));
        Cover();
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
    if (Index == g_areas.size()) return;
    Area          &Of = *g_areas[Index];
    items::Reading Read;
    try {
        items::Listener Listen;
        Listen.Refused = [&Of](std::uint32_t Item, std::uint16_t Op, std::uint16_t Property, error_t Reason) {
            const ItemRefusal Why{Op, Property, static_cast<std::int32_t>(Reason)};
            Tell(Of, ITEM_EVENT_REFUSED, Item, &Why, sizeof(Why));
        };
        Listen.BlockChanged = [&Of](std::uint32_t Block2, const ItemBlockChanged &Where) { BlockChanged(Of, Block2, Where); };
        Read = items::ReadSteps(*Of.Head, Of.Ring(), Of.State->Items, Listen);
    } catch (...) {
        /* No room to read it: the steps wait in the ring, read at the next
         * message. */
        return;
    }
    if (Read.Broken) {
        /* A ring that breaks the language: the area closed, its program told
         * why. */
        kdbg_print("[display] area %u of program %u broke the items' language; it is closed", Of.Id, Of.Of->Pid);
        const ItemRefusal Why{0, 0, ERR_CORRUPTED};
        Tell(Of, ITEM_EVENT_REFUSED, 0, &Why, sizeof(Why));
        Ring(Of);
        Drop(Index);
        return;
    }
    if (Read.Stepped) Of.Step = Read.Step, Of.Stepped = true;
    Of.Bytes += Read.Bytes;
    Of.Records += Read.Records;
    Ring(Of);
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
        const bool Shown = !FrontBehind(Each->Of->Pid, Each->Of->Generation);
        if (Shown == Each->State->Shown) continue;
        Each->State->Shown      = Shown;
        const ItemAreaNews News = NewsOf(*Each);
        Tell(*Each, ITEM_EVENT_AREA, 0, &News, sizeof(News));
        Ring(*Each);
        Changed = true;
    }
    if (Changed) Cover();
    return Changed;
}

void AreasTheme(std::uint32_t Text, std::uint32_t Page) noexcept
{
    try {
        g_theme = items::ThemeOf(Text, Page);
    } catch (...) {
        return;
    }
    for (const auto &Each : g_areas) Each->State->Items.RedrawAll();
}

std::span<const Rect> AreasCovered() noexcept { return g_covered; }

void AreasDamage(const Rect &Pixels) noexcept
{
    if (g_compositor) g_compositor->Damage(Pixels);
}

bool AreasWant() noexcept
{
    if (g_left || (g_compositor && g_compositor->Damaged())) return true;
    for (const auto &Each : g_areas) {
        items::AreaState &State = *Each->State;
        if (State.Items.Marked() || !State.Items.Vacated().empty() || State.Place != State.Was || State.Shown != State.WasShown)
            return true;
    }
    return false;
}

void AreasFrame(Easel &On) noexcept
{
    if (!g_compositor || !g_tile || On.Lent()) return;
    std::vector<items::AreaState *> Order;
    std::vector<Rect>               Damaged;
    const auto                      Start  = std::chrono::steady_clock::now();
    std::uint64_t                   Pixels = 0;
    try {
        Order.reserve(g_areas.size());
        /* Areas of the whole screen over the others. */
        for (const auto &Each : g_areas)
            if (Each->Placement != DISPLAY_AREA_WHOLE) Order.push_back(Each->State.get());
        for (const auto &Each : g_areas)
            if (Each->Placement == DISPLAY_AREA_WHOLE) Order.push_back(Each->State.get());
        g_compositor->Take(Order, g_theme);
        Damaged = g_compositor->Rects();
        for (const Rect &Each : Damaged)
            for (std::int32_t Y = Each.Y; Y < Each.Y + Each.Height; Y += TileSide)
                for (std::int32_t X = Each.X; X < Each.X + Each.Width; X += TileSide) {
                    const Rect Tile{X, Y, std::min(TileSide, Each.X + Each.Width - X), std::min(TileSide, Each.Y + Each.Height - Y)};
                    const painter::core::Point Corner{Tile.X, Tile.Y};
                    if (!g_compositor->Hidden(Tile, Order)) On.Beneath(*g_tile, Corner, Tile);
                    g_compositor->Compose(*g_tile, Corner, Tile, Order, g_theme);
                    On.Put(*g_tile, Tile);
                    Pixels += static_cast<std::uint64_t>(Tile.Width) * static_cast<std::uint64_t>(Tile.Height);
                }
    } catch (...) {
        /* No room to compose: what was put on the glass stays, and all of
         * this frame's damage is composed again at the next. */
        for (const Rect &Each : Damaged) g_compositor->Damage(Each);
    }
    On.Flush();
    const auto Micros = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - Start).count();
    TellShown(static_cast<std::uint32_t>(std::clamp<std::int64_t>(Micros, 0, UINT32_MAX)), Pixels);
}

namespace {

/* Characters written into a text page's cells from a cell on, up to the
 * page's edge, in Ink. */
void Write(std::vector<GlassCell> &Cells, std::uint32_t Cols, std::uint32_t Rows, std::int32_t Col, std::int32_t Row,
           std::u32string_view Characters, std::uint32_t Ink) noexcept
{
    if (Row < 0 || Row >= static_cast<std::int32_t>(Rows)) return;
    for (std::size_t I = 0; I < Characters.size(); I++) {
        const std::int32_t At = Col + static_cast<std::int32_t>(I);
        if (At < 0 || At >= static_cast<std::int32_t>(Cols)) continue;
        GlassCell &Into = Cells[static_cast<std::size_t>(Row) * Cols + static_cast<std::size_t>(At)];
        Into.Cell.cp    = Characters[I];
        Into.Cell.fg    = Ink;
    }
}

/* A text item's ink on a text page: its first fill's colour. Throws
 * std::bad_alloc only. */
std::uint32_t InkOf(const items::Item &Of)
{
    const std::vector<items::Fill> Fills = items::FillsOf(Of.Said, Of.Kind, Of.Role);
    if (Fills.empty()) return COLOR_DEFAULT;
    const painter::core::Color C =
        Fills[0].Kind == ITEM_FILL_THEME ? items::ThemeColor(g_theme, Fills[0].Theme) : items::ColorOf(Fills[0].Color);
    return static_cast<std::uint32_t>(C.Red()) << 16 | static_cast<std::uint32_t>(C.Green()) << 8 | C.Blue();
}

/* One area's items into the cells: the area's cells cleared, then each text
 * written where it lies — a secret as stars — and each other item but a group
 * by its caption. Throws std::bad_alloc only. */
void CellsOf(const items::AreaState &Area, std::vector<GlassCell> &Cells, std::uint32_t Cols, std::uint32_t Rows)
{
    const Rect         Place = Area.Place;
    const std::int32_t Right = std::min<std::int32_t>((Place.X + Place.Width) / PageCellWide, static_cast<std::int32_t>(Cols));
    const std::int32_t Foot  = std::min<std::int32_t>((Place.Y + Place.Height) / PageCellTall, static_cast<std::int32_t>(Rows));
    for (std::int32_t Row = Place.Y / PageCellTall; Row < Foot; Row++)
        for (std::int32_t Col = Place.X / PageCellWide; Col < Right; Col++)
            Cells[static_cast<std::size_t>(Row) * Cols + static_cast<std::size_t>(Col)] = GlassCell{};
    /* Every item, with where its groups put it, in points. */
    std::vector<std::tuple<std::uint32_t, std::int64_t, std::int64_t>> Walk;
    for (auto It = Area.Items.Top().rbegin(); It != Area.Items.Top().rend(); ++It) Walk.emplace_back(*It, 0, 0);
    while (!Walk.empty()) {
        const auto [Id, X0, Y0] = Walk.back();
        Walk.pop_back();
        const items::Item *Of = Area.Items.Find(Id);
        if (!Of || items::HiddenOf(Of->Said)) continue;
        const ItemPoint    Own = Of->Said.Place ? *Of->Said.Place : ItemPoint{0, 0};
        const std::int64_t X = X0 + Own.X, Y = Y0 + Own.Y;
        for (auto Child = Of->Children.rbegin(); Child != Of->Children.rend(); ++Child) Walk.emplace_back(*Child, X, Y);
        const auto Col = static_cast<std::int32_t>((Place.X + X / ITEM_POINT) / PageCellWide);
        const auto Row = static_cast<std::int32_t>((Place.Y + Y / ITEM_POINT) / PageCellTall);
        if (Of->Kind == ITEM_TEXT && Of->Said.Text) {
            std::u32string Characters = typecase::CharactersOf(*Of->Said.Text);
            if (Of->Said.Secret && *Of->Said.Secret)
                std::replace_if(Characters.begin(), Characters.end(), [](char32_t C) { return C != U'\n'; }, U'*');
            const std::uint32_t Ink  = InkOf(*Of);
            std::u32string_view Rest = Characters;
            for (std::int32_t Line = 0; !Rest.empty(); Line++) {
                const std::size_t End = Rest.find(U'\n');
                Write(Cells, Cols, Rows, Col, Row + Line, Rest.substr(0, End), Ink);
                Rest = End == std::u32string_view::npos ? std::u32string_view() : Rest.substr(End + 1);
            }
        } else if (Of->Kind != ITEM_GROUP && Of->Said.Caption) {
            Write(Cells, Cols, Rows, Col, Row, typecase::CharactersOf(*Of->Said.Caption), COLOR_DEFAULT);
        }
    }
}

}  // namespace

void AreasCells(std::vector<GlassCell> &Cells, std::uint32_t Cols, std::uint32_t Rows) noexcept
{
    if (Cells.size() != static_cast<std::size_t>(Cols) * Rows) return;
    g_left = false;
    for (const auto &Each : g_areas) {
        items::AreaState &State = *Each->State;
        try {
            if (State.Shown) CellsOf(State, Cells, Cols, Rows);
        } catch (...) {
            /* No room to walk it: the area shows what was written of it. */
        }
        State.Items.Taken();
        State.Items.Vacated().clear();
        State.Was      = State.Place;
        State.WasShown = State.Shown;
    }
    TellShown(0, 0);
}
