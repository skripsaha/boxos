/*
 * display.cpp — the BoxOS display program: the one hand that draws.
 *
 * The kernel lends this program the screen (box/screen.h) and draws nothing
 * itself. Everything on the glass is drawn here: the programs' text from
 * their console lanes (lanes.cpp), the kernel's own lines from the log ring
 * (ring.cpp), and the layers programs keep over both (layers.cpp), onto the
 * easel (easel.cpp).
 *
 * The screen's order IS the log ring's order. A run of a program's text is
 * said into the ring before it is drawn, and the kernel's lines the ring holds
 * before that place are drawn first — so what the glass shows, the serial line
 * carries and `logsave` writes are one account in one order.
 *
 * What a program asks of the display is a question (box/ipc.h ask), and every
 * question is answered, a refusal too: the asker stands until it is. Opening a
 * layer and folding one into the text care where in the console's order they
 * stand, so they carry the asker's mark — the number of its last frame. While
 * such a question waits, no frame numbered past the lowest waiting mark is
 * drawn, and a question is answered once its asker's frames up to its mark are
 * drawn: the caret an open names stands after the asker's text, and a fold
 * lands after everything numbered before it and before everything after.
 * Waiting cannot stick: the lowest mark's own frames lie below the ceiling,
 * and a mark that lies holds no one but its asker.
 *
 * One event loop, no timeouts: a rotation of try-steps (keys → deaths →
 * screen turns → lanes → ring → layers → IPC). A pass that moves nothing does
 * not go round again — the program turns in and stops costing a core until
 * something arrives for it. A Touch and a Result the kernel sees land, so the
 * sleep watches those cursors itself; a lane frame and a kernel line are not
 * Results, and that is what the bells are for: before going down, the program
 * hangs its pid on every lane and on the ring, and whoever adds to one rings.
 * Hang the bells BEFORE the last look, or what arrives in between is seen by
 * nobody. And every answer from the kernel is a Result, so one landing after
 * the mark refuses the sleep it was taken for: in the last look only a step
 * that has moved something calls into the kernel, and then the pass goes
 * round again rather than sleeping.
 */

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#include "box/core/cabin.h"
#include "box/core/result.h"
#include "box/debug.h"
#include "box/error.h"
#include "box/ipc.h"
#include "box/life.h"
#include "box/print.h"
#include "box/touch.h"
#include "box/turnin.h"
#include "boxos_utf8.h"

#include "console_model.h"
#include "easel.h"
#include "front.h"
#include "lanes.h"
#include "layers.h"
#include "ring.h"

BOX_LIFE("serves\nrise");

