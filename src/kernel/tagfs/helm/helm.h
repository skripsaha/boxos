#ifndef TAGFS_HELM_H
#define TAGFS_HELM_H

#include "../tagfs.h"
#include "../plot/plot.h"

/*
 * Helm — one holder of a file's extents at a time, and a queue instead of a
 * spin (the design, M1).
 *
 * A write — the synchronous one and the asynchronous job alike — a span and a
 * defrag each hold their file's Helm for the whole of their work, the medium
 * included, and load the file's record only once it is theirs (PlotLoad), so
 * none of them works on a copy another holder has since changed. The Helm is
 * not a lock: nothing is held with interrupts off across the medium, and
 * nobody spins for it. One that finds it held waits in its FIFO, and the
 * holder that lets go hands it to the first waiter (HelmHand): a program's
 * job is parked on its chit meanwhile (write_job.c), and a caller in the
 * kernel that cannot be parked is refused rather than left spinning.
 *
 * Who holds no Helm: a read, which walks the record its open copied; a name
 * or a tag, whose Ink never touches the extents — but for a tag of key system,
 * boot or god put on a book's file, which goes on under the Helm with the
 * file's tail laid in its block first (engross.h); and a delete, whose Quitclaim
 * marks the file `vacated` — the holder answers ERR_FILE_NOT_FOUND at its own
 * Ink, and its Stakes go back.
 *
 * And what a handle sees stays where it is. A handle is opened with a view_gen,
 * the newest Ink as it looked; ground an Ink after that frees from its file
 * rests in Fallow until the handle is closed (fallow.h, condition 3), so a
 * delete under a reader never hands the reader's blocks to another file.
 *
 * ‼ THE FIFO WAITS WITH THE OPEN BOOK (RT-13). From the moment the volume sees
 * its medium gone nobody new is queued behind a holder — one who asks is
 * answered at once (HelmWaiter.gone) — and a holder who lets go hands the
 * Helm to nobody: its Helm is parked, its FIFO held as the volume's open book
 * is (mount/away.h). When the same volume comes back as it was left, every
 * parked Helm is handed to its first waiter and the writes go on; when the
 * book is given up, every waiter is answered with the count of changes no
 * cycle settled — what left with the medium. A waiter holds the volume's
 * door, so the volume is let go of only once every one of them has its
 * answer.
 *
 * The open-file table the Helm lives in belongs to the machine, not to the
 * volume: a mount leaves it alone. Its lock and every FileHelm's are leaves.
 */

/* The file's entry, made when it is not open yet: NULL when there is no
 * memory for one — an open without one is refused. */
OpenFileEntry* ofe_acquire(uint64_t file_id);
void           ofe_release(OpenFileEntry* ofe);

#if CONFIG_TAGFS_HOLD_GROUND
/* Whether `ofe` is the entry the table hands out for its file, and its
 * references then. */
bool           ofe_listed(const OpenFileEntry* ofe, uint32_t* out_refs);
#endif

/* The Helm for `w`: true when it was free and is w's now; false when w waits
 * in the FIFO, and w->hand runs once it is w's — on the core that let go, with
 * no TagFS lock held — or when the medium left, free Helm or not: w->gone has
 * answered it before this returns. Either way a false w is not the caller's
 * to touch again. */
bool     HelmAsk(OpenFileEntry* ofe, HelmWaiter* w);

/* The Helm for `w` only when it is free: false, and w not queued, when it is
 * held or parked, or the medium left. */
bool     HelmTry(OpenFileEntry* ofe, HelmWaiter* w);

/* HelmTry for the Engross service (engross.h): a Helm it finds held is noted,
 * and its holder's let-go asks the service again (EngrossAsked) — the one look
 * a turn took at a held Helm is never the service's last. */
bool     HelmTryNoted(OpenFileEntry* ofe, HelmWaiter* w);

