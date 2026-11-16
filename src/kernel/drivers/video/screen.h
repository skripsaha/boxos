#ifndef SCREEN_H
#define SCREEN_H

#include "ktypes.h"
#include "error.h"
#include "boxos_screen.h"

/*
 * Screen — the glass as a resource the kernel lends to ONE system program.
 *
 * The kernel orchestrates and hands out resources; it does not draw. The
 * display program claims the screen (hw.screen.claim) and gets the glass
 * mapped into its cabin at CABIN_SCREEN_BASE — write-combining for a linear
 * framebuffer, uncacheable for the 80x25 text page, the same type the
 * kernel's own mapping of it has. From then on everything on the glass is the
 * display's, the kernel's own lines included: it reads them from the log ring.
 *
 * The kernel draws only through the Emergency Screen (emergency_screen.h) and
 * only while nobody holds the screen: before the first claim, after a
 * release, after the holder's cabin ends, while Ctrl+Alt+Esc has the glass,
 * and in a panic.
 *
 * Lock: s_screen_turn, held through a whole turn — the holder record, the
 * page-table rewrite and its shootdown, the Emergency Screen shown or hidden.
 * Order: s_screen_turn → ctx->lock / shootdown, and s_screen_turn →
 * console lock → ring lock. Touch events go out after it is let go.
 */

struct process_t;
struct cabin_t;

/* The legacy text page every BIOS boot starts on. */
#define SCREEN_TEXT_PHYS   0xB8000u
#define SCREEN_TEXT_COLS   80u
#define SCREEN_TEXT_ROWS   25u

/* The first act of the kernel's console: the text page, with the Emergency
 * Screen waiting on it — a quiet boot draws nothing until the display program
 * claims the glass, unless the boot cannot reach it or the person asks to see
 * (emergency_screen.h, EmergencyScreenAwait). Runs before mem_init. */
void ScreenInit(void);

/* The quiet boot is over, because nobody is coming for the glass — no volume
 * to start the display program from, or a start set without it — or because
 * the person asked to see everything (gangway.settings startup all). The
 * log's tail goes on the glass now, unless a holder has it. */
void ScreenQuietEnds(void);

/* The linear framebuffer a UEFI loader handed over (boot_info), after
 * vmm_init. Says on the console what it took, or why it kept the text page. */
void ScreenAdoptFramebuffer(uint64_t phys, uint32_t width, uint32_t height,
                            uint32_t stride, uint32_t format);

/* The loader left the first frame of the scene whose file is `fid` on the
 * framebuffer (boarding_pass.h SCENE): said to the first claim of the glass
 * (SCREEN_LEFT_SCENE), unless the Emergency Screen has drawn over it by then. */
void ScreenLeftByLoader(uint64_t fid);

/* hw.screen.claim / hw.screen.release. A claim by the cabin that already holds
 * the screen answers the same description again, *taken false; a claim that
 * took the screen now says *taken true. ERR_BUSY: another cabin holds it and
 * its program is ending — wait for the `screen` FREE event. ERR_ACCESS_DENIED:
 * a living program holds it. */
error_t ScreenClaim(struct process_t *proc, ScreenDescription *out, bool *taken);
error_t ScreenRelease(struct process_t *proc);

/* The cabin's end. MUST run first in cabin_destroy: the window's frames are a
 * device's, and vmm_destroy_context would hand every one of them to the PMM. */
void ScreenCleanupCabin(struct cabin_t *cabin);

/* Is this the cabin that holds the screen? */
bool ScreenHeldBy(struct cabin_t *cabin);

/* The ring's bell (klib_logring.h), hung for the strand (pid, generation) of
 * the holder `cabin` under the screen's own lock — a let-go that races it
 * either comes first and refuses it, or comes after and drops it, so a bell
 * never outlives the hold. ERR_ACCESS_DENIED when `cabin` holds nothing; with
 * OK, *news says an utterance past `from` is there already and none was hung. */
error_t ScreenBellHang(struct cabin_t *cabin, uint32_t pid, uint32_t generation,
                       uint64_t from, bool *news);

/* Ctrl+Alt+Esc (keyboard.c). Any context, an interrupt's included: counts the
 * press and knocks; a K-Core turns the glass over — or, with nobody holding
 * it, ends a quiet boot and shows the log. */
void ScreenSwapKnock(void);

#endif /* SCREEN_H */
