#include "layers.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <utility>

#include "box/core/cabin.h"
#include "box/cxx/bay.h"
#include "box/ipc.h"
#include "boxos_glyph_fold.h"

#include "front.h"

namespace {

struct DisplayLayerState {
    std::uint32_t Id = 0;
    std::uint32_t OwnerPid = 0;
    std::uint32_t OwnerGeneration = 0;
    std::uint32_t ProgramPid = 0;
    std::uint32_t ProgramGeneration = 0;
    std::uint32_t BayNumber = 0;
    DisplayLayerRole Role = DISPLAY_LAYER_OVERLAY;
    std::int32_t X = 0;
    std::int32_t Y = 0;
    std::uint32_t Width = 0;
    std::uint32_t Height = 0;
    std::size_t CellCount = 0;
    box::bay<std::byte> Shared;
    DisplayLayerSharedHeader *Header = nullptr;
    std::vector<DisplayLayerCell> Cells;
    std::uint64_t Sequence = 0;
    std::uint32_t CursorX = 0;
    std::uint32_t CursorY = 0;
    bool CursorVisible = false;
    bool HasFrame = false;
};

std::vector<DisplayLayerState> g_layers;
std::vector<std::size_t> g_order;   // composition order, kept between presents
std::uint32_t g_next_layer_id = 1;

// The N of "display:layer:N" no layer holds now, and the next N never used. A
// Bay's name is a tag the volume keeps for good, so the names are reused: the
// volume learns as many as were ever shown at once, not one for every layer
// opened. The pool has room for every number there is — made when the number
// is — so giving one back never allocates.
std::vector<std::uint32_t> g_free_bay_numbers;
std::uint32_t g_next_bay_number;

DisplayLayerCell *LayerSlot(DisplayLayerState &Layer, std::uint32_t Slot) noexcept
{
    auto *Base = reinterpret_cast<DisplayLayerCell *>(
        reinterpret_cast<std::byte *>(Layer.Header) + sizeof(DisplayLayerSharedHeader));
    return Base + static_cast<std::size_t>(Slot) * Layer.CellCount;
}

// Whether the layer's picture is opaque at that cell of the screen.
bool LayerCovers(const DisplayLayerState &Layer, std::uint32_t ScreenX, std::uint32_t ScreenY) noexcept
{
    const std::int64_t Col = static_cast<std::int64_t>(ScreenX) - Layer.X;
    const std::int64_t Row = static_cast<std::int64_t>(ScreenY) - Layer.Y;
    if (Col < 0 || Row < 0 || Col >= Layer.Width || Row >= Layer.Height) return false;
    return Layer.Cells[static_cast<std::size_t>(Row) * Layer.Width + static_cast<std::size_t>(Col)].Opaque != 0;
}

// The newest READY slot becomes the layer's picture; every slot read is
// handed back FREE.
bool LayerConsume(DisplayLayerState &Layer) noexcept
{
    bool Changed = false;
    std::uint64_t NewestSequence = Layer.Sequence;
    for (std::uint32_t Slot = 0; Slot < DISPLAY_LAYER_SLOTS; Slot++) {
        std::uint32_t Expected = DISPLAY_LAYER_READY;
        if (!__atomic_compare_exchange_n(&Layer.Header->slot_state[Slot], &Expected,
                                          DISPLAY_LAYER_READING, false,
                                          __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
            continue;

        const std::uint64_t Sequence = Layer.Header->sequence[Slot];
        if (!Layer.HasFrame || Sequence > NewestSequence) {
            std::memcpy(Layer.Cells.data(), LayerSlot(Layer, Slot),
                        Layer.CellCount * sizeof(DisplayLayerCell));
            NewestSequence = Sequence;
            Layer.CursorX = Layer.Header->cursor_x[Slot];
            Layer.CursorY = Layer.Header->cursor_y[Slot];
            Layer.CursorVisible = Layer.Header->cursor_visible[Slot] != 0;
            Layer.HasFrame = true;
            Changed = true;
        }
        __atomic_store_n(&Layer.Header->slot_state[Slot], DISPLAY_LAYER_FREE, __ATOMIC_RELEASE);
    }
    Layer.Sequence = NewestSequence;
    return Changed;
}

bool LayerAbove(const DisplayLayerState &Left, const DisplayLayerState &Right) noexcept
{
    if (Left.Role != Right.Role) return Left.Role < Right.Role;
    return Left.Id < Right.Id;
}

// The answer to an open: the layer, or why there is none; the console's size
// and caret either way. Anything but OK back means the asker never learned of
// what the answer names.
int Reply(const Result &Asked, error_t Error, const DisplayLayerState *Layer,
          const ConsoleModel &Model) noexcept
{
    DisplayLayerOpenReply Answer{};
    Answer.command = DISP_CMD_LAYER_OPEN;
    Answer.error = static_cast<std::int32_t>(Error);
    Answer.display_pid = cabin_info()->pid;
    Answer.cols = Model.Width();
    Answer.rows = Model.Height();
    Answer.cursor_x = Model.CursorX();
    Answer.cursor_y = Model.CursorY();
    if (Layer) {
        Answer.layer_id = Layer->Id;
        std::snprintf(Answer.bay_tag, sizeof(Answer.bay_tag), "%s%u", DISPLAY_LAYER_TAG_PREFIX,
                      Layer->BayNumber);
    }
    return answer(&Asked, &Answer, sizeof(Answer));
}

// The area a program already shows in one role, over all its strands: one
// screen of picture at one depth is all there is to show, so it is all one
// program may hold.
std::uint64_t AreaHeld(std::uint32_t ProgramPid, std::uint32_t ProgramGeneration,
                       DisplayLayerRole Role) noexcept
{
    std::uint64_t Area = 0;
    for (const DisplayLayerState &Layer : g_layers)
        if (Layer.ProgramPid == ProgramPid && Layer.ProgramGeneration == ProgramGeneration &&
            Layer.Role == Role)
            Area += static_cast<std::uint64_t>(Layer.Width) * Layer.Height;
    return Area;
}

// The layer's memory, made for the asker's program alone under a reused name.
// A pooled name whose Bay a program still holds — it has not let go of an
// earlier layer yet — is passed over and kept for later; so is a fresh one
// somebody else made first. Throws std::bad_alloc only, making room for a
// fresh number before it is used.
std::expected<box::bay<std::byte>, box::error> LayerMemory(std::size_t Bytes, const AskFrom &From,
                                                          std::uint32_t &Number)
{
    auto Make = [&](std::uint32_t Candidate) {
        char Tag[DISPLAY_LAYER_TAG_MAX];
        std::snprintf(Tag, sizeof(Tag), "%s%u", DISPLAY_LAYER_TAG_PREFIX, Candidate);
        return box::bay<std::byte>::create_for(Tag, Bytes, From.program_pid,
                                               From.program_generation);
    };

    for (std::size_t Index = g_free_bay_numbers.size(); Index-- > 0;) {
        std::expected<box::bay<std::byte>, box::error> Made = Make(g_free_bay_numbers[Index]);
        if (Made) {
            Number = g_free_bay_numbers[Index];
            g_free_bay_numbers.erase(g_free_bay_numbers.begin() +
                                     static_cast<std::ptrdiff_t>(Index));
            return Made;
        }
        if (Made.error().raw() != ERR_ALREADY_EXISTS) return Made;
    }
    for (;;) {
        const std::uint32_t Fresh = g_next_bay_number;
        g_free_bay_numbers.reserve(static_cast<std::size_t>(Fresh) + 1u);
        g_next_bay_number++;
        std::expected<box::bay<std::byte>, box::error> Made = Make(Fresh);
        if (Made) {
            Number = Fresh;
            return Made;
        }
        g_free_bay_numbers.push_back(Fresh);
        if (Made.error().raw() != ERR_ALREADY_EXISTS) return Made;
    }
}

}  // namespace

void LayersOpen(const Result &Asked, const AskFrom &From, const DisplayLayerOpenRequest &Request,
                const ConsoleModel &Model) noexcept
{
    const std::uint32_t Width = Model.Width();
    const std::uint32_t Height = Model.Height();
    if (Request.width == 0 || Request.height == 0 || Request.x < 0 || Request.y < 0 ||
        Request.width > Width || Request.height > Height ||
        static_cast<std::uint32_t>(Request.x) > Width - Request.width ||
        static_cast<std::uint32_t>(Request.y) > Height - Request.height) {
        (void)Reply(Asked, ERR_OUT_OF_RANGE, nullptr, Model);
        return;
    }
    if (Request.role < DISPLAY_LAYER_SCENE || Request.role > DISPLAY_LAYER_NOTICE ||
        Request.flags != 0) {
        (void)Reply(Asked, ERR_INVALID_ARGUMENT, nullptr, Model);
        return;
    }
    const DisplayLayerRole Role = static_cast<DisplayLayerRole>(Request.role);

    const std::size_t CellCount = static_cast<std::size_t>(Request.width) * Request.height;
    if (AreaHeld(From.program_pid, From.program_generation, Role) + CellCount >
        static_cast<std::uint64_t>(Width) * Height) {
        (void)Reply(Asked, ERR_QUOTA_EXCEEDED, nullptr, Model);
        return;
    }

    DisplayLayerState Layer;
    try {
        std::expected<box::bay<std::byte>, box::error> Shared =
            LayerMemory(sizeof(DisplayLayerSharedHeader) +
                            DISPLAY_LAYER_SLOTS * CellCount * sizeof(DisplayLayerCell),
                        From, Layer.BayNumber);
        if (!Shared) {
            (void)Reply(Asked, Shared.error().raw(), nullptr, Model);
            return;
        }
        Layer.Shared = std::move(*Shared);
        Layer.Cells.resize(CellCount);
    } catch (...) {
        if (Layer.Shared) g_free_bay_numbers.push_back(Layer.BayNumber);
        (void)Reply(Asked, ERR_NO_MEMORY, nullptr, Model);
        return;
    }

    auto *Header = reinterpret_cast<DisplayLayerSharedHeader *>(Layer.Shared.data());
    Header->magic = DISPLAY_LAYER_MAGIC;
    Header->version = DISPLAY_LAYER_VERSION;
    Header->width = Request.width;
    Header->height = Request.height;
    Header->cell_count = static_cast<std::uint32_t>(CellCount);
    Header->slot_count = DISPLAY_LAYER_SLOTS;

    Layer.Id = g_next_layer_id++;
    if (Layer.Id == 0) Layer.Id = g_next_layer_id++;
    Layer.OwnerPid = Asked.sender_pid;
    Layer.OwnerGeneration = From.generation;
    Layer.ProgramPid = From.program_pid;
    Layer.ProgramGeneration = From.program_generation;
    Layer.Role = Role;
    Layer.X = Request.x;
    Layer.Y = Request.y;
    Layer.Width = Request.width;
    Layer.Height = Request.height;
    Layer.CellCount = CellCount;
    Layer.Header = Header;
    try {
        g_layers.push_back(std::move(Layer));
    } catch (...) {
        g_free_bay_numbers.push_back(Layer.BayNumber);
        (void)Reply(Asked, ERR_NO_MEMORY, nullptr, Model);
        return;
    }

    // A layer whose answer reached nobody — its asker died first — is
    // nobody's.
    if (Reply(Asked, OK, &g_layers.back(), Model) != 0) {
        g_free_bay_numbers.push_back(g_layers.back().BayNumber);
        g_layers.pop_back();
    }
}

bool LayersFrame(std::uint32_t Requester, std::uint32_t Generation, std::uint32_t LayerId) noexcept
{
    for (DisplayLayerState &Layer : g_layers) {
        if (Layer.Id != LayerId || Layer.OwnerPid != Requester ||
            Layer.OwnerGeneration != Generation)
            continue;
        return LayerConsume(Layer);
    }
    return false;
}

// The generation is the message's own claim, and it is enough: the pid is the
// kernel's stamp, so only a later life of the same pid could claim an earlier
// one's layer — a layer of the dead, going anyway.
bool LayersClose(std::uint32_t Requester, std::uint32_t Generation, std::uint32_t LayerId) noexcept
{
    for (auto It = g_layers.begin(); It != g_layers.end(); ++It) {
        if (It->Id != LayerId) continue;
        if (It->OwnerPid != Requester || It->OwnerGeneration != Generation) return false;
        g_free_bay_numbers.push_back(It->BayNumber);
        g_layers.erase(It);
        return true;
    }
    return false;
}

std::expected<LayerCommit, error_t> LayersCommit(std::uint32_t Pid, std::uint32_t Generation,
                                                 const DisplayLayerCommitRequest &Request,
                                                 const ConsoleModel &Model) noexcept
{
    const auto It = std::find_if(g_layers.begin(), g_layers.end(), [&](const DisplayLayerState &Layer) {
        return Layer.Id == Request.layer_id;
    });
    if (It == g_layers.end()) return std::unexpected(ERR_INVALID_STATE);
    if (It->OwnerPid != Pid || It->OwnerGeneration != Generation)
        return std::unexpected(ERR_PERMISSION_DENIED);
    (void)LayerConsume(*It);
    if (!It->HasFrame) return std::unexpected(ERR_INVALID_STATE);

    // The layer's place was checked against this console when it opened; what
    // the program wrote since — the scroll it asks for, the caret in its
    // memory — is checked here, where the fold is decided.
    if (Request.scroll_up > Model.Height() ||
        (It->CursorVisible && (It->CursorX >= It->Width || It->CursorY >= It->Height)))
        return std::unexpected(ERR_INVALID_ARGUMENT);

    LayerCommit Commit;
    try {
        // The fold as the log ring will have it: what the layer itself shows
        // — its glyph where a cell is opaque, a blank where it lets the text
        // beneath through, for that text is in the account already — each row
        // without its trailing blanks and on a line of its own, the rows that
        // show nothing above and below the picture left out, and the first on
        // a line of its own too when the caret stands inside a line.
        const auto Blank = [](char Glyph) { return Glyph == ' ' || Glyph == '\0' || Glyph == '\xFF'; };
        const auto Shows = [&](std::uint32_t Row) {
            for (std::uint32_t Col = 0; Col < It->Width; Col++) {
                const DisplayLayerCell &Cell =
                    It->Cells[static_cast<std::size_t>(Row) * It->Width + Col];
                if (Cell.Opaque && !Blank(Cell.Cell.ch)) return true;
            }
            return false;
        };
        std::uint32_t First = 0;
        while (First < It->Height && !Shows(First)) First++;
        std::uint32_t Last = It->Height;
        while (Last > First && !Shows(Last - 1u)) Last--;

        std::string &Text = Commit.Text;
        if (First < Last && Model.CursorX() != 0) Text += '\n';
        for (std::uint32_t Row = First; Row < Last; Row++) {
            if (Row != First) Text += '\n';
            std::size_t Kept = Text.size();
            for (std::uint32_t Col = 0; Col < It->Width; Col++) {
                const DisplayLayerCell &Cell =
                    It->Cells[static_cast<std::size_t>(Row) * It->Width + Col];
                const char Glyph = Cell.Opaque ? Cell.Cell.ch : ' ';
                Text += BoxGlyphUnicode(static_cast<unsigned char>(Glyph));
                if (!Blank(Glyph)) Kept = Text.size();
            }
            Text.resize(Kept);
        }
        // And the account goes on where the fold leaves the caret: on the
        // row below the picture, when the layer puts it there, as the glass
        // does. A caret left inside the picture has no place a line can go
        // back to, and what follows it is said after the picture's last row.
        if (First < Last && It->CursorVisible && It->CursorY >= Last) {
            Text.append(It->CursorY - (Last - 1u), '\n');
            Text.append(It->CursorX, ' ');
        }
    } catch (...) {
        return std::unexpected(ERR_NO_MEMORY);
    }

    Commit.X = It->X;
    Commit.Y = It->Y;
    Commit.Width = It->Width;
    Commit.Height = It->Height;
    Commit.Cells = std::move(It->Cells);
    Commit.ScrollRows = Request.scroll_up;
    Commit.CursorX = It->CursorX;
    Commit.CursorY = It->CursorY;
    Commit.CursorVisible = It->CursorVisible;
    g_free_bay_numbers.push_back(It->BayNumber);
    g_layers.erase(It);
    return Commit;
}

bool LayersDropProcess(std::uint32_t Pid, std::uint32_t Generation) noexcept
{
    bool Dropped = false;
    for (auto It = g_layers.begin(); It != g_layers.end();) {
        if (It->OwnerPid != Pid || It->OwnerGeneration != Generation) {
            ++It;
            continue;
        }
        g_free_bay_numbers.push_back(It->BayNumber);
        It = g_layers.erase(It);
        Dropped = true;
    }
    return Dropped;
}

bool LayersStep() noexcept
{
    bool Changed = false;
    for (DisplayLayerState &Layer : g_layers) Changed |= LayerConsume(Layer);
    return Changed;
}

void LayersCompose(const ConsoleModel &Model, std::vector<TextCell> &Composed, Caret &At) noexcept
{
    const std::vector<TextCell> &Base = Model.Cells();
    std::copy(Base.begin(), Base.end(), Composed.begin());
    At = Caret{Model.CursorX(), Model.CursorY(), true};

    // A program behind (front.h) shows no scene and no overlay — the glass is
    // the program's in front — and keeps its notices, which are made to be
    // seen beside whatever is in front.
    g_order.clear();
    try {
        for (std::size_t Index = 0; Index < g_layers.size(); Index++) {
            const DisplayLayerState &Layer = g_layers[Index];
            if (!Layer.HasFrame) continue;
            if (Layer.Role != DISPLAY_LAYER_NOTICE && FrontBehind(Layer.ProgramPid, Layer.ProgramGeneration)) continue;
            g_order.push_back(Index);
        }
    } catch (...) {
        // No room to order the layers: the console's text alone this time;
        // the next present asks again.
        g_order.clear();
    }
    std::sort(g_order.begin(), g_order.end(), [](std::size_t A, std::size_t B) {
        return LayerAbove(g_layers[A], g_layers[B]);
    });

    const std::uint32_t Width = Model.Width();
    const std::uint32_t Height = Model.Height();
    for (const std::size_t Ordered : g_order) {
        const DisplayLayerState &Layer = g_layers[Ordered];
        for (std::uint32_t Row = 0; Row < Layer.Height; Row++) {
            for (std::uint32_t Col = 0; Col < Layer.Width; Col++) {
                const DisplayLayerCell &Cell =
                    Layer.Cells[static_cast<std::size_t>(Row) * Layer.Width + Col];
                if (!Cell.Opaque) continue;
                const std::uint32_t ScreenX = static_cast<std::uint32_t>(Layer.X) + Col;
                const std::uint32_t ScreenY = static_cast<std::uint32_t>(Layer.Y) + Row;
                Composed[static_cast<std::size_t>(ScreenY) * Width + ScreenX] = Cell.Cell;
            }
        }
        if (Layer.CursorVisible) {
            At.X       = std::min(static_cast<std::uint32_t>(Layer.X) + Layer.CursorX, Width - 1u);
            At.Y       = std::min(static_cast<std::uint32_t>(Layer.Y) + Layer.CursorY, Height - 1u);
            At.Visible = true;
        } else if (At.Visible && LayerCovers(Layer, At.X, At.Y)) {
            At.Visible = false;
        }
    }
}