/* The Helm for a caller in the kernel, only when it is free: such a caller
 * cannot be parked behind the holder the way a program is (write_job.c), and
 * it does not spin for it. `here` is made its place. OK when it is held;
 * ERR_BUSY — said — when another holds it; ERR_DEVICE_NOT_READY when the
 * medium left. */
error_t  HelmTakeNow(TagFSFileHandle* handle, HelmWaiter* here);

/* `w` lets go; the first waiter, if any, is handed the Helm (HelmHand) — or,
 * once the medium left, none is: the Helm is parked with its FIFO. */
void     HelmLetGo(OpenFileEntry* ofe, HelmWaiter* w);

/* The fate of the open book, done here for every Helm's FIFO, where the fate
 * is decided — on the core attending the medium, with no TagFS lock held.
 * Given up — the latch still standing, so nobody is queued meanwhile — every
 * waiter answered (w->gone) with `lost`. Taken up — the latch let go of —
 * every parked Helm handed to its first waiter. */
void     HelmBookGivenUp(uint64_t lost);
void     HelmBookTakenUp(void);

/* The extents the holder of the file's Helm claimed room for in its record
 * (RT-19, InkRoomClaim): 0 when the file is not open or nothing is claimed.
 * Under the volume's state lock, which the claim is made under. */
uint32_t HelmClaimedRuns(uint64_t file_id);

/* Whether a Quitclaim gave the file up since it was opened. */
bool     HelmVacated(const OpenFileEntry* ofe);

/* The file was given up: its Helm, when it is open, is vacated. */
void     HelmVacate(uint64_t file_id);

#ifdef CONFIG_VOLUME_TESTS
/* The stand's hold of a holder (write_job.c, a defrag asked to hold its
 * file), kept by the holder and listed in helm.c until the file is given up. */
typedef struct HelmStandHold {
    struct HelmStandHold* next;
    const OpenFileEntry*  ofe;
    HelmWaiter*           holder;
} HelmStandHold;

/* `w`, the holder of the file's Helm, listed for a service point (HelmReady)
 * by the give-up itself (HelmVacate) — at once when the file is given up
 * already. `hold` is the holder's until it is listed. No TagFS lock held. */
void     HelmReadyOnceVacated(HelmStandHold* hold, OpenFileEntry* ofe, HelmWaiter* w);
#endif

/* A waiter handed the Helm that does its work synchronously: listed, and run
 * by the next service point (TagFSServiceIfPending) — never in the hand, which
 * may run in an interrupt's continuation that must not stand over the medium.
 * The drain core is knocked awake for it. */
void     HelmReady(HelmWaiter* w);

/* The service point: every waiter HelmReady had listed when it began run here,
 * the scheduler held off this core for each (SchedulerHoldHere). One listed
 * meanwhile — a write come back for its next window — waits for the next
 * service point, so the core goes round its loop between two windows. One
 * load when none is. */
void     HelmRunReady(void);

/* Whether a service point has work: a waiter listed. An idle core does it
 * before it sleeps. */
bool     HelmReadyListed(void);

/* Holders and waiters of every Helm: work the machine still owes a caller. */
uint64_t HelmOwed(void);

/* A handle's view, taken before its open copies the record — and again, moved
 * on, by a holder that loads the record afresh with no copy of its own — and
 * let go of at its close; and the oldest view of `file_id` among the open
 * handles of this mounting — UINT64_MAX when none is open. */
void     HelmViewTake(TagFSFileHandle* handle);
void     HelmViewLetGo(TagFSFileHandle* handle);
uint64_t HelmViewFloor(uint64_t file_id);

