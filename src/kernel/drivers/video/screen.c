/*
 * screen.c — the glass, lent to one cabin at a time. See screen.h.
 *
 * The holder is a CABIN, not a strand: the window is mapped into an address
 * space every strand of it runs in, exactly like a Bay claim, and it ends the
 * way a Bay claim ends — an explicit release, or the cabin's teardown.
 *
 * Ctrl+Alt+Esc turns the glass over without taking anything from the holder.
 * Its window is pointed, entry by entry and in one hold of the page tables, at
 * a single page of ordinary memory (the stand-in): the display goes on drawing
 * into it, faults on nothing, and sees nothing of its own on the glass. The
 * second press points the window back and tells the holder (Touch `screen`
 * BACK) to draw its whole picture again. Unmapping instead would have killed
 * it — a touch of an absent page in this window is no first-touch region, and
 * the fault handler ends the cabin — and a hung display is exactly the one
 * this key exists for, while a healthy one must survive a press made by mistake.
 *
 * The stand-in is mapped WRITE-BACK, like every other mapping of RAM: the Pull
 * Map already reaches it write-back, and a write-combining alias of cacheable
 * memory is what Intel SDM Vol 3A §12.12.4 forbids. It is registered as a
 * shared physical page so no teardown can ever hand it to the PMM.
 */

#include "screen.h"
#include "emergency_screen.h"
#include "klib.h"
#include "klib_logring.h"   /* LogRingBellDrop — the bell goes with the screen */
#include "process.h"
#include "cabin.h"
#include "vmm.h"
#include "pmm.h"
#include "io.h"
#include "touch.h"
#include "baton.h"          /* Knock — Ctrl+Alt+Esc comes from an interrupt */
#include "cabin_layout.h"
#include "journey.h"

typedef struct {
    uint32_t kind;        /* SCREEN_KIND_* */
    uint64_t phys;        /* the glass's first byte */
    uint64_t bytes;       /* rows of it the mode shows: stride x height */
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t format;
} ScreenGlass;

typedef struct {
    struct cabin_t *cabin;  /* NULL: nobody holds the screen */
    bool            away;   /* Ctrl+Alt+Esc has the glass; the window leads to the stand-in */
} ScreenHolder;

static ScreenGlass  g_glass;
static ScreenHolder g_holder;
static spinlock_t   s_screen_turn;   /* BSS-zero is unlocked; ScreenInit says so formally */
static uintptr_t    g_stand_in;      /* phys of the stand-in page; 0 until the first claim; under s_screen_turn */

static uint32_t g_swap_presses;      /* Ctrl+Alt+Esc presses not acted on yet */
static void ScreenSwapKnocked(void *ctx);
static Knock g_swap_knock = {
    .baton  = { .next = NULL, .run = ScreenSwapKnocked, .ctx = NULL },
    .raised = 0,
};

/* =========================================================================
 *  The window
 * ========================================================================= */

typedef struct {
    uintptr_t phys;     /* page-aligned start */
    size_t    pages;
    uint64_t  offset;   /* of the glass's first byte inside the first page */
    uint64_t  flags;
} ScreenWindow;

/* Where the glass goes in a cabin, and as what. The memory type is the one
 * the kernel's own mapping of the same glass has — write-combining for the
 * framebuffer (vmm_map_framebuffer), uncacheable for the text page (the Pull
 * Map's legacy hole) — so no two mappings of it ever disagree. */
static ScreenWindow ScreenWindowOf(void)
{
    ScreenWindow w;
    w.phys   = (uintptr_t)(g_glass.phys & ~(uint64_t)(VMM_PAGE_SIZE - 1u));
    w.offset = g_glass.phys - w.phys;
    w.pages  = (size_t)((w.offset + g_glass.bytes + VMM_PAGE_SIZE - 1u) / VMM_PAGE_SIZE);
    w.flags  = VMM_FLAGS_USER_RW | VMM_FLAG_NO_EXECUTE |
               (g_glass.kind == SCREEN_KIND_PIXELS
                    ? vmm_wc_pte_flags()
                    : (VMM_FLAG_CACHE_DISABLE | VMM_FLAG_WRITE_THROUGH));
    return w;
}

static void ScreenSay(uint32_t event)
{
    TouchPublish(SCREEN_TOUCH_TAG, &event, sizeof(event));
}

