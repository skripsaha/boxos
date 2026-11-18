/*
 * area.cpp — a program's area on the screen. See painter/gui/area.h.
 */

#include "painter/gui/area.h"

#include <cstring>
#include <utility>

#include "box/core/strand_self.h"
#include "box/display.h"
#include "box/error.h"
#include "box/ipc.h"
#include "box/print.h"
#include "box/system.h"

namespace painter::gui {

namespace {

box::error Cause(int Code) { return box::error{box_errno_of(Code)}; }

std::int32_t Points(float Value) noexcept
{
    return Value > 0 && Value < static_cast<float>(ITEM_REACH) / ITEM_POINT
               ? static_cast<std::int32_t>(Value * ITEM_POINT + 0.5f)
               : 0;
}

/* The area's rings, after its head. */
std::uint8_t *RingOf(box::bay<std::byte> &Memory) noexcept
{
    return reinterpret_cast<std::uint8_t *>(Memory.data()) + sizeof(ItemAreaHeader);
}

std::uint8_t *EventsOf(box::bay<std::byte> &Memory) noexcept { return RingOf(Memory) + ITEM_RING_BYTES; }

const ItemBlockHeader *HeadOf(const box::bay<std::byte> &Memory) noexcept
{
    return reinterpret_cast<const ItemBlockHeader *>(Memory.data());
}

}  // namespace

std::span<std::uint8_t> PictureBlock::Pixels() noexcept
{
    if (!Memory) return {};
    return {reinterpret_cast<std::uint8_t *>(Memory.data()) + sizeof(ItemBlockHeader),
            static_cast<std::size_t>(Width) * Height * 4u};
}

bool CanvasBlock::Free(std::uint32_t Page) const noexcept
{
    if (!Memory || Page > 1) return false;
    return __atomic_load_n(&HeadOf(Memory)->PageFree[Page], __ATOMIC_ACQUIRE) != 0;
}

std::optional<core::Canvas> CanvasBlock::Page(std::uint32_t Page) noexcept
{
    if (!Free(Page)) return std::nullopt;
    auto *Pixels = reinterpret_cast<core::Pixel *>(reinterpret_cast<std::uint8_t *>(Memory.data()) + sizeof(ItemBlockHeader) +
                                                   static_cast<std::size_t>(Page) * HeadOf(Memory)->PageBytes);
    return core::Canvas::Over(Pixels, Width, Height, Width);
}

void CanvasBlock::Taken(std::uint32_t Page) noexcept
{
    if (!Memory || Page > 1) return;
    auto *Head = reinterpret_cast<ItemBlockHeader *>(Memory.data());
    __atomic_store_n(&Head->PageFree[Page], 0u, __ATOMIC_RELEASE);
}

std::expected<Area, box::error> Area::Open(Placement Where, core::PointF Wish)
{
    DisplayAreaOpenRequest Request{};
    Request.command   = DISP_CMD_AREA_OPEN;
    Request.placement = static_cast<std::uint8_t>(Where);
    Request.width     = Points(Wish.X);
    Request.height    = Points(Wish.Y);
    Request.order     = console_order_mark();
    DisplayAreaOpenReply Reply{};
    std::uint32_t        Got  = 0;
    const int            Code = ask(DISPLAY_TAG, &Request, sizeof(Request), &Reply, sizeof(Reply), &Got);
    if (Code != 0) return std::unexpected(Cause(Code));
    if (Got != sizeof(Reply) || Reply.command != DISP_CMD_AREA_OPEN) return std::unexpected(box::error{box::errc::corrupted});
    if (Reply.error != 0) return std::unexpected(box::error{static_cast<error_t>(Reply.error)});
    if (Reply.display_pid == 0 || Reply.area == 0 || std::memchr(Reply.bay_tag, '\0', sizeof(Reply.bay_tag)) == nullptr ||
        (Reply.glass != ITEM_GLASS_TEXT && Reply.glass != ITEM_GLASS_PIXELS) || Reply.scale == 0)
        return std::unexpected(box::error{box::errc::corrupted});

    Area Made;
    Made.AreaId         = Reply.area;
    Made.DisplayPid     = Reply.display_pid;
    Made.Opened         = true;   /* the display holds it now: closed by the destructor whatever follows */
    Made.Given          = core::PointF{static_cast<float>(Reply.width) / ITEM_POINT, static_cast<float>(Reply.height) / ITEM_POINT};
    Made.PixelsPerPoint = static_cast<float>(Reply.scale) / 256.0f;
    Made.Kind           = static_cast<gui::Glass>(Reply.glass);
    Made.Memory         = box::bay<std::byte>::open(Reply.bay_tag);
    if (!Made.Memory) return std::unexpected(box::error{box::errc::tag_not_found});
    Made.Head = reinterpret_cast<ItemAreaHeader *>(Made.Memory.data());
    if (Made.Memory.size_bytes() < sizeof(ItemAreaHeader) + ITEM_RING_BYTES + ITEM_EVENT_BYTES ||
        Made.Head->Magic != ITEM_AREA_MAGIC || Made.Head->Bytes != ITEM_RING_BYTES || Made.Head->EventBytes != ITEM_EVENT_BYTES)
        return std::unexpected(box::error{box::errc::corrupted});
    Made.Rung = touch_pair_choose(touch_intern(Reply.bay_tag));
    if (Made.Rung != TOUCH_TAG_INVALID && touch_claim(Made.Rung, TOUCH_REST, 0, 0) != 0) Made.Rung = TOUCH_TAG_INVALID;
    return Made;
}

Area::Area(Area &&Other) noexcept
    : Changes(std::move(Other)), AreaId(Other.AreaId), DisplayPid(Other.DisplayPid), Memory(std::move(Other.Memory)),
      Head(Other.Head), Given(Other.Given), PixelsPerPoint(Other.PixelsPerPoint), Kind(Other.Kind), Rung(Other.Rung),
      Steps(Other.Steps), Opened(Other.Opened)
{
    Other.Opened = false;
    Other.Head   = nullptr;
    Other.Rung   = TOUCH_TAG_INVALID;
}

Area::~Area()
{
    if (Opened) Tell(DISP_CMD_AREA_CLOSE, AreaId);
    if (Rung != TOUCH_TAG_INVALID) touch_release(Rung);
}

void Area::Tell(std::uint8_t Command, std::uint32_t What) noexcept
{
    DisplayAreaNotice Notice{};
    Notice.command    = Command;
    Notice.area       = What;
    Notice.generation = strand_self_generation();
    for (;;) {
        const int Code = send(DisplayPid, &Notice, sizeof(Notice));
        if (Code == 0 || box_errno_of(Code) != ERR_NO_MEMORY) return;
        yield();
    }
}

bool Area::Present()
{
    if (!Head) return false;
    std::uint64_t       Write  = Head->Write;
    const std::uint64_t Read   = __atomic_load_n(&Head->Read, __ATOMIC_ACQUIRE);
    const std::uint64_t Before = Write;
    const bool          All    = Flush(RingOf(Memory), ITEM_RING_BYTES, Write, Read, Steps + 1);
    if (Write == Before) return All;
    Steps++;
    __atomic_store_n(&Head->Write, Write, __ATOMIC_RELEASE);
    Tell(DISP_CMD_AREA_STEP, AreaId);
    return All;
}

std::optional<Event> Area::Next()
{
    if (!Head) return std::nullopt;
    for (;;) {
        const std::uint64_t Write = __atomic_load_n(&Head->EventWrite, __ATOMIC_ACQUIRE);
        std::uint64_t       Read  = Head->EventRead;
        if (Read >= Write || Write - Read > ITEM_EVENT_BYTES) return std::nullopt;
        const auto Offset = static_cast<std::uint32_t>(Read % ITEM_EVENT_BYTES);
        ItemEvent  Head2{};
        std::memcpy(&Head2, EventsOf(Memory) + Offset, sizeof(Head2));
        const std::uint32_t Length = Head2.Kind == ITEM_EVENT_PAD && Head2.Words == 0 ? ITEM_EVENT_BYTES - Offset
                                                                                       : static_cast<std::uint32_t>(Head2.Words) * 8u;
        if (Length < sizeof(Head2) || Length > ITEM_EVENT_BYTES - Offset || Length > Write - Read) return std::nullopt;
        const std::uint8_t *Body  = EventsOf(Memory) + Offset + sizeof(Head2);
        const std::uint32_t Bytes = Length - static_cast<std::uint32_t>(sizeof(Head2));
        Read += Length;
        __atomic_store_n(&Head->EventRead, Read, __ATOMIC_RELEASE);
        Event Made;
        Made.Item = Head2.Item;
        switch (Head2.Kind) {
        case ITEM_EVENT_AREA: {
            ItemAreaNews News{};
            if (Bytes < sizeof(News)) continue;
            std::memcpy(&News, Body, sizeof(News));
            Given          = core::PointF{static_cast<float>(News.Width) / ITEM_POINT, static_cast<float>(News.Height) / ITEM_POINT};
            PixelsPerPoint = News.Scale ? static_cast<float>(News.Scale) / 256.0f : PixelsPerPoint;
            Kind           = News.Glass == ITEM_GLASS_TEXT ? gui::Glass::Text : gui::Glass::Pixels;
            Made.What  = Event::Kind::Area;
            Made.Size  = Given;
            Made.Scale = PixelsPerPoint;
            Made.On    = Kind;
            Made.Seen  = News.Shown != 0;
            return Made;
        }
        case ITEM_EVENT_SIZE: {
            ItemPoint Size{};
            if (Bytes < sizeof(Size)) continue;
            std::memcpy(&Size, Body, sizeof(Size));
            Made.What = Event::Kind::Size;
            Made.Size = core::PointF{static_cast<float>(Size.X) / ITEM_POINT, static_cast<float>(Size.Y) / ITEM_POINT};
            return Made;
        }
        case ITEM_EVENT_REFUSED: {
            ItemRefusal Why{};
            if (Bytes < sizeof(Why)) continue;
            std::memcpy(&Why, Body, sizeof(Why));
            Made.What     = Event::Kind::Refused;
            Made.Op       = Why.Op;
            Made.Property = Why.Property;
            Made.Reason   = static_cast<error_t>(Why.Reason);
            return Made;
        }
        case ITEM_EVENT_SHOWN: {
            ItemShown Shown{};
            if (Bytes < sizeof(Shown)) continue;
            std::memcpy(&Shown, Body, sizeof(Shown));
            Made.What    = Event::Kind::Shown;
            Made.Step    = Shown.Step;
            Made.Micros  = Shown.Micros;
            Made.Pixels  = Shown.Pixels;
            Made.Bytes   = Shown.Bytes;
            Made.Records = Shown.Records;
            return Made;
        }
        case ITEM_EVENT_FREE: {
            ItemPageFree Free{};
            if (Bytes < sizeof(Free)) continue;
            std::memcpy(&Free, Body, sizeof(Free));
            Made.What = Event::Kind::PageFree;
            Made.Page = Free.Page;
            return Made;
        }
        default:
            continue;   /* a pad, or what a later display says: passed over */
        }
    }
}

std::expected<box::bay<std::byte>, box::error> Area::Block(std::uint8_t Kind2, std::uint32_t Width, std::uint32_t Height,
                                                           std::uint32_t Bytes, std::uint32_t &Id)
{
    DisplayBlockOpenRequest Request{};
    Request.command = DISP_CMD_BLOCK_OPEN;
    Request.kind    = Kind2;
    Request.area    = AreaId;
    Request.width   = Width;
    Request.height  = Height;
    Request.bytes   = Bytes;
    DisplayBlockOpenReply Reply{};
    std::uint32_t         Got  = 0;
    const int             Code = ask(DISPLAY_TAG, &Request, sizeof(Request), &Reply, sizeof(Reply), &Got);
    if (Code != 0) return std::unexpected(Cause(Code));
    if (Got != sizeof(Reply) || Reply.command != DISP_CMD_BLOCK_OPEN) return std::unexpected(box::error{box::errc::corrupted});
    if (Reply.error != 0) return std::unexpected(box::error{static_cast<error_t>(Reply.error)});
    if (Reply.block == 0 || std::memchr(Reply.bay_tag, '\0', sizeof(Reply.bay_tag)) == nullptr)
        return std::unexpected(box::error{box::errc::corrupted});
    Id = Reply.block;
    box::bay<std::byte> Memory2 = box::bay<std::byte>::open(Reply.bay_tag);
    if (!Memory2) {
        Tell(DISP_CMD_BLOCK_CLOSE, Id);
        return std::unexpected(box::error{box::errc::tag_not_found});
    }
    const ItemBlockHeader *Head2 = HeadOf(Memory2);
    if (Memory2.size_bytes() < sizeof(ItemBlockHeader) || Head2->Magic != ITEM_BLOCK_MAGIC || Head2->Kind != Kind2) {
        Tell(DISP_CMD_BLOCK_CLOSE, Id);
        return std::unexpected(box::error{box::errc::corrupted});
    }
    return Memory2;
}

std::expected<PictureBlock, box::error> Area::OpenPicture(std::uint32_t Width, std::uint32_t Height)
{
    PictureBlock Made;
    auto         Memory2 = Block(ITEM_BLOCK_PICTURE, Width, Height, 0, Made.Id);
    if (!Memory2) return std::unexpected(Memory2.error());
    if (Memory2->size_bytes() < sizeof(ItemBlockHeader) + static_cast<std::size_t>(Width) * Height * 4u) {
        Tell(DISP_CMD_BLOCK_CLOSE, Made.Id);
        return std::unexpected(box::error{box::errc::corrupted});
    }
    Made.Width  = Width;
    Made.Height = Height;
    Made.Memory = std::move(*Memory2);
    return Made;
}

std::expected<CanvasBlock, box::error> Area::OpenCanvas(std::uint32_t Width, std::uint32_t Height)
{
    CanvasBlock Made;
    auto        Memory2 = Block(ITEM_BLOCK_CANVAS, Width, Height, 0, Made.Id);
    if (!Memory2) return std::unexpected(Memory2.error());
    const std::size_t Page = static_cast<std::size_t>(Width) * Height * sizeof(core::Pixel);
    if (HeadOf(*Memory2)->PageBytes < Page || Memory2->size_bytes() < sizeof(ItemBlockHeader) + 2 * Page) {
        Tell(DISP_CMD_BLOCK_CLOSE, Made.Id);
        return std::unexpected(box::error{box::errc::corrupted});
    }
    Made.Width  = Width;
    Made.Height = Height;
    Made.Memory = std::move(*Memory2);
    return Made;
}

std::expected<std::uint32_t, box::error> Area::OpenFont(std::span<const std::uint8_t> Bytes)
{
    std::uint32_t Id      = 0;
    auto          Memory2 = Block(ITEM_BLOCK_FONT, 0, 0, static_cast<std::uint32_t>(Bytes.size()), Id);
    if (!Memory2) return std::unexpected(Memory2.error());
    if (Memory2->size_bytes() < sizeof(ItemBlockHeader) + Bytes.size()) {
        Tell(DISP_CMD_BLOCK_CLOSE, Id);
        return std::unexpected(box::error{box::errc::corrupted});
    }
    std::memcpy(reinterpret_cast<std::uint8_t *>(Memory2->data()) + sizeof(ItemBlockHeader), Bytes.data(), Bytes.size());
    /* The display reads it once told, with the next step. */
    BlockChanged(Id, core::Rect{0, 0, 0, 0});
    return Id;
}

void Area::CloseBlock(std::uint32_t Block2) { Tell(DISP_CMD_BLOCK_CLOSE, Block2); }

}  // namespace painter::gui
