#ifndef TAGFS_ENGROSS_H
#define TAGFS_ENGROSS_H

#include "../tagfs.h"
#include "../ink/ink.h"

/*
 * Engross — a file's tail, out of the Slips over its last block, laid in that
 * block (spec_v2 §16.5 п.14): by the holder of the file's Helm, through the
 * Ink, as a defrag is (HelmEngross) — never by the cycle, which would be a
 * second door a file changes by, racing the writer that holds the Helm.
 *
 * A Tally takes no slice while the settled state carries a Slip (LooseLeaves,
 * binder.c): the Binder asks this service, which walks the files a turn at a
 * time on a service point of the Helm and lays the tail of each one whose
 * Helm is free; the cycle that carries those Inks settles them, and the Tally
 * goes after it. A file whose Helm a writer holds is left to the writer: no
 * Slip is given while a Tally is owed (plot_slip.c), so its next write lays
 * the block, its tail folded in. And a file of a book comes to wear system,
 * boot or god with its tail in its block (EngrossGuarded): the loader reads
 * such a file by its blocks (§9.1).
 */

/* The service asked: listed on the Helm's service points once, the walk of
 * the files it begins answering every ask made before it — by the Settle's
 * holder at a cycle's DONE (BinderWeigh), and by the let-go of a Helm a turn
 * found held (HelmTryNoted). With no TagFS lock held.
 *
 * A file whose tail would not be laid — its last block unreadable, the file
 * the loader reads the kernel by, a file the mount locked, no ground — is the
 * file's own: the walk goes on past it, says what it left once while that
 * stays the same, and tries a file refused for itself again only once its
 * record changes. The Tally waits for each such tail (BinderMayNot). */
void    EngrossAsked(void);

/* The tails the last walk left refused for their files' own sake, as it said
 * them, that Escheat can cut — every one but the tail of the file the loader
 * reads the kernel by, which no call changes, *boot: from any core, a word
 * that may be a walk old (escheat.h). */
uint32_t EngrossRefusedNow(uint32_t* boot);

/* Every file's tail laid in its block now, in the caller's own call — the
 * Tally a caller asks for and waits for (BinderRunWhole). OK; or, said, the
 * first file's refusal, or ERR_BUSY when every file left is another's Helm or
 * waits for the ribbon's room; ERR_DEVICE_NOT_READY when the medium left or
 * the volume takes no write. Inside the volume's door. */
error_t EngrossWhole(void);

/* The change of InkFields that puts a tag of key system, boot or god on file
 * `fid` of a book — warranted — made under the file's Helm, taken now, with
 * its tail laid in its block first: two Inks the Helm holds together, so no
 * write lays a Slip between them and the file never wears the tag with one.
 * OK, ERR_BUSY (said) while a writer holds the Helm, or the refusal of either
 * Ink — ERR_WOULD_BLOCK among them while the ribbon's room is on its way, which
 * the caller waits out as it does a tag's own Ink's. Inside the volume's door. */
error_t EngrossGuarded(uint64_t fid, const char* name, uint32_t add, uint32_t drop, InkReceipt* out);

#ifdef CONFIG_TAGFS_SETTLE_CUT_POINT
/* The Ink of an Engross laid at `gen` — by that Ink, under the state lock, so
 * a cycle whose snapshot takes `gen` sees it — and whether a stitch whose
 * snapshot reaches `target` names a block an Engross laid: the SETTLECUT point
 * before such a stitch (settle_cut.h, `engross`, C22). Lock-free. */
void    EngrossInked(uint64_t gen);
bool    EngrossCarriedBy(uint64_t target);
#endif

#endif /* TAGFS_ENGROSS_H */