/*
 * The work a Helm's holder does, synchronously (helm_work.c). Each loads the
 * file's record under the Helm and answers ERR_FILE_NOT_FOUND at once for a
 * file given up. ERR_WOULD_BLOCK from it is no answer: nothing of the call was
 * done, the volume is short of ground a Settle cycle gives back, and the
 * holder waits for the cycle its own way before it makes the call again — a
 * program's job parked until the cycle's end (write_job.c), a caller in the
 * kernel in its own call (HelmRoomCameBack). No cycle is run beneath the work.
 *
 * A write is inked a window at a time (RT-16): the run the medium takes in one
 * command, in whole blocks — HelmWindowBytes, the bytes of the window that
 * begins at file offset `at`, which ends at a block's end — each its own load,
 * reservation, blocks and Ink, so a write of any length never holds more than
 * a window's ground unnamed, and the holder may give its core back between
 * two windows while it keeps the Helm. HelmWriteWindow: the next window of the
 * source — from its byte *inked, at the handle's offset (at the file's size
 * for a TAGFS_HANDLE_APPEND handle) — written and inked; *inked and the offset
 * move by what the file now holds of it. OK when the window is the file's
 * whole, else the refusal, with the prefix that landed before it inked
 * (W-ERR). HelmWrite: every window, one after another, as tagfs_write
 * describes — for a caller that has no core to give back.
 *
 * A span and a defrag go a window at a time too (R24), so neither holds its
 * holder's core for the length of a file. HelmSpanWindow: the span's next
 * window — the file grown by a window towards `size`, each its own load,
 * reservation, zeros and Ink, or cut to `size` in one — and *out_ended once it
 * is `size` long; with `grow_only` a file no shorter than `size` is left as it
 * is, decided here under the Helm. *out_found is the size the window found
 * when it loaded the file, left alone when it did not. The growth is asked of
 * the volume before its first block is taken (PlotRoomFor): a span past every
 * block the volume could give is refused ERR_TAGFS_NO_SPACE, said, with what
 * it already grew kept. HelmDefragWindow: the defrag's next window — the run
 * taken at the first, then the file's blocks copied into it as many at a time
 * as the seat takes, and one Ink once the last is copied and durable
 * (*out_ended) — on the HelmDefragging its holder keeps between windows and
 * lets go of on every way out (HelmDefragLetGo), which gives back a run
 * nothing named. HelmSpan and HelmDefrag: every window, one after another, as
 * tagfs_span_file and tagfs_defrag_file describe, for a caller that has no
 * core to give back; HelmSpan's *out_was is the size it found. Each is guarded
 * with the warrant the handle carries (InkWarrant).
 */
typedef struct {
    Plot          plot;      /* the file's, loaded at the first window */
    PlotGathering gather;    /* how far the copy into the run has come */
    bool          loaded;
} HelmDefragging;

uint64_t HelmWindowBytes(uint64_t at);
error_t  HelmWriteWindow(TagFSFileHandle* handle, const PlotSource* src, uint64_t* inked);
error_t  HelmWrite(TagFSFileHandle* handle, const PlotSource* src, uint64_t* out_written);
error_t  HelmSpanWindow(TagFSFileHandle* handle, uint64_t size, bool grow_only,
                        uint64_t* out_found, bool* out_ended);
error_t  HelmSpan(TagFSFileHandle* handle, uint64_t size, uint64_t* out_was);
error_t  HelmDefragWindow(TagFSFileHandle* handle, HelmDefragging* d, bool* out_ended);
void     HelmDefragLetGo(HelmDefragging* d);
error_t  HelmDefrag(TagFSFileHandle* handle);

/* Engross by the holder of the file's Helm (helm_work.c, spec_v2 §16.5 п.14):
 * its tail, out of the Slips over its last block, laid in that block and
 * inked — through the Ink, as a defrag is. *out_laid whether there was one;
 * OK, or the refusal of the load, the Plot or the Ink — ERR_WOULD_BLOCK among
 * them while the ribbon's room is on its way. */
error_t  HelmEngross(TagFSFileHandle* handle, bool* out_laid);

/* A caller in the kernel standing in its call, whose call answered *rc: true
 * when it was ERR_WOULD_BLOCK and Settle cycles brought ground back
 * (SettleForRoom), so the call is made again; otherwise false, and *rc is the
 * answer. HelmWrite waits this way between its windows. */
bool     HelmRoomCameBack(error_t* rc);

#endif /* TAGFS_HELM_H */
