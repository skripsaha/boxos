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
 * or a tag, whose Ink never touches the extents; and a delete, whose Quitclaim
 * marks the file `vacated` — the holder answers ERR_FILE_NOT_FOUND at its own
 * Ink, and its Stakes go back.
 *
 * And what a handle sees stays where it is. A handle is opened with a view_gen,
 * the newest Ink as it looked; ground an Ink after that frees from its file
 * rests in Fallow until the handle is closed (fallow.h, condition 3), so a
 * delete under a reader never hands the reader's blocks to another file.
 *
 * The open-file table the Helm lives in belongs to the machine, not to the
 * volume: a mount leaves it alone. Its lock and every FileHelm's are leaves.
 */

/* The file's entry, made when it is not open yet: NULL when there is no
 * memory for one — an open without one is refused. */
OpenFileEntry* ofe_acquire(uint32_t file_id);
void           ofe_release(OpenFileEntry* ofe);

#if CONFIG_TAGFS_HOLD_GROUND
/* Whether `ofe` is the entry the table hands out for its file, and its
 * references then. */
bool           ofe_listed(const OpenFileEntry* ofe, uint32_t* out_refs);
#endif

/* The Helm for `w`: true when it was free and is w's now; false when w waits
 * in the FIFO, and w->hand runs once it is w's — on the core that let go, with
 * no TagFS lock held. */
bool     HelmAsk(OpenFileEntry* ofe, HelmWaiter* w);

/* The Helm for `w` only when it is free: false, and w not queued, when it is
 * held. */
bool     HelmTry(OpenFileEntry* ofe, HelmWaiter* w);

/* `w` lets go; the first waiter, if any, is handed the Helm (HelmHand). */
void     HelmLetGo(OpenFileEntry* ofe, HelmWaiter* w);

/* Whether a Quitclaim gave the file up since it was opened. */
bool     HelmVacated(const OpenFileEntry* ofe);

/* The file was given up: its Helm, when it is open, is vacated. */
void     HelmVacate(uint32_t file_id);

/* A waiter handed the Helm that does its work synchronously: listed, and run
 * by the next service point (TagFSServiceIfPending) — never in the hand, which
 * may run in an interrupt's continuation that must not stand over the medium.
 * The drain core is knocked awake for it. */
void     HelmReady(HelmWaiter* w);

/* The service point: every waiter HelmReady listed, run here, the scheduler
 * held off this core for each (SchedulerHoldHere). One load when none is. */
void     HelmRunReady(void);

/* Holders and waiters of every Helm: work the machine still owes a caller. */
uint64_t HelmOwed(void);

/* A handle's view, taken before its open copies the record — and again, moved
 * on, by a holder that loads the record afresh with no copy of its own — and
 * let go of at its close; and the oldest view of `file_id` among the open
 * handles of this mounting — UINT64_MAX when none is open. */
void     HelmViewTake(TagFSFileHandle* handle);
void     HelmViewLetGo(TagFSFileHandle* handle);
uint64_t HelmViewFloor(uint32_t file_id);

/*
 * The work a Helm's holder does, synchronously (helm_work.c). Each loads the
 * file's record under the Helm, answers ERR_FILE_NOT_FOUND at once for a file
 * given up, and when the volume is short of ground Fallow still rests has
 * Settle cycles run and tries again — the holder holds no lock, so it may wait
 * for one.
 *
 * HelmWrite: the source's bytes at the handle's offset — at the file's size
 * for a TAGFS_HANDLE_APPEND handle — as tagfs_write describes. HelmSpan: the
 * file made exactly `size` bytes long, as tagfs_span_file, and in *out_was the
 * size it found. HelmDefrag: the file gathered into one run near
 * `target_block`, as tagfs_defrag_file.
 */
error_t  HelmWrite(TagFSFileHandle* handle, const PlotSource* src, uint64_t* out_written);
error_t  HelmSpan(TagFSFileHandle* handle, uint64_t size, uint64_t* out_was);
error_t  HelmDefrag(TagFSFileHandle* handle, uint32_t target_block);

#endif /* TAGFS_HELM_H */