static void EmergencyShow(void)
{
    console_lock_acquire();
    EmergencyScreenShow();
    console_lock_release();
}

static void EmergencyHide(void)
{
    console_lock_acquire();
    EmergencyScreenHide();
    console_lock_release();
}

/* The holder lets go, under s_screen_turn: the window cleared — its frames are
 * not the cabin's and nothing is freed — the ring's bell taken down, and the
 * glass to the Emergency Screen unless Ctrl+Alt+Esc has it already. */
static void ScreenLetGoLocked(void)
{
    ScreenWindow w = ScreenWindowOf();
    vmm_unmap_range(g_holder.cabin->vmm, CABIN_SCREEN_BASE, w.pages);
    LogRingBellDrop();

    bool was_away = g_holder.away;
    g_holder.cabin = NULL;
    g_holder.away  = false;
    if (!was_away) EmergencyShow();
}

/* =========================================================================
 *  The text page's hardware
 * ========================================================================= */

/* ‼ THE HIGH BIT OF A BACKGROUND IS A COLOUR HERE, NOT A FLASH.
 *
 * An attribute byte is (bg << 4) | fg, and in the mode the firmware leaves
 * behind, bit 7 — the top bit of the background nibble — makes the cell BLINK.
 * Eight of the sixteen backgrounds were therefore unusable: lavalamp's liquid
 * flashed at 2 Hz instead of glowing. Clearing bit 3 of Attribute Controller
 * index 0x10 hands them back as what they look like they are.
 *
 * ‼ THE FLIP-FLOP IS RESET TWICE. Port 0x3C0 is index and data through one
 * address, told apart by a flip-flop a read of 0x3DA puts into INDEX state.
 * Reading 0x3C1 leaves it in DATA state, so the index written again straight
 * after would land as data, the data as an index with bit 5 (PAS) clear — and
 * a palette disconnected is a BLACK screen, measured exactly that way on the
 * first BIOS boot after this was added. Hence the second read of 0x3DA, and the
 * index carrying 0x20, which keeps PAS on.
 *
 * No interrupt is disabled: this runs once, on the BSP, before any other core
 * exists. */
static void ScreenTextBlinkOff(void)
{
    (void)inb(0x3DA);                        /* flip-flop -> index */
    outb(0x3C0, 0x10 | 0x20);                /* index 0x10, palette access kept on */
    uint8_t mode = inb(0x3C1);               /* flip-flop is left in DATA state */

    (void)inb(0x3DA);                        /* -> index again, for the write pair */
    outb(0x3C0, 0x10 | 0x20);
    outb(0x3C0, (uint8_t)(mode & ~0x08u));   /* blink enable cleared */
}

/* The hardware cursor off, for good: the Emergency Screen draws no caret, and
 * the display draws its own (CRTC Cursor Start, index 0x0A, bit 5 = disable). */
static void ScreenTextCursorOff(void)
{
    outb(0x3D4, 0x0A);
    uint8_t start = inb(0x3D5);
    outb(0x3D5, (uint8_t)(start | 0x20u));
}

/* =========================================================================
 *  Boot
 * ========================================================================= */

void ScreenInit(void)
{
    spinlock_init(&s_screen_turn);

    g_glass.kind   = SCREEN_KIND_TEXT;
    g_glass.phys   = SCREEN_TEXT_PHYS;
    g_glass.width  = SCREEN_TEXT_COLS;
    g_glass.height = SCREEN_TEXT_ROWS;
    g_glass.stride = SCREEN_TEXT_COLS * 2u;
    g_glass.format = 0;
    g_glass.bytes  = (uint64_t)g_glass.stride * g_glass.height;

    ScreenTextBlinkOff();
    ScreenTextCursorOff();

    console_lock_acquire();
    EmergencyScreenUseText(SCREEN_TEXT_COLS, SCREEN_TEXT_ROWS);
    EmergencyScreenAwait();
    console_lock_release();
}

void ScreenQuietEnds(void)
{
    spin_lock(&s_screen_turn);
    if (!g_holder.cabin) EmergencyShow();
    spin_unlock(&s_screen_turn);
}

