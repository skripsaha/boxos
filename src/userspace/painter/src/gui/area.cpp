/*
 * area.cpp — a program's area on the screen. See painter/gui/area.h.
 */

#include "painter/gui/area.h"

#include <algorithm>
#include <cstring>
#include <utility>

#include "box/alarm.h"
#include "box/core/strand_self.h"
#include "box/display.h"
#include "box/error.h"
#include "box/ipc.h"
#include "box/print.h"
#include "box/system.h"
#include "box/turnin.h"
#include "font_opened.h"
#include "painter/tui/keyboard.h"

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
    return core::Canvas::Over(PageAt(Page), Width, Height, Width);
}

core::Pixel *CanvasBlock::PageAt(std::uint32_t Page) noexcept
{
    if (!Memory || Page > 1) return nullptr;
    return reinterpret_cast<core::Pixel *>(reinterpret_cast<std::uint8_t *>(Memory.data()) + sizeof(ItemBlockHeader) +
                                           static_cast<std::size_t>(Page) * HeadOf(Memory)->PageBytes);
}

std::size_t CanvasBlock::PageBytes() const noexcept { return Memory ? HeadOf(Memory)->PageBytes : 0; }

void CanvasBlock::Taken(std::uint32_t Page) noexcept
{
    if (!Memory || Page > 1) return;
    auto *Head = reinterpret_cast<ItemBlockHeader *>(Memory.data());
    __atomic_store_n(&Head->PageFree[Page], 0u, __ATOMIC_RELEASE);
}

std::expected<Area, box::error> Area::Open(Placement Where, core::PointF Wish, bool Always)
{
    DisplayAreaOpenRequest Request{};
    Request.command   = DISP_CMD_AREA_OPEN;
    Request.placement = static_cast<std::uint8_t>(Where);
    Request.flags     = Always ? DISPLAY_AREA_ALWAYS : 0;
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
    Made.MosaicFlags    = Reply.mosaics;
    Made.Less           = (Reply.motion & ITEM_MOTION_LESS) != 0;
    Made.Speed          = Reply.speed ? Reply.speed : 100;
    Made.CaretAtOpen    = core::Point{static_cast<std::int32_t>(std::min<std::uint32_t>(Reply.caret_col, ITEM_GRID_SIDE_MOST)),
                                      static_cast<std::int32_t>(std::min<std::uint32_t>(Reply.caret_row, ITEM_GRID_SIDE_MOST))};
    Made.Memory         = box::bay<std::byte>::open(Reply.bay_tag);
    if (!Made.Memory) return std::unexpected(box::error{box::errc::tag_not_found});
    Made.Head = reinterpret_cast<ItemAreaHeader *>(Made.Memory.data());
    if (Made.Memory.size_bytes() < sizeof(ItemAreaHeader) + ITEM_RING_BYTES + ITEM_EVENT_BYTES ||
        Made.Head->Magic != ITEM_AREA_MAGIC || Made.Head->Bytes != ITEM_RING_BYTES || Made.Head->EventBytes != ITEM_EVENT_BYTES)
        return std::unexpected(box::error{box::errc::corrupted});
    Made.BellName = touch_intern(Reply.bay_tag);
    Made.Listen(true);
    return Made;
}

void Area::Listen(bool On) noexcept
{
    if (On == (Rung != TOUCH_TAG_INVALID)) return;
    if (On) {
        Rung = touch_pair_choose(BellName);
        if (Rung != TOUCH_TAG_INVALID && touch_claim(Rung, TOUCH_REST, 0, 0) != 0) Rung = TOUCH_TAG_INVALID;
        return;
    }
    /* Let go, with every touch it already brought taken. */
    Touch Heard;
    while (touch_try_pop_tag(Rung, &Heard)) {
    }
    touch_release(Rung);
    Rung = TOUCH_TAG_INVALID;
}

Area::Area() : Anchored(std::make_shared<detail::Anchor>(detail::Anchor{this, &Pool, this, nullptr})) {}

Area::Area(Area &&Other) noexcept
    : Changes(std::move(Other)), AreaId(Other.AreaId), DisplayPid(Other.DisplayPid), Memory(std::move(Other.Memory)),
      Head(Other.Head), Given(Other.Given), PixelsPerPoint(Other.PixelsPerPoint), Kind(Other.Kind),
      MosaicFlags(Other.MosaicFlags), CaretAtOpen(Other.CaretAtOpen), BellName(Other.BellName), Rung(Other.Rung),
      Steps(Other.Steps), Opened(Other.Opened), Pool(std::move(Other.Pool)), Anchored(std::move(Other.Anchored)),
      Queued(std::move(Other.Queued)), Frames(std::move(Other.Frames))
{
    /* Its items' words follow it. */
    if (Anchored) Anchored->Into = this, Anchored->Pool = &Pool, Anchored->Owner = this;
    Other.Opened = false;
    Other.Head   = nullptr;
    Other.Rung   = TOUCH_TAG_INVALID;
}

