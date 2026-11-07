#include "display_client.h"

#include <cstddef>
#include <cstring>
#include <limits>

#include "box/error.h"
#include "box/ipc.h"
#include "box/print.h"
#include "box/system.h"
#include "box/core/strand_self.h"

namespace painter::tui {

namespace {

box::error Cause(int Code)
{
    return Code == 0 ? box::error{} : box::error{box_errno_of(Code)};
}

std::size_t CellCount(core::Size Size) noexcept
{
    if (Size.Width <= 0 || Size.Height <= 0) return 0;
    const auto Width = static_cast<std::size_t>(Size.Width);
    const auto Height = static_cast<std::size_t>(Size.Height);
    if (Height > std::numeric_limits<std::size_t>::max() / Width) return 0;
    return Width * Height;
}

}  // namespace

DisplayLayer::DisplayLayer(core::Rect BoundsValue, const DisplayLayerOpenReply &Reply,
                           box::bay<std::byte> SharedValue) noexcept
    : Bounds(BoundsValue), DisplayPid(Reply.display_pid), LayerId(Reply.layer_id),
      ConsoleCursor{static_cast<std::int32_t>(Reply.cursor_x),
                    static_cast<std::int32_t>(Reply.cursor_y)},
      Shared(std::move(SharedValue)),
      Header(reinterpret_cast<DisplayLayerSharedHeader *>(Shared.data())), Opened(true)
{
}

std::expected<DisplayLayer, box::error> DisplayLayer::Open(core::Rect Bounds,
                                                            DisplayLayerRole Role) noexcept
{
    const core::Size Size{Bounds.Width, Bounds.Height};
    const std::size_t Cells = CellCount(Size);
    if (Cells == 0 || Bounds.X < 0 || Bounds.Y < 0 ||
        Cells > std::numeric_limits<std::uint32_t>::max() ||
        Cells > (std::numeric_limits<std::size_t>::max() - sizeof(DisplayLayerSharedHeader)) /
                    (DISPLAY_LAYER_SLOTS * sizeof(DisplayLayerCell)))
        return std::unexpected(box::error{box::errc::out_of_range});

    DisplayLayerOpenRequest Request{};
    Request.command = DISP_CMD_LAYER_OPEN;
    Request.role = static_cast<std::uint8_t>(Role);
    Request.x = Bounds.X;
    Request.y = Bounds.Y;
    Request.width = static_cast<std::uint32_t>(Size.Width);
    Request.height = static_cast<std::uint32_t>(Size.Height);
    // Everything this strand printed goes out first, and the mark says how
    // far that is: the display draws it before answering, so the caret it
    // names stands after it.
    Request.order = console_order_mark();

    DisplayLayerOpenReply Reply{};
    std::uint32_t Got = 0;
    const int Code = ask(DISPLAY_TAG, &Request, sizeof(Request), &Reply, sizeof(Reply), &Got);
    if (Code != 0) return std::unexpected(Cause(Code));
    if (Got != sizeof(Reply) || Reply.command != DISP_CMD_LAYER_OPEN)
        return std::unexpected(box::error{box::errc::corrupted});
    if (Reply.error != 0) return std::unexpected(box::error{static_cast<error_t>(Reply.error)});
    if (Reply.display_pid == 0 || Reply.layer_id == 0 ||
        std::memchr(Reply.bay_tag, '\0', sizeof(Reply.bay_tag)) == nullptr)
        return std::unexpected(box::error{box::errc::corrupted});

    // The display made the layer's memory and holds the layer from here on;
    // anything wrong with what it named is told back by closing the layer
    // (the destructor of the layer made below).
    DisplayLayer Layer(Bounds, Reply, box::bay<std::byte>::open(Reply.bay_tag));
    if (!Layer.Shared) return std::unexpected(box::error{box::errc::tag_not_found});

    const DisplayLayerSharedHeader *Header = Layer.Header;
    if (Layer.Shared.size_bytes() < sizeof(DisplayLayerSharedHeader) +
                                        DISPLAY_LAYER_SLOTS * Cells * sizeof(DisplayLayerCell) ||
        Header->magic != DISPLAY_LAYER_MAGIC || Header->version != DISPLAY_LAYER_VERSION ||
        Header->width != Request.width || Header->height != Request.height ||
        Header->cell_count != Cells || Header->slot_count != DISPLAY_LAYER_SLOTS)
        return std::unexpected(box::error{box::errc::corrupted});
    return Layer;
}

DisplayLayer::DisplayLayer(DisplayLayer &&Other) noexcept
    : Bounds(Other.Bounds), DisplayPid(Other.DisplayPid), LayerId(Other.LayerId),
      ConsoleCursor(Other.ConsoleCursor), Shared(std::move(Other.Shared)), Header(Other.Header),
      NextSequence(Other.NextSequence), Opened(Other.Opened)
{
    Other.Header = nullptr;
    Other.Opened = false;
}

DisplayLayer &DisplayLayer::operator=(DisplayLayer &&Other) noexcept
{
    if (this == &Other) return *this;
    (void)Close();
    Bounds = Other.Bounds;
    DisplayPid = Other.DisplayPid;
    LayerId = Other.LayerId;
    ConsoleCursor = Other.ConsoleCursor;
    Shared = std::move(Other.Shared);
    Header = Other.Header;
    NextSequence = Other.NextSequence;
    Opened = Other.Opened;
    Other.Header = nullptr;
    Other.Opened = false;
    return *this;
}

DisplayLayer::~DisplayLayer()
{
    (void)Close();
}

box::status DisplayLayer::Notify() noexcept
{
    DisplayLayerFrameNotice Notice{};
    Notice.command = DISP_CMD_LAYER_FRAME;
    Notice.layer_id = LayerId;
    Notice.generation = strand_self_generation();
    for (;;) {
        const int Code = send(DisplayPid, &Notice, sizeof(Notice));
        if (Code == 0) return {};
        const error_t CauseCode = box_errno_of(Code);
        if (CauseCode != ERR_ROUTE_TARGET_FULL && CauseCode != ERR_NO_MEMORY)
            return std::unexpected(box::error{CauseCode});
        yield();
    }
}

box::status DisplayLayer::Present(std::span<const DisplayLayerCell> Cells,
                                  core::Point Cursor, bool CursorVisible) noexcept
{
    if (!Opened || !Header) return std::unexpected(box::error{box::errc::invalid_state});
    const std::size_t Count = CellCount({Bounds.Width, Bounds.Height});
    if (Cells.size() != Count) return std::unexpected(box::error{box::errc::invalid_argument});

    // A FREE slot, or — when the display is behind and all it has not taken
    // yet are READY — the oldest of those: a frame nobody will show is the
    // one to write over, and an animation goes on rather than stopping. The
    // display holds at most one slot READING at a time, so of three one is
    // always FREE or READY; a slot it takes first is looked for again.
    std::uint32_t Slot = DISPLAY_LAYER_SLOTS;
    while (Slot == DISPLAY_LAYER_SLOTS) {
        std::uint32_t Oldest = DISPLAY_LAYER_SLOTS;
        for (std::uint32_t Index = 0; Index < DISPLAY_LAYER_SLOTS; Index++) {
            std::uint32_t Expected = DISPLAY_LAYER_FREE;
            if (__atomic_compare_exchange_n(&Header->slot_state[Index], &Expected,
                                             DISPLAY_LAYER_WRITING, false,
                                             __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
                Slot = Index;
                break;
            }
            if (Expected == DISPLAY_LAYER_READY &&
                (Oldest == DISPLAY_LAYER_SLOTS || Header->sequence[Index] < Header->sequence[Oldest]))
                Oldest = Index;
        }
        if (Slot != DISPLAY_LAYER_SLOTS || Oldest == DISPLAY_LAYER_SLOTS) continue;
        std::uint32_t Expected = DISPLAY_LAYER_READY;
        if (__atomic_compare_exchange_n(&Header->slot_state[Oldest], &Expected,
                                         DISPLAY_LAYER_WRITING, false,
                                         __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
            Slot = Oldest;
    }

    auto *Base = reinterpret_cast<DisplayLayerCell *>(
        reinterpret_cast<std::byte *>(Header) + sizeof(DisplayLayerSharedHeader));
    auto *Destination = Base + static_cast<std::size_t>(Slot) * Count;
    std::memcpy(Destination, Cells.data(), Count * sizeof(DisplayLayerCell));
    Header->cursor_x[Slot] = Cursor.X < 0 ? 0u : static_cast<std::uint32_t>(Cursor.X);
    Header->cursor_y[Slot] = Cursor.Y < 0 ? 0u : static_cast<std::uint32_t>(Cursor.Y);
    Header->cursor_visible[Slot] = CursorVisible ? 1u : 0u;
    const std::uint64_t Sequence = ++NextSequence;
    Header->sequence[Slot] = Sequence;
    __atomic_store_n(&Header->slot_state[Slot], DISPLAY_LAYER_READY, __ATOMIC_RELEASE);
    return Notify();
}

box::status DisplayLayer::Commit(std::uint32_t ScrollRows) noexcept
{
    if (!Opened || !Header) return std::unexpected(box::error{box::errc::invalid_state});
    if (ScrollRows > static_cast<std::uint32_t>(Bounds.Height))
        return std::unexpected(box::error{box::errc::out_of_range});

    // A question with this strand's mark: the display folds the layer in
    // after everything this strand printed, and answers once it is text.
    DisplayLayerCommitRequest Request{};
    Request.command = DISP_CMD_LAYER_COMMIT;
    Request.layer_id = LayerId;
    Request.scroll_up = ScrollRows;
    Request.order = console_order_mark();

    DisplayLayerCommitReply Reply{};
    std::uint32_t Got = 0;
    const int Code = ask(DISPLAY_TAG, &Request, sizeof(Request), &Reply, sizeof(Reply), &Got);
    if (Code != 0) return std::unexpected(Cause(Code));
    if (Got != sizeof(Reply) || Reply.command != DISP_CMD_LAYER_COMMIT)
        return std::unexpected(box::error{box::errc::corrupted});
    if (Reply.error != 0) return std::unexpected(box::error{static_cast<error_t>(Reply.error)});

    Opened = false;
    Header = nullptr;
    Shared = {};
    return {};
}

box::status DisplayLayer::Close() noexcept
{
    if (!Opened) return {};
    DisplayLayerCloseRequest Request{};
    Request.command = DISP_CMD_LAYER_CLOSE;
    Request.layer_id = LayerId;
    Request.generation = strand_self_generation();
    const int Code = send(DisplayPid, &Request, sizeof(Request));
    Opened = false;
    Header = nullptr;
    Shared = {};
    if (Code != 0 && box_errno_of(Code) != ERR_PROCESS_NOT_FOUND)
        return std::unexpected(box::error{box_errno_of(Code)});
    return {};
}

}  // namespace painter::tui