void ScreenAdoptFramebuffer(uint64_t phys, uint32_t width, uint32_t height,
                            uint32_t stride, uint32_t format)
{
    uint32_t least_w, least_h;
    EmergencyScreenLeastCell(&least_w, &least_h);
    if (!phys || width < least_w || height < least_h || stride < width * 4u) {
        kprintf("[SCREEN] the loader's framebuffer is not one this screen can draw on "
                "(phys 0x%lx, %ux%u, stride %u); the text page stays\n",
                (unsigned long)phys, width, height, stride);
        return;
    }

    uint64_t bytes = (uint64_t)height * stride;
    volatile void *pixels = vmm_map_framebuffer((uintptr_t)phys, (size_t)bytes);
    if (!pixels) {
        kprintf("[SCREEN] no kernel window for the %ux%u framebuffer at 0x%lx; "
                "the text page stays\n", width, height, (unsigned long)phys);
        return;
    }

    spin_lock(&s_screen_turn);
    g_glass.kind   = SCREEN_KIND_PIXELS;
    g_glass.phys   = phys;
    g_glass.bytes  = bytes;
    g_glass.width  = width;
    g_glass.height = height;
    g_glass.stride = stride;
    g_glass.format = format;
    console_lock_acquire();
    EmergencyScreenUsePixels((volatile uint8_t *)pixels, width, height, stride, format);
    console_lock_release();
    spin_unlock(&s_screen_turn);

    uint32_t cols, rows, cell_w, cell_h;
    EmergencyScreenCells(&cols, &rows, &cell_w, &cell_h);
    kprintf("[SCREEN] framebuffer %ux%u, stride %u, format %u — %u columns by %u rows "
            "in cells of %ux%u from the imprint\n", width, height, stride, format, cols, rows, cell_w, cell_h);
}

/* =========================================================================
 *  Claim and release
 * ========================================================================= */

/* The stand-in page, made at the first claim and never given back. Under
 * s_screen_turn, so it is made once. */
static bool ScreenStandInReadyLocked(void)
{
    if (g_stand_in) return true;

    void *page = pmm_alloc_zero(1);
    if (!page) return false;
    if (!vmm_register_shared_phys((uint64_t)(uintptr_t)page)) {
        kprintf("[SCREEN] the stand-in page could not be registered as shared; "
                "the screen is not lent until it can be\n");
        pmm_free(page, 1);
        return false;
    }
    g_stand_in = (uintptr_t)page;
    return true;
}

static void ScreenDescribe(ScreenDescription *out)
{
    ScreenWindow w = ScreenWindowOf();
    out->kind     = g_glass.kind;
    out->width    = g_glass.width;
    out->height   = g_glass.height;
    out->stride   = g_glass.stride;
    out->format   = g_glass.format;
    out->reserved = 0;
    out->va       = CABIN_SCREEN_BASE + w.offset;
}

error_t ScreenClaim(struct process_t *proc, ScreenDescription *out, bool *taken)
{
    if (!proc || !proc->cabin || !out || !taken) return ERR_INVALID_ARGUMENT;

    struct cabin_t *cabin = proc->cabin;
    *taken = false;
    spin_lock(&s_screen_turn);

    if (g_holder.cabin == cabin) {
        ScreenDescribe(out);
        spin_unlock(&s_screen_turn);
        return OK;
    }
    if (g_holder.cabin) {
        /* A holder whose program is ending lets go as its cabin is taken
         * down, and FREE says when: worth waiting for. A living one may hold
         * the screen for good, and a claimant that waited on it would wait
         * for ever. `ending` is set once, by CabinEnd, before the death any
         * waiter could have heard of. */
        bool ending = __atomic_load_n(&g_holder.cabin->ending, __ATOMIC_ACQUIRE) != 0;
        spin_unlock(&s_screen_turn);
        return ending ? ERR_BUSY : ERR_ACCESS_DENIED;
    }
    if (!ScreenStandInReadyLocked()) {
        spin_unlock(&s_screen_turn);
        return ERR_NO_MEMORY;
    }

    ScreenWindow w = ScreenWindowOf();
    if ((uint64_t)w.pages * VMM_PAGE_SIZE > CABIN_SCREEN_END - CABIN_SCREEN_BASE) {
        spin_unlock(&s_screen_turn);
        return ERR_OUT_OF_RANGE;
    }
    vmm_map_result_t mapped = vmm_map_pages(cabin->vmm, CABIN_SCREEN_BASE, w.phys,
                                            w.pages, w.flags);
    if (!mapped.success) {
        spin_unlock(&s_screen_turn);
        return ERR_NO_MEMORY;
    }

    g_holder.cabin = cabin;
    g_holder.away  = false;
    EmergencyHide();
    ScreenDescribe(out);
    *taken = true;
    JourneyMark("display");
    spin_unlock(&s_screen_turn);
    return OK;
}