Area::~Area()
{
    /* Its items' objects say nothing from now. */
    if (Anchored) Anchored->Into = nullptr, Anchored->Pool = nullptr, Anchored->Owner = nullptr;
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

box::status Area::Commit(std::uint32_t ScrollRows)
{
    if (!Head || !Opened) return std::unexpected(box::error{box::errc::invalid_state});
    /* What changed goes first, all of it: the fold takes the cells as the
     * program last said them. The area goes, so what it told is no one's. */
    while (Next()) {
    }
    PresentAll();
    DisplayAreaCommitRequest Request{};
    Request.command     = DISP_CMD_AREA_COMMIT;
    Request.area        = AreaId;
    Request.scroll_rows = ScrollRows;
    Request.order       = console_order_mark();
    DisplayAreaCommitReply Reply{};
    std::uint32_t          Got  = 0;
    const int              Code = ask(DISPLAY_TAG, &Request, sizeof(Request), &Reply, sizeof(Reply), &Got);
    if (Code != 0) return std::unexpected(Cause(Code));
    if (Got != sizeof(Reply) || Reply.command != DISP_CMD_AREA_COMMIT) return std::unexpected(box::error{box::errc::corrupted});
    if (Reply.error != 0) return std::unexpected(box::error{static_cast<error_t>(Reply.error)});
    /* The display holds the area no more. */
    Opened = false;
    Head   = nullptr;
    Memory = {};
    return {};
}

void Area::PresentAll()
{
    if (Present()) return;
    const bool Heard = Rung != TOUCH_TAG_INVALID;
    Listen(true);
    while (!Present()) {
        Touch Rang;
        if (Rung == TOUCH_TAG_INVALID || !touch_wait_tag(Rung, &Rang, 0)) yield();
    }
    if (!Heard) Listen(false);
}

std::optional<Event> Area::Next()
{
    if (!Queued.empty()) {
        Event Told = Queued.front();
        Queued.pop_front();
        return Told;
    }
    return Read();
}

void Area::Gather()
{
    Touch Rang;
    if (Rung != TOUCH_TAG_INVALID)
        while (touch_try_pop_tag(Rung, &Rang)) {
        }
    try {
        while (std::optional<Event> Told = Read()) Queued.push_back(*Told);
    } catch (...) {
        // No room to keep what it told: what is still in the ring waits there.
    }
}

Event Area::Wait(tui::Keyboard *Keys)
{
    Listen(true);
    for (;;) {
        /* The mark first, then the look: what arrives after it wakes the
         * turn-in (box/turnin.h). */
        const TurnInMark Mark = box_mark();
        Gather();
        if (std::optional<Event> Told = Next()) return *Told;
        if (Keys) {
            const tui::Event Heard = Keys->Poll();
            Event            Made;
            switch (Heard.Kind) {
            case tui::EventKind::Key: Made.What = Event::Kind::Key, Made.Key = Heard.Key; return Made;
            case tui::EventKind::Gone: Made.What = Event::Kind::Gone; return Made;
            case tui::EventKind::EndAsked: Made.What = Event::Kind::EndAsked; return Made;
            case tui::EventKind::Quiet: break;
            }
        }
        std::uint64_t Moment = 0;
        if (alarm_heard(&Moment)) {
            Event Made;
            Made.What   = Event::Kind::Alarm;
            Made.Moment = Moment;
            return Made;
        }
        box_turn_in(Mark);
    }
}

std::optional<Event> Area::Read()
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
            Less           = (News.Motion & ITEM_MOTION_LESS) != 0;
            Speed          = News.Speed ? News.Speed : Speed;
            Made.What        = Event::Kind::Area;
            Made.Size        = Given;
            Made.Scale       = PixelsPerPoint;
            Made.On          = Kind;
            Made.Seen        = News.Shown != 0;
            Made.LessMotion  = Less;
            Made.MotionSpeed = Speed;
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
            /* What the display holds is what was said before: said again,
             * it goes again. */
            if (Why.Op == ITEM_OP_CHANGE) Forget(Head2.Item, Why.Property);
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
            Made.Own     = Shown.Own;
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
        case ITEM_EVENT_FOCUS: {
            ItemFocus Focus{};
            if (Bytes < sizeof(Focus)) continue;
            std::memcpy(&Focus, Body, sizeof(Focus));
            Made.What = Event::Kind::Focus;
            Made.Came = Focus.Came != 0;
            return Made;
        }
        case ITEM_EVENT_PRESS:
            Made.What = Event::Kind::Press;
            return Made;
        case ITEM_EVENT_TEXT: {
            ItemBytes Length{};
            if (Bytes < sizeof(Length)) continue;
            std::memcpy(&Length, Body, sizeof(Length));
            if (Length.Bytes > Bytes - sizeof(Length)) continue;
            Made.What = Event::Kind::Text;
            Made.Text.assign(reinterpret_cast<const char *>(Body) + sizeof(Length), Length.Bytes);
            /* What the person typed is what the field says now: said again,
             * it goes nowhere; said otherwise, it is written. */
            Changes Scratch;
            Scratch.Text(Made.Item, Made.Text);
            if (const std::vector<std::uint8_t> *Value = Scratch.Said(Made.Item, ITEM_PROP_TEXT)) Known(Made.Item, ITEM_PROP_TEXT, *Value);
            return Made;
        }
        case ITEM_EVENT_ANIMATION_END: {
            ItemAnimationEnd End{};
            if (Bytes < sizeof(End)) continue;
            std::memcpy(&End, Body, sizeof(End));
            Made.What     = Event::Kind::AnimationEnd;
            Made.Number   = End.Number;
            Made.Finished = End.Finished != 0;
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
    /* The display reads it once told, with the next step, into the program's
     * fonts; texts measured here are laid in them as well. */
    BlockChanged(Id, core::Rect{0, 0, 0, 0});
    if (detail::FontOpened) detail::FontOpened(Bytes);
    return Id;
}

void Area::CloseBlock(std::uint32_t Block2) { Tell(DISP_CMD_BLOCK_CLOSE, Block2); }

}  // namespace painter::gui