namespace {

std::optional<Easel> g_easel;
ConsoleModel *g_model;
RingFollower g_ring;
std::vector<GlassCell> g_composed;
bool g_dirty;                      // the picture changed since it was last presented
TouchTag g_died = TOUCH_TAG_INVALID;

// The frames one render pass has met, in the console's order, waiting for
// their text to take its place in the ring: the text of all of them, as one
// run said at once, and the frames themselves with the lane each came on.
struct HeldRun {
    ConsoleRun    Run;
    std::uint32_t Lane;
};
std::vector<HeldRun> g_held;
char g_said[LOG_SAY_MAX];
std::uint32_t g_said_length;

// A question that cares about the console's order: asked (Asked) by the
// strand From names, with the number of that strand's last frame (Mark), and
// what it asks — Open or Commit, by Command.
struct WaitingQuestion {
    Result Asked;
    AskFrom From;
    std::uint64_t Mark;
    std::uint8_t Command;
    DisplayLayerOpenRequest Open;
    DisplayLayerCommitRequest Commit;
};

// The waiting questions, lowest mark first: they are answered in that order
// and no other, so each lands at its own place in the console's order.
std::vector<WaitingQuestion> g_waiting;

// The merge's ceiling: no frame numbered past the lowest waiting mark is
// drawn until that question is answered.
std::uint64_t Ceiling() noexcept
{
    return g_waiting.empty() ? UINT64_MAX : g_waiting.front().Mark;
}

// Text said into the log ring and drawn where it went: the kernel's lines the
// ring holds before its place drawn first, then Draw — what the text is on the
// screen — then the ring followed past it. Text the ring would not take is
// still drawn: the screen does not go blind because the log did. Text longer
// than one utterance goes in pieces, and a kernel line that falls between two
// stands between them in the ring and after Draw on the glass — what Draw puts
// there is a picture, the ring a line. With nothing to say, Draw follows
// everything the ring holds now: what the kernel said before it is not drawn
// after it.
template <typename DrawFunction>
void Utter(std::string_view Text, DrawFunction &&Draw)
{
    if (Text.empty()) {
        g_dirty |= g_ring.CatchUp(*g_model, UINT64_MAX);
        Draw();
        return;
    }

    bool Drawn = false;
    std::size_t Offset = 0;
    while (Offset < Text.size()) {
        const std::size_t Left = Text.size() - Offset;
        std::size_t Piece = Left;
        if (Piece > LOG_SAY_MAX) Piece = BoxUtf8Whole(Text.data() + Offset, LOG_SAY_MAX);
        std::uint64_t Start = 0, End = 0;
        const bool Said = g_ring.Say(Text.data() + Offset, static_cast<std::uint32_t>(Piece),
                                     Start, End);
        if (Said) g_dirty |= g_ring.CatchUp(*g_model, Start);
        if (!Drawn) {
            Draw();
            Drawn = true;
        }
        if (Said) g_ring.Passed(End);
        Offset += Piece;
    }
}

// The held frames onto the model, their text said into the ring first.
void DrawHeld()
{
    if (g_held.empty()) return;
    Utter(std::string_view(g_said, g_said_length), [] {
        // The text is in the ring: the writers are told so before anything
        // else, the narrower the window a death could fall into (box/display.h,
        // CONSOLE_ORDER_SAID). A hold is never more than one utterance.
        LanesSaid(g_held.back().Run.order);
        g_model->Typing(LanesTyping());
        for (const HeldRun &Held : g_held) g_model->Apply(Held.Run, Held.Lane);
    });
    g_dirty = true;
    g_held.clear();
    g_said_length = 0;
}

void HoldFrame(const ConsoleRun &Run, std::uint32_t Lane)
{
    // The room is made at start for what a pass holds between two draws; a
    // fuller hold is drawn first rather than grown.
    if (g_held.size() == g_held.capacity()) DrawHeld();
    if (Run.kind == CONSOLE_RUN_TEXT) {
        const std::uint32_t Length = std::min<std::uint32_t>(Run.len, CONSOLE_RUN_TEXT_MAX);
        if (g_said_length + Length > LOG_SAY_MAX) DrawHeld();
        std::memcpy(g_said + g_said_length, Run.text, Length);
        g_said_length += Length;
    }
    g_held.push_back(HeldRun{Run, Lane});
}

void Present()
{
    if (!g_dirty) return;
    g_dirty = false;
    Caret At;
    LayersCompose(*g_model, g_composed, At);
    g_easel->Present(g_composed, At);
}

// The waiting questions whose askers' frames up to their marks are drawn,
// answered — lowest mark first, stopping at the first that must wait on.
bool AnswerWaiting()
{
    bool Answered = false;
    while (!g_waiting.empty()) {
        const WaitingQuestion &Waiting = g_waiting.front();
        if (!LanesDrawnTo(Waiting.Asked.sender_pid, Waiting.From.generation, Waiting.Mark)) break;

        if (Waiting.Command == DISP_CMD_LAYER_OPEN) {
            LayersOpen(Waiting.Asked, Waiting.From, Waiting.Open, *g_model);
        } else {
            std::expected<LayerCommit, error_t> Taken =
                LayersCommit(Waiting.Asked.sender_pid, Waiting.From.generation, Waiting.Commit,
                             *g_model);
            if (Taken) {
                Utter(Taken->Text, [&] {
                    g_model->CommitLayer(Taken->X, Taken->Y, Taken->Width, Taken->Height,
                                         Taken->Cells, Taken->ScrollRows, Taken->CursorX,
                                         Taken->CursorY, Taken->CursorVisible);
                });
                g_dirty = true;
            }
            DisplayLayerCommitReply Reply{};
            Reply.command = DISP_CMD_LAYER_COMMIT;
            Reply.error = static_cast<std::int32_t>(Taken ? OK : Taken.error());
            (void)answer(&Waiting.Asked, &Reply, sizeof(Reply));
        }
        g_waiting.erase(g_waiting.begin());
        Answered = true;
    }
    return Answered;
}

bool DeathStep()
{
    bool Did = false;
    Touch Event;
    while (touch_try_pop_tag(g_died, &Event)) {
        Did = true;
        if (Event.payload_len < sizeof(TouchProcessDied)) continue;
        TouchProcessDied Died;
        std::memcpy(&Died, Event.payload, sizeof(Died));
        LanesDeath(Died.pid, Died.generation);
        FrontDeath(Died.pid, Died.generation);
        g_dirty |= LayersDropProcess(Died.pid, Died.generation);

        // A dead asker's question waits for nothing now. It is answered all
        // the same — with nothing, which the kernel delivers nowhere — so the
        // kernel lets go of its record of it.
        for (auto It = g_waiting.begin(); It != g_waiting.end();) {
            if (It->Asked.sender_pid != Died.pid || It->From.generation != Died.generation) {
                ++It;
                continue;
            }
            (void)answer(&It->Asked, nullptr, 0);
            It = g_waiting.erase(It);
        }
    }
    return Did;
}

bool ScreenStep()
{
    const bool Again = g_easel->Step();
    g_dirty |= Again;
    return Again;
}

// A lane's turn (lanes.h): what is held drawn first, so the line kept, or
// laid again, is the one on the glass. No room to keep a line: it is not laid
// again, and the reader goes on where the caret is.
void LaneTurned(std::uint32_t Lane, LaneTurn Turn) noexcept
{
    if (Turn == LaneTurn::Closed) {
        g_model->Forget(Lane);
        return;
    }
    DrawHeld();
    try {
        if (Turn == LaneTurn::Behind) g_model->Away(Lane);
        else g_model->Back(Lane);
    } catch (...) {
        g_model->Forget(Lane);
    }
    g_dirty = true;
}

bool LaneStep()
{
    bool Moved = LanesStep(HoldFrame, DrawHeld, Ceiling());
    Moved |= AnswerWaiting();
    return Moved;
}

bool RingStep()
{
    const bool Drew = g_ring.CatchUp(*g_model);
    g_dirty |= Drew;
    return Drew;
}

bool LayerStep()
{
    const bool Changed = LayersStep();
    g_dirty |= Changed;
    return Changed;
}

void AnswerSize(const Result &Asked)
{
    std::uint8_t Reply[9];
    const std::uint32_t Cols = g_model->Width();
    const std::uint32_t Rows = g_model->Height();
    Reply[0] = DISP_CMD_SIZE;
    std::memcpy(Reply + 1, &Cols, sizeof(Cols));
    std::memcpy(Reply + 5, &Rows, sizeof(Rows));
    (void)answer(&Asked, Reply, sizeof(Reply));
}

// DISP_CMD_PAGE: the screen's colours, said by a keeper — kept as long as
// this display lives, and drawn from the next present; a keeper says them
// again to a display that comes after it.
void AnswerPage(const Result &Asked, const AskFrom &From, const DisplayPageRequest &Request)
{
    error_t Error = OK;
    if (!FrontKeeper(From)) Error = ERR_ACCESS_DENIED;
    else if ((Request.text >> 24) != 0 || (Request.page >> 24) != 0) Error = ERR_INVALID_ARGUMENT;
    if (Error == OK) {
        g_easel->Page(Request.text, Request.page);
        g_dirty = true;
    }
    const DisplayFrontReply Reply{DISP_CMD_PAGE, 0, {0, 0}, static_cast<std::int32_t>(Error), cabin_info()->pid};
    (void)answer(&Asked, &Reply, sizeof(Reply));
}

// A question (KCTX_ASK): AskFrom, then what is asked. Every branch answers —
// nothing at all when the question is not one this display knows.
void HearQuestion(const Result &Entry)
{
    const auto *Data = reinterpret_cast<const std::uint8_t *>(
        static_cast<std::uintptr_t>(Entry.data_addr));
    if (Entry.data_length <= sizeof(AskFrom)) {
        (void)answer(&Entry, nullptr, 0);
        return;
    }
    AskFrom From;
    std::memcpy(&From, Data, sizeof(From));
    const std::uint8_t *Question = Data + sizeof(AskFrom);
    const std::uint32_t Length = Entry.data_length - static_cast<std::uint32_t>(sizeof(AskFrom));

    switch (Question[0]) {
    case DISP_CMD_LANE:
        LanesGrant(Entry, From);
        return;
    case DISP_CMD_FRONT:
    case DISP_CMD_BACK:
    case DISP_CMD_ASK_END: {
        DisplayFrontRequest Request{};
        if (Length < sizeof(Request)) {
            (void)answer(&Entry, nullptr, 0);
            return;
        }
        std::memcpy(&Request, Question, sizeof(Request));
        FrontAsked(Entry, From, Request);
        g_dirty = true;
        return;
    }
    case DISP_CMD_SIZE:
        AnswerSize(Entry);
        return;
    case DISP_CMD_PLACES:
        FrontPlacesAsked(Entry);
        return;
    case DISP_CMD_KEYS: {
        DisplayKeysRequest Head{};
        if (Length < sizeof(Head)) {
            (void)answer(&Entry, nullptr, 0);
            return;
        }
        std::memcpy(&Head, Question, sizeof(Head));
        if (Length != sizeof(Head) + Head.count * sizeof(KeyChord)) {
            (void)answer(&Entry, nullptr, 0);
            return;
        }
        KeyChord Chords[255];
        std::memcpy(Chords, Question + sizeof(Head), Head.count * sizeof(KeyChord));
        FrontKeysAsked(Entry, From, Chords, Head.count, Head.on_screen);
        return;
    }
    case DISP_CMD_PAGE: {
        DisplayPageRequest Request{};
        if (Length < sizeof(Request)) {
            (void)answer(&Entry, nullptr, 0);
            return;
        }
        std::memcpy(&Request, Question, sizeof(Request));
        AnswerPage(Entry, From, Request);
        return;
    }
    case DISP_CMD_LAYER_OPEN:
    case DISP_CMD_LAYER_COMMIT: {
        WaitingQuestion Waiting{};
        Waiting.Asked = Entry;
        Waiting.From = From;
        Waiting.Command = Question[0];
        if (Question[0] == DISP_CMD_LAYER_OPEN && Length >= sizeof(Waiting.Open)) {
            std::memcpy(&Waiting.Open, Question, sizeof(Waiting.Open));
            Waiting.Mark = Waiting.Open.order;
        } else if (Question[0] == DISP_CMD_LAYER_COMMIT && Length >= sizeof(Waiting.Commit)) {
            std::memcpy(&Waiting.Commit, Question, sizeof(Waiting.Commit));
            Waiting.Mark = Waiting.Commit.order;
        } else {
            (void)answer(&Entry, nullptr, 0);
            return;
        }
        try {
            const auto At = std::upper_bound(
                g_waiting.begin(), g_waiting.end(), Waiting.Mark,
                [](std::uint64_t Mark, const WaitingQuestion &Other) { return Mark < Other.Mark; });
            g_waiting.insert(At, Waiting);
        } catch (...) {
            // No room to keep the question waiting: refused now, in the shape
            // its asker reads.
            if (Waiting.Command == DISP_CMD_LAYER_OPEN) {
                DisplayLayerOpenReply Refusal{};
                Refusal.command = DISP_CMD_LAYER_OPEN;
                Refusal.error = ERR_NO_MEMORY;
                (void)answer(&Entry, &Refusal, sizeof(Refusal));
            } else {
                DisplayLayerCommitReply Refusal{};
                Refusal.command = DISP_CMD_LAYER_COMMIT;
                Refusal.error = ERR_NO_MEMORY;
                (void)answer(&Entry, &Refusal, sizeof(Refusal));
            }
        }
        return;
    }
    default: {
        static bool Warned = false;
        if (!Warned) {
            Warned = true;
            kdbg_print("[display] unknown question 0x%x from pid %u", Question[0],
                       Entry.sender_pid);
        }
        (void)answer(&Entry, nullptr, 0);
        return;
    }
    }
}

// A message: what needs no answer.
void HearMessage(const Result &Entry)
{
    const auto *Data = reinterpret_cast<const std::uint8_t *>(
        static_cast<std::uintptr_t>(Entry.data_addr));
    const std::uint32_t Length = Entry.data_length;
    const std::uint32_t From = Entry.sender_pid;

    switch (Data[0]) {
    case DISP_CMD_LAYER_FRAME:
        if (Length >= sizeof(DisplayLayerFrameNotice)) {
            DisplayLayerFrameNotice Notice{};
            std::memcpy(&Notice, Data, sizeof(Notice));
            g_dirty |= LayersFrame(From, Notice.generation, Notice.layer_id);
        }
        return;
    case DISP_CMD_LAYER_CLOSE:
        if (Length >= sizeof(DisplayLayerCloseRequest)) {
            DisplayLayerCloseRequest Request{};
            std::memcpy(&Request, Data, sizeof(Request));
            g_dirty |= LayersClose(From, Request.generation, Request.layer_id);
        }
        return;
    case DISP_CMD_LISTEN:
        /* [cmd][u32 generation][u8 listening] */
        if (Length >= 6) {
            std::uint32_t Generation;
            std::memcpy(&Generation, Data + 1, sizeof(Generation));
            LanesListen(From, Generation, Data[5] != 0);
        }
        return;
    case DISP_CMD_PING: {
        const std::uint32_t Me = cabin_info()->pid;
        (void)send(From, &Me, sizeof(Me));
        return;
    }
    default: {
        /* Nothing legitimate sends anything else. Said once rather than
         * swallowed forever. */
        static bool Warned = false;
        if (!Warned) {
            Warned = true;
            kdbg_print("[display] unknown wire byte 0x%x from pid %u", Data[0], From);
        }
        return;
    }
    }
}

bool IpcStep()
{
    bool Did = false;
    Result Entry;

    while (receive(&Entry)) {
        Did = true;
        // The ring's and the lanes' bells are Results with no data: they
        // woke the sleep, and that was all they were for.
        if (Entry.data_addr == 0 || Entry.data_length == 0 || Entry.sender_pid == 0) continue;
        // A question's bytes go with its answer; a message's are given back
        // once heard — every frame of every layer is one, for as long as the
        // machine runs.
        if (KCTX_KIND(Entry.context) == KCTX_ASK) {
            HearQuestion(Entry);
        } else {
            HearMessage(Entry);
            (void)give_back(&Entry);
        }
    }
    return Did;
}

bool Rotate()
{
    bool Moved = false;
    Moved |= LanesKeys();
    Moved |= DeathStep();
    Moved |= FrontSpawnStep();
    Moved |= ScreenStep();
    Moved |= LaneStep();
    Moved |= RingStep();
    Moved |= LayerStep();
    Moved |= IpcStep();
    return Moved;
}

}  // namespace