error_t ScreenRelease(struct process_t *proc)
{
    if (!proc || !proc->cabin) return ERR_INVALID_ARGUMENT;

    spin_lock(&s_screen_turn);
    if (g_holder.cabin != proc->cabin) {
        spin_unlock(&s_screen_turn);
        return ERR_INVALID_STATE;
    }
    ScreenLetGoLocked();
    spin_unlock(&s_screen_turn);

    ScreenSay(SCREEN_EVENT_FREE);
    return OK;
}

void ScreenCleanupCabin(struct cabin_t *cabin)
{
    if (!cabin) return;

    spin_lock(&s_screen_turn);
    bool held = (g_holder.cabin == cabin);
    if (held) ScreenLetGoLocked();
    spin_unlock(&s_screen_turn);

    if (held) ScreenSay(SCREEN_EVENT_FREE);
}

bool ScreenHeldBy(struct cabin_t *cabin)
{
    spin_lock(&s_screen_turn);
    bool held = cabin && g_holder.cabin == cabin;
    spin_unlock(&s_screen_turn);
    return held;
}

error_t ScreenBellHang(struct cabin_t *cabin, uint32_t pid, uint32_t generation,
                       uint64_t from, bool *news)
{
    spin_lock(&s_screen_turn);
    if (!cabin || g_holder.cabin != cabin) {
        spin_unlock(&s_screen_turn);
        return ERR_ACCESS_DENIED;
    }
    *news = LogRingBellHang(pid, generation, from);
    spin_unlock(&s_screen_turn);
    return OK;
}

/* =========================================================================
 *  Ctrl+Alt+Esc
 * ========================================================================= */

void ScreenSwapKnock(void)
{
    __atomic_fetch_add(&g_swap_presses, 1u, __ATOMIC_RELEASE);
    KnockOn(&g_swap_knock);
}

/* On a K-Core. Knocks merge, so the presses are counted: an even number since
 * the last turn leaves the glass where it is. */
static void ScreenSwapKnocked(void *ctx)
{
    (void)ctx;
    KnockOpen(&g_swap_knock);

    uint32_t presses = __atomic_exchange_n(&g_swap_presses, 0u, __ATOMIC_ACQ_REL);
    if (!(presses & 1u)) return;

    uint32_t event = 0;
    spin_lock(&s_screen_turn);
    if (!g_holder.cabin) {
        /* Nobody holds the glass: a quiet boot a person wants to look behind,
         * or the log already on it — either way, the log. */
        EmergencyShow();
    } else {
        ScreenWindow w = ScreenWindowOf();
        bool turned;
        if (!g_holder.away) {
            /* The stand-in starts every turn blank: what one holder drew into
             * it while it was away is nobody else's to see. */
            memset(vmm_phys_to_virt(g_stand_in), 0, VMM_PAGE_SIZE);
            turned = vmm_retarget_pages(g_holder.cabin->vmm, CABIN_SCREEN_BASE, w.pages,
                                        g_stand_in, 0,
                                        VMM_FLAGS_USER_RW | VMM_FLAG_NO_EXECUTE);
            if (turned) {
                g_holder.away = true;
                EmergencyShow();
                event = SCREEN_EVENT_AWAY;
            }
        } else {
            EmergencyHide();
            turned = vmm_retarget_pages(g_holder.cabin->vmm, CABIN_SCREEN_BASE, w.pages,
                                        w.phys, VMM_PAGE_SIZE, w.flags);
            if (turned) {
                g_holder.away = false;
                event = SCREEN_EVENT_BACK;
            }
        }
        if (!turned) {
            /* An entry of the window was gone — the holder's own doing, since
             * nothing else unmaps it. Its window is broken; it holds nothing. */
            kprintf("[SCREEN] the holder's screen window has a hole in it; "
                    "the screen is taken back\n");
            g_holder.away = false;
            ScreenLetGoLocked();
            event = SCREEN_EVENT_FREE;
        }
    }
    spin_unlock(&s_screen_turn);

    if (event) ScreenSay(event);
}
