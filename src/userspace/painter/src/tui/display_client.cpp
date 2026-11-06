#include "display_client.h"

#include <algorithm>
#include <cstring>
#include <limits>

#include "box/clock.h"
#include "box/cpu.h"
#include "box/error.h"
#include "box/ipc.h"
#include "box/system.h"
#include "box/core/result.h"
#include "box/core/strand_self.h"
#include "box/display_layer_tag.h"

namespace painter::tui {

namespace {

box::error Cause(int Code)
{
    return Code == 0 ? box::error{} : box::error{box_errno_of(Code)};
}

std::uint64_t NextLayerNonce() noexcept
{
    static std::uint64_t Nonce = 0;
    return ++Nonce;
}

box::result<DisplayLayerOpenReply> AwaitOpenReply(std::uint64_t Nonce)
{
    std::vector<Result> Held;
    constexpr std::uint64_t ReplyTimeoutUs = 5'000'000;
    const std::uint64_t Deadline = clock_uptime_us() + ReplyTimeoutUs;
    for (;;) {
        const std::uint64_t Now = clock_uptime_us();
        if (Now >= Deadline) {
            for (const Result &Other : Held) result_restash(&Other);
            return std::unexpected(box::error{box::errc::timeout});
        }
        const std::uint64_t RemainingUs = Deadline - Now;
        const std::uint32_t WaitMs = static_cast<std::uint32_t>(
            std::min<std::uint64_t>(1000, (RemainingUs + 999) / 1000));
        Result Raw{};
        if (!receive_wait(&Raw, WaitMs)) continue;
        if (Raw.sender_pid != 0 && Raw.data_addr != 0 &&
            Raw.data_length >= sizeof(DisplayLayerOpenReply)) {
            DisplayLayerOpenReply Reply{};
            std::memcpy(&Reply, reinterpret_cast<const void *>(static_cast<std::uintptr_t>(Raw.data_addr)),
                        sizeof(Reply));
            if (Reply.command == DISP_CMD_LAYER_OPEN && Reply.nonce == Nonce) {
                for (const Result &Other : Held) result_restash(&Other);
                return Reply;
            }
        }
        try {
            Held.push_back(Raw);
        } catch (...) {
            for (const Result &Other : Held) result_restash(&Other);
            result_restash(&Raw);
            return std::unexpected(box::error{box::errc::no_memory});
        }
    }
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

DisplayLayer::DisplayLayer(core::Rect BoundsValue, std::uint32_t DisplayPidValue,
                           std::uint32_t LayerIdValue, box::bay<std::byte> SharedValue,
                           DisplayLayerSharedHeader *HeaderValue) noexcept
    : Bounds(BoundsValue), DisplayPid(DisplayPidValue), LayerId(LayerIdValue),
      Shared(std::move(SharedValue)), Header(HeaderValue), Opened(true)
{
}

std::expected<DisplayLayer, box::error> DisplayLayer::Open(core::Rect Bounds,
                                                            DisplayLayerRole Role) noexcept
{
    const core::Size Size{Bounds.Width, Bounds.Height};
    const std::size_t Cells = CellCount(Size);
    if (Cells == 0 || Bounds.X < 0 || Bounds.Y < 0 ||
        Bounds.X > 0x7FFF || Bounds.Y > 0x7FFF ||
        Size.Width > 0xFFFF || Size.Height > 0xFFFF)
        return std::unexpected(box::error{box::errc::out_of_range});
    if (Cells > std::numeric_limits<std::uint32_t>::max())
        return std::unexpected(box::error{box::errc::out_of_range});
    if (Cells > (std::numeric_limits<std::size_t>::max() - sizeof(DisplayLayerSharedHeader)) /
                    (DISPLAY_LAYER_SLOTS * sizeof(DisplayLayerCell)))
        return std::unexpected(box::error{box::errc::out_of_range});

    const std::size_t BayBytes = sizeof(DisplayLayerSharedHeader) +
                                 DISPLAY_LAYER_SLOTS * Cells * sizeof(DisplayLayerCell);
    const std::uint32_t Pid = strand_self();
    const std::uint32_t Generation = strand_self_generation();
    const std::uint64_t Nonce = NextLayerNonce();
    std::expected<std::string, box::error> Tag =
        box::display::LayerBayTag(Pid, Generation, Nonce);
    if (!Tag) return std::unexpected(Tag.error());

    box::bay<std::byte> Shared = box::bay<std::byte>::create(Tag->c_str(), BayBytes);
    if (!Shared) return std::unexpected(box::error{box::errc::no_memory});

    auto *Header = reinterpret_cast<DisplayLayerSharedHeader *>(Shared.data());
    std::memset(Header, 0, sizeof(*Header));
    Header->magic = DISPLAY_LAYER_MAGIC;
    Header->version = DISPLAY_LAYER_VERSION;
    Header->width = static_cast<std::uint32_t>(Size.Width);
    Header->height = static_cast<std::uint32_t>(Size.Height);
    Header->cell_count = static_cast<std::uint32_t>(Cells);
    Header->slot_count = DISPLAY_LAYER_SLOTS;

    DisplayLayerOpenRequest Request{};
    Request.command = DISP_CMD_LAYER_OPEN;
    Request.role = static_cast<std::uint8_t>(Role);
    Request.generation = Generation;
    Request.nonce = Nonce;
    Request.x = static_cast<std::int16_t>(Bounds.X);
    Request.y = static_cast<std::int16_t>(Bounds.Y);
    Request.width = static_cast<std::uint16_t>(Size.Width);
    Request.height = static_cast<std::uint16_t>(Size.Height);

    const int BroadcastCode = broadcast("display", &Request, sizeof(Request));
    if (BroadcastCode != 0) return std::unexpected(Cause(BroadcastCode));
    box::result<DisplayLayerOpenReply> Reply = AwaitOpenReply(Nonce);
    if (!Reply) return std::unexpected(Reply.error());
    if (Reply->error != 0)
        return std::unexpected(box::error{static_cast<error_t>(Reply->error)});
    if (Reply->display_pid == 0 || Reply->layer_id == 0)
        return std::unexpected(box::error{box::errc::corrupted});

    return DisplayLayer(Bounds, Reply->display_pid, Reply->layer_id,
                        std::move(Shared), Header);
}

DisplayLayer::DisplayLayer(DisplayLayer &&Other) noexcept
    : Bounds(Other.Bounds), DisplayPid(Other.DisplayPid), LayerId(Other.LayerId),
      Shared(std::move(Other.Shared)), Header(Other.Header),
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

    std::uint32_t Slot = DISPLAY_LAYER_SLOTS;
    for (;;) {
        for (std::uint32_t Index = 0; Index < DISPLAY_LAYER_SLOTS; Index++) {
            std::uint32_t Expected = DISPLAY_LAYER_FREE;
            if (__atomic_compare_exchange_n(&Header->slot_state[Index], &Expected,
                                             DISPLAY_LAYER_WRITING, false,
                                             __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
                Slot = Index;
                break;
            }
        }
        if (Slot != DISPLAY_LAYER_SLOTS) break;
        return std::unexpected(box::error{box::errc::would_block});
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