int main(void)
{
    // This program draws; what it says goes into the log ring.
    io_set_mode(IO_MODE_RING);

    std::expected<Easel, box::error> Claimed = Easel::Claim();
    if (!Claimed) {
        if (Claimed.error().raw() == ERR_ACCESS_DENIED)
            kdbg_print("[display] the screen is held by a living program; this "
                       "display draws nothing and goes");
        else
            kdbg_print("[display] the screen could not be claimed (%d); nothing is drawn "
                       "but the kernel's Emergency Screen",
                       static_cast<int>(Claimed.error().raw()));
        return 1;
    }
    g_easel.emplace(std::move(*Claimed));

    try {
        g_model = new ConsoleModel(g_easel->Cols(), g_easel->Rows());
        g_composed.resize(static_cast<std::size_t>(g_easel->Cols()) * g_easel->Rows());
        g_held.reserve(32);
    } catch (...) {
        kdbg_print("[display] no room for a %ux%u console", g_easel->Cols(), g_easel->Rows());
        return 1;
    }

    // The machine's account so far, from the oldest byte the ring holds: what
    // the Emergency Screen showed until now goes on being shown.
    (void)g_ring.CatchUp(*g_model);
    g_dirty = true;
    Present();

    if (!LanesStart(LaneTurned)) {
        kdbg_print("[display] the keyboard's tag could not be claimed; nobody can type");
        return 1;
    }
    if (!FrontStart()) {
        kdbg_print("[display] process:spawned could not be claimed; the program in front could not be kept");
        return 1;
    }
    g_died = touch_pair_choose(touch_intern(TOUCH_TAG_PROCESS_DIED));
    if (g_died == TOUCH_TAG_INVALID || touch_claim(g_died, TOUCH_REST, 0, 0) != 0) {
        kdbg_print("[display] process:died could not be claimed; the dead's lanes would never close");
        return 1;
    }

    CabinInfo *Info = cabin_info();
    if (Info->spawner_pid != 0) {
        const std::uint8_t Ready = 0xFF;
        (void)send(Info->spawner_pid, &Ready, 1);
    }
    const std::uint32_t Me = Info->pid;

    for (;;) {
        const bool Moved = Rotate();
        Present();
        if (Moved) continue;

        /* Nothing moved. Hang the bells FIRST, then take the mark, then look
         * one last time — in that order, and the order is the whole proof.
         * Anything arriving from here on either rings a bell (itself an
         * arrival on the Result ring), or moves a cursor past the mark, or
         * turns up in the look below. The ring's bell is a call into the
         * kernel, so it goes up before the mark, and it says at once when a
         * line came between the last look at the ring and its hanging. The
         * last look reads the ring only once the bell has said there is
         * something to read. */
        LanesBellsHang(Me);
        const bool News = g_ring.HangBell();
        const TurnInMark Mark = box_mark();
        if (News || Rotate()) {
            Present();
            LanesBellsTake();
            continue;
        }
        box_turn_in(Mark);
        LanesBellsTake();
    }
}
