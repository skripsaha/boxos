/*
 * canvas.c — text-console engine.  See canvas.h for the architecture.
 *
 * Concurrency:
 *   s_canvas_lock is the single spinlock that protects every Canvas state
 *   mutation AND the commit pipeline.  Every public Canvas API call takes
 *   the lock at entry and releases it on return; internal _locked helpers
 *   assume the lock is already held and never re-acquire it.  This lets
 *   one method's body call another's body without recursive locking and
 *   without exposing partially-mutated state to other cores.
 *
 *   The commit runs UNDER the lock, so concurrent writers wait while a
 *   coalesced render pipes through the backend.  At 1024×768 GOP under
 *   QEMU TCG a full-frame blit is ≤ 1 ms; other cores wanting to log
 *   block briefly during that window.  This is intentional: display state
 *   is the atomic unit, intermixed half-blits would tear cells across
 *   producers.
 *
 *   Lock ordering: kprintf/hardware_ops first take g_kprintf_lock
 *   (a.k.a. console_lock), then Canvas methods take s_canvas_lock.  No
 *   code path ever takes s_canvas_lock first and then console_lock —
 *   backend methods (DrawCells/Scroll/Present/FillRow/DrawCaret) are
 *   pure pixel/MMIO and never re-enter the kernel logger.
 *
 * Manifest-level coalescing:
 *   manifest_exec.c wraps a MULTI-op Manifest in one CanvasBatchBegin/End,
 *   so a render() from the display daemon — one Manifest with N
 *   vga_setcolor / vga_puts / vga_newline ops — accumulates all of them
 *   into a single Canvas batch and commits once.  A five-newline render
 *   used to cost five full-frame blits.
 *
 *   A SINGLE-op Manifest gets no wrap, and that is only safe while the
 *   invariant below holds: every public writer here opens an implicit
 *   batch of its own, so it commits at its own boundary.  CanvasSetCursor
 *   was the one that did not, and the caret paid for it — see its comment.
 *
 * Hot-path invariants:
 *   • cells[][] is the single source of truth for visible content.
 *   • EVERY public writer — CanvasPrintChar, CanvasScrollUp, CanvasPaint,
 *     the Clear* family and CanvasSetCursor — modifies state, appends
 *     damage to the plan, and opens an implicit batch so a standalone
 *     call reaches the glass.  None of them touches the backend surface
 *     directly.
 *   • Surface updates happen exclusively inside canvas_commit_locked(),
 *     called from the outermost CanvasBatchEnd.
 *
 * Why lazy: every '\n' past the last row used to do its own 3 MB shadow
 * memmove + full-screen NT blit (UEFI GOP at 1024×768).  Five lines of
 * shell output → 18 MB of memory traffic per Manifest.  With coalescing
 * the same five lines collapse into one memmove + one blit (~6 MB), and
 * a full-screen wipe (48 lines) drops from ~141 MB to ~2 MB.
 */

#include "canvas.h"
#include "klib.h"
#include "vmm.h"
#include "touch.h"

/* If a single batch ever accumulates this many scrolls we force an
 * intermediate commit.  In practice the limit is several orders of
 * magnitude above what any sane batch produces; it exists to keep
 * uint32_t bookkeeping defensible. */
#define CANVAS_MAX_DEFERRED_SCROLL  4096u

/* Static fallback buffers sized for legacy VGA text mode.  Used when
 * CanvasInit runs BEFORE mem_init (the kernel boots in text mode and
 * starts kprintf-ing immediately).  When the GOP backend takes over
 * later, CanvasReplaceBackend swaps these for vmalloc-backed buffers
 * sized for the actual framebuffer. */
#define CANVAS_STATIC_COLS  80u
#define CANVAS_STATIC_ROWS  25u
static TextCell  s_static_cells [CANVAS_STATIC_ROWS * CANVAS_STATIC_COLS];
static uint8_t   s_static_dirty [CANVAS_STATIC_ROWS];
static uint16_t  s_static_col_lo[CANVAS_STATIC_ROWS];
static uint16_t  s_static_col_hi[CANVAS_STATIC_ROWS];

static bool canvas_fits_static(uint32_t cols, uint32_t rows)
{
    return cols <= CANVAS_STATIC_COLS && rows <= CANVAS_STATIC_ROWS;
}

/* A never-written cell: default pair, space glyph. The pair is the exact
 * RGB of VIDEO_ATTR_DEFAULT so the VGA backend's draw-time quantisation
 * reproduces the historical attribute byte bit-for-bit. */
static inline TextCell cell_blank(void)
{
    return (TextCell){ .fg = BoxAttrFgRgb(VIDEO_ATTR_DEFAULT),
                       .bg = BoxAttrBgRgb(VIDEO_ATTR_DEFAULT),
                       .ch = ' ' };
}

typedef struct {
    DisplayBackend *be;
    uint32_t        cols;
    uint32_t        rows;
    TextCell       *cells;

    /* Logical cursor — where the next char will go. */
    uint32_t        col;
    uint32_t        row;

    /* Visible caret state (where it currently appears on the screen). */
    uint32_t        caret_col;
    uint32_t        caret_row;
    bool            caret_visible;

    /* Re-entrant batch */
    uint32_t        batch_depth;

    /* RenderPlan accumulator (reset at each outermost begin / after commit) */
    uint32_t        plan_scroll;
    uint8_t        *plan_row_dirty;
    uint16_t       *plan_col_lo;
    uint16_t       *plan_col_hi;

    bool            ready;
} CanvasState;

static CanvasState s_canvas;
static spinlock_t  s_canvas_lock;   /* BSS-zero == unlocked; init in CanvasInit */

/* =========================================================================
 *  Plan helpers (lock-held; no spinlock activity here)
 * ========================================================================= */

static void plan_reset_dirty(CanvasState *c)
{
    memset(c->plan_row_dirty, 0, c->rows);
    for (uint32_t r = 0; r < c->rows; r++) {
        c->plan_col_lo[r] = (uint16_t)0xFFFFu;
        c->plan_col_hi[r] = 0;
    }
}

static void plan_reset(CanvasState *c)
{
    c->plan_scroll = 0;
    plan_reset_dirty(c);
}

static void mark_cell_dirty(CanvasState *c, uint32_t row, uint32_t col)
{
    if (row >= c->rows || col >= c->cols) return;
    c->plan_row_dirty[row] = 1;
    if (col < c->plan_col_lo[row])      c->plan_col_lo[row] = (uint16_t)col;
    if (col + 1u > c->plan_col_hi[row]) c->plan_col_hi[row] = (uint16_t)(col + 1u);
}

static void mark_row_full(CanvasState *c, uint32_t row)
{
    if (row >= c->rows) return;
    c->plan_row_dirty[row] = 1;
    c->plan_col_lo[row]    = 0;
    c->plan_col_hi[row]    = (uint16_t)c->cols;
}

/* Shift cells[][] up by dy rows; bottom dy rows filled with spaces. */
static void cells_scroll_up(CanvasState *c, uint32_t dy)
{
    if (dy == 0) return;
    if (dy >= c->rows) {
        TextCell empty = cell_blank();
        for (uint32_t i = 0; i < c->rows * c->cols; i++) c->cells[i] = empty;
        return;
    }
    memmove(c->cells,
            c->cells + (size_t)dy * c->cols,
            sizeof(TextCell) * (size_t)(c->rows - dy) * c->cols);
    TextCell empty = cell_blank();
    for (uint32_t r = c->rows - dy; r < c->rows; r++)
        for (uint32_t col = 0; col < c->cols; col++)
            c->cells[(size_t)r * c->cols + col] = empty;
}

/* Shift the dirty bitmap + col_lo/col_hi up by dy rows; bottom rows reset
 * to "clean".  Pre-scroll dirty marks at rows < dy are discarded — those
 * cell positions scrolled off-screen along with their content. */
static void dirty_scroll_up(CanvasState *c, uint32_t dy)
{
    if (dy == 0) return;
    if (dy >= c->rows) { plan_reset_dirty(c); return; }
    memmove(c->plan_row_dirty, c->plan_row_dirty + dy, c->rows - dy);
    memset(c->plan_row_dirty + (c->rows - dy), 0, dy);
    memmove(c->plan_col_lo, c->plan_col_lo + dy, sizeof(uint16_t) * (c->rows - dy));
    memmove(c->plan_col_hi, c->plan_col_hi + dy, sizeof(uint16_t) * (c->rows - dy));
    for (uint32_t r = c->rows - dy; r < c->rows; r++) {
        c->plan_col_lo[r] = (uint16_t)0xFFFFu;
        c->plan_col_hi[r] = 0;
    }
}

/* =========================================================================
 *  Commit (caller holds s_canvas_lock)
 * ========================================================================= */

/* Put the caret on the surface at the logical cursor, and remember where it
 * was put — the commit erases it from there next time round. False when this
 * backend has no caret of its own to draw, or the cursor is off the screen.
 *
 * Its own function because a surface swap needs it too: DrawCells alone
 * repaints the glyphs and leaves the new surface with no caret at all. */
static bool draw_caret_locked(CanvasState *c)
{
    DisplayBackend *be = c->be;
    if (!be->DrawCaret) return false;
    if (c->row >= c->rows || c->col >= c->cols) return false;

    const TextCell *cell = &c->cells[(size_t)c->row * c->cols + c->col];
    uint32_t fg = cell->fg;
    /* A fully zeroed pair is a never-initialised cell (black-on-black was
     * attr 0x00 before): draw the caret in the default foreground so it stays
     * visible — same rule as the old code. */
    if (!fg && !cell->bg) fg = BoxAttrFgRgb(VIDEO_ATTR_DEFAULT);

    be->DrawCaret(be, c->col, c->row, fg);
    c->caret_col     = c->col;
    c->caret_row     = c->row;
    c->caret_visible = true;
    return true;
}

static void canvas_commit_locked(CanvasState *c)
{
    if (!c->ready) return;
    DisplayBackend *be = c->be;

    /* Early exit: a Manifest that touched no cells, did no scroll and
     * left the cursor in place has nothing to show.  Without this guard
     * every 1-op Manifest (vga_setcolor, vga_getcursor, etc.) would
     * still cost a caret re-draw + ~64 KB blit per call. */
    bool anything_dirty = (c->plan_scroll > 0);
    if (!anything_dirty) {
        for (uint32_t r = 0; r < c->rows; r++) {
            if (c->plan_row_dirty[r]) { anything_dirty = true; break; }
        }
    }
    bool cursor_moved = !c->caret_visible
                     || c->caret_col != c->col
                     || c->caret_row != c->row;
    if (!anything_dirty && !cursor_moved) {
        plan_reset(c);
        return;
    }

    /* How far step 2 is about to scroll — needed in step 1, which has to
     * follow the caret through that scroll. */
    uint32_t dy = c->plan_scroll;
    if (dy > c->rows) dy = c->rows;

    /* 1. Erase the old caret by marking its cell dirty, so DrawCells repaints
     *    the glyph underneath.  HW-cursor backends (whose DrawCaret moves the
     *    cursor without leaving residue) absorb a harmless extra cell
     *    re-render.
     *
     *    ‼ AND IT IS THE CELL THE CARET ENDS UP IN, NOT THE ONE IT WAS PUT IN.
     *    A backend that draws the caret in pixels puts those pixels inside the
     *    surface, so a scroll carries them up with everything else: after
     *    Scroll(dy) the residue sits dy rows above where it was drawn. Marking
     *    the pre-scroll cell repainted a cell that no longer held the caret,
     *    and the ghost stayed on the glass — one more of them per scroll,
     *    each climbing the screen with the text it was left on.
     *
     *    MEASURED 2026-09-10, UEFI 1280x800: three presses of Enter left FOUR
     *    carets on screen, one real and three riding the old prompt lines
     *    upward. A caret whose row scrolled off the top took its pixels with
     *    it and needs no erasing, which is what the dy bound says. */
    if (c->caret_visible && c->caret_row >= dy) {
        uint32_t residue_row = c->caret_row - dy;
        if (dy > 0 || residue_row != c->row || c->caret_col != c->col)
            mark_cell_dirty(c, residue_row, c->caret_col);
    }

    /* 2. Coalesced scroll covers every '\n' in the batch. */
    if (dy > 0) {
        be->Scroll(be, dy);
    }

    /* 3. Render damaged cells. */
    uint32_t blit_lo = c->rows;
    uint32_t blit_hi = 0;
    for (uint32_t r = 0; r < c->rows; r++) {
        if (!c->plan_row_dirty[r]) continue;
        uint16_t lo = c->plan_col_lo[r];
        uint16_t hi = c->plan_col_hi[r];
        if (lo >= hi) continue;
        be->DrawCells(be, r, lo, hi, &c->cells[(size_t)r * c->cols]);
        if (r < blit_lo)       blit_lo = r;
        if (r + 1u > blit_hi)  blit_hi = r + 1u;
    }

    /* 4. Caret at the new position. */
    if (draw_caret_locked(c)) {
        if (c->caret_row < blit_lo)       blit_lo = c->caret_row;
        if (c->caret_row + 1u > blit_hi)  blit_hi = c->caret_row + 1u;
    }

    /* 5. Present the damage rectangle.  A scroll forces full-text present
     *    (all visible rows changed visually); otherwise present the union
     *    of per-row damage ranges. */
    if (dy > 0) {
        be->Present(be, 0, c->rows);
    } else if (blit_lo < blit_hi) {
        be->Present(be, blit_lo, blit_hi);
    }

    plan_reset(c);
}

/* =========================================================================
 *  Batch / scroll _locked primitives (caller holds s_canvas_lock)
 * ========================================================================= */

static void batch_begin_locked(CanvasState *c)
{
    if (c->batch_depth == 0) plan_reset(c);
    c->batch_depth++;
}

static void batch_end_locked(CanvasState *c)
{
    if (c->batch_depth == 0) return;
    if (--c->batch_depth > 0) return;
    canvas_commit_locked(c);
}

static void perform_scroll_locked(CanvasState *c)
{
    cells_scroll_up(c, 1);
    dirty_scroll_up(c, 1);
    c->plan_scroll++;
    /* New bottom row is blank in cells[]; mark it for render so DrawCells
     * paints the cleared row (or FillRow takes the fast path for backends
     * that detect default-attr space). */
    mark_row_full(c, c->rows - 1);

    /* Cap to keep the counter from drifting toward UINT32_MAX in
     * pathological loops; force an intermediate commit and continue
     * accumulating in a fresh plan. */
    if (c->plan_scroll >= CANVAS_MAX_DEFERRED_SCROLL) {
        uint32_t saved = c->batch_depth;
        c->batch_depth = 0;
        canvas_commit_locked(c);
        c->batch_depth = saved;
        plan_reset(c);
    }
}

static void print_char_locked(CanvasState *c, char ch, uint32_t fg, uint32_t bg)
{
    if (ch == '\n') {
        c->col = 0;
        c->row++;
        if (c->row >= c->rows) { perform_scroll_locked(c); c->row = c->rows - 1; }
    } else if (ch == '\r') {
        c->col = 0;
    } else if (ch == '\b') {
        if (c->col > 0) {
            c->col--;
            c->cells[(size_t)c->row * c->cols + c->col] =
                (TextCell){ .fg = fg, .bg = bg, .ch = ' ' };
            mark_cell_dirty(c, c->row, c->col);
        }
    } else if (ch == '\t') {
        uint32_t target = (c->col + 8u) & ~7u;
        if (target >= c->cols) {
            c->col = 0;
            c->row++;
            if (c->row >= c->rows) { perform_scroll_locked(c); c->row = c->rows - 1; }
        } else {
            c->col = target;
        }
    } else {
        if (c->row < c->rows && c->col < c->cols) {
            c->cells[(size_t)c->row * c->cols + c->col] =
                (TextCell){ .fg = fg, .bg = bg, .ch = ch };
            mark_cell_dirty(c, c->row, c->col);
        }
        c->col++;
        if (c->col >= c->cols) {
            c->col = 0;
            c->row++;
            if (c->row >= c->rows) { perform_scroll_locked(c); c->row = c->rows - 1; }
        }
    }
}

/* Implicit batch helpers: open a transient batch when the caller wasn't
 * already inside one, so a standalone Canvas API call still commits at
 * its boundary. */
static bool implicit_open_locked(CanvasState *c)
{
    if (c->batch_depth == 0) { batch_begin_locked(c); return true; }
    return false;
}
static void implicit_close_locked(CanvasState *c, bool opened)
{
    if (opened) batch_end_locked(c);
}

/* =========================================================================
 *  Lifecycle
 * ========================================================================= */

void CanvasInit(DisplayBackend *be)
{
    if (!be) return;

    /* Init the spinlock formally; before mem_init runs the BSS-zero
     * layout is already an unlocked spinlock, but spinlock_init keeps
     * the contract explicit and idempotent. */
    spinlock_init(&s_canvas_lock);

    s_canvas.be   = be;
    s_canvas.cols = be->cols;
    s_canvas.rows = be->rows;

    if (canvas_fits_static(be->cols, be->rows)) {
        /* Early-boot path: VGA text mode (80×25) sits comfortably in
         * the static arrays — no vmalloc needed, so VideoInit can run
         * before mem_init and kprintf still works for boot-time logs. */
        s_canvas.cells          = s_static_cells;
        s_canvas.plan_row_dirty = s_static_dirty;
        s_canvas.plan_col_lo    = s_static_col_lo;
        s_canvas.plan_col_hi    = s_static_col_hi;
    } else {
        /* Larger surface (GOP framebuffer): runs after vmm_init.  vmalloc
         * is available; cells + plan arrays are allocated dynamically. */
        size_t cell_bytes  = sizeof(TextCell) * s_canvas.rows * s_canvas.cols;
        size_t dirty_bytes = (size_t)s_canvas.rows;
        size_t cl_bytes    = sizeof(uint16_t) * (size_t)s_canvas.rows;

        s_canvas.cells          = (TextCell *)vmalloc(cell_bytes);
        s_canvas.plan_row_dirty = (uint8_t  *)vmalloc(dirty_bytes);
        s_canvas.plan_col_lo    = (uint16_t *)vmalloc(cl_bytes);
        s_canvas.plan_col_hi    = (uint16_t *)vmalloc(cl_bytes);

        if (!s_canvas.cells || !s_canvas.plan_row_dirty ||
            !s_canvas.plan_col_lo || !s_canvas.plan_col_hi) {
            debug_printf("[CANVAS] init: alloc failed (cell=%zu dirty=%zu range=%zu)\n",
                         cell_bytes, dirty_bytes, cl_bytes);
            s_canvas.ready = false;
            return;
        }
    }

    TextCell empty = cell_blank();
    for (size_t i = 0; i < s_canvas.rows * s_canvas.cols; i++)
        s_canvas.cells[i] = empty;
    plan_reset(&s_canvas);

    s_canvas.col           = 0;
    s_canvas.row           = 0;
    s_canvas.caret_col     = 0;
    s_canvas.caret_row     = 0;
    s_canvas.caret_visible = false;
    s_canvas.batch_depth   = 0;
    s_canvas.ready         = true;

    for (uint32_t r = 0; r < s_canvas.rows; r++)
        be->FillRow(be, r, empty.fg, empty.bg);
    be->Present(be, 0, s_canvas.rows);
}

void CanvasReplaceBackend(DisplayBackend *be)
{
    if (!be) return;

    /* Resolution change (e.g. BIOS text → UEFI GOP at boot): drop the
     * cached state and re-init from scratch.  Boot-time only — the
     * orphaned cell/plan allocations are bounded by their sizes and we
     * do not have a vfree() that splits arbitrary heap nodes yet. */
    spin_lock(&s_canvas_lock);
    bool replace = (be->cols != s_canvas.cols || be->rows != s_canvas.rows);
    if (replace) {
        s_canvas.ready = false;
        spin_unlock(&s_canvas_lock);
        CanvasInit(be);
        return;
    }
    s_canvas.be = be;
    for (uint32_t r = 0; r < s_canvas.rows; r++)
        be->DrawCells(be, r, 0, s_canvas.cols, &s_canvas.cells[(size_t)r * s_canvas.cols]);
    /* A new surface carries no caret: the cells were redrawn, the caret was
     * not, and the commit's early exit ("nothing dirty, cursor did not move")
     * would keep it that way until something else printed. */
    (void)draw_caret_locked(&s_canvas);
    be->Present(be, 0, s_canvas.rows);
    spin_unlock(&s_canvas_lock);
}

void CanvasActivatePullMap(void)
{
    spin_lock(&s_canvas_lock);
    if (s_canvas.be && s_canvas.be->ActivatePullMap)
        s_canvas.be->ActivatePullMap(s_canvas.be);
    spin_unlock(&s_canvas_lock);
}

void CanvasNotifyReady(void)
{
    /* Producers / power-state daemons / log collectors subscribe via the
     * tag registry.  TouchPublish is independent of canvas_lock. */
    if (s_canvas.ready) TouchPublish("display:ready", NULL, 0);
}

/* =========================================================================
 *  Batch
 * ========================================================================= */

void CanvasBatchBegin(void)
{
    if (!s_canvas.ready) return;
    spin_lock(&s_canvas_lock);
    batch_begin_locked(&s_canvas);
    spin_unlock(&s_canvas_lock);
}

void CanvasBatchEnd(void)
{
    if (!s_canvas.ready) return;
    spin_lock(&s_canvas_lock);
    batch_end_locked(&s_canvas);
    spin_unlock(&s_canvas_lock);
}

void CanvasForceReset(void)
{
    if (!s_canvas.ready) return;
    /* Panic-path: an interrupted commit may have left the lock held by
     * a dead core.  Reset the lock first so the post-panic kprintf can
     * grab it cleanly; then clear pending plan state so the banner
     * commits immediately on the next CanvasBatchEnd. */
    spin_force_release(&s_canvas_lock);
    s_canvas.batch_depth = 0;
    plan_reset(&s_canvas);
}

/* =========================================================================
 *  Cell writers
 * ========================================================================= */

void CanvasPrintChar(char ch, uint32_t fg, uint32_t bg)
{
    if (!s_canvas.ready) return;
    spin_lock(&s_canvas_lock);
    bool opened = implicit_open_locked(&s_canvas);
    print_char_locked(&s_canvas, ch, fg, bg);
    implicit_close_locked(&s_canvas, opened);
    spin_unlock(&s_canvas_lock);
}

void CanvasScrollUp(void)
{
    if (!s_canvas.ready) return;
    spin_lock(&s_canvas_lock);
    bool opened = implicit_open_locked(&s_canvas);
    perform_scroll_locked(&s_canvas);
    implicit_close_locked(&s_canvas, opened);
    spin_unlock(&s_canvas_lock);
}

void CanvasClearScreen(uint32_t fg, uint32_t bg)
{
    if (!s_canvas.ready) return;
    spin_lock(&s_canvas_lock);
    bool opened = implicit_open_locked(&s_canvas);

    CanvasState *c = &s_canvas;
    TextCell empty = { .fg = fg, .bg = bg, .ch = ' ' };
    for (uint32_t i = 0; i < c->rows * c->cols; i++) c->cells[i] = empty;
    c->plan_scroll = 0;
    plan_reset_dirty(c);
    for (uint32_t r = 0; r < c->rows; r++) mark_row_full(c, r);
    c->col = 0;
    c->row = 0;

    implicit_close_locked(c, opened);
    spin_unlock(&s_canvas_lock);
}

bool CanvasPaint(uint32_t row, uint32_t col, uint32_t height, uint32_t width,
                 const TextCell *cells)
{
    if (!s_canvas.ready || !cells || height == 0 || width == 0) return false;

    spin_lock(&s_canvas_lock);
    CanvasState *c = &s_canvas;

    /* Checked inside the lock: rows/cols change when the backend is replaced
     * (VGA text -> GOP), and a rectangle sized against the old screen must be
     * refused rather than written past the end of the new one. */
    if (row >= c->rows || col >= c->cols ||
        height > c->rows - row || width > c->cols - col) {
        spin_unlock(&s_canvas_lock);
        return false;
    }

    bool opened = implicit_open_locked(c);

    for (uint32_t r = 0; r < height; r++) {
        memcpy(&c->cells[(size_t)(row + r) * c->cols + col],
               &cells[(size_t)r * width],
               sizeof(TextCell) * (size_t)width);
        /* Both ends of the run: mark_cell_dirty widens [col_lo, col_hi) to
         * cover everything between them. */
        mark_cell_dirty(c, row + r, col);
        mark_cell_dirty(c, row + r, col + width - 1u);
    }

    implicit_close_locked(c, opened);
    spin_unlock(&s_canvas_lock);
    return true;
}

void CanvasClearLine(int line, uint32_t fg, uint32_t bg)
{
    if (!s_canvas.ready) return;
    if (line < 0) return;
    spin_lock(&s_canvas_lock);
    CanvasState *c = &s_canvas;
    if ((uint32_t)line >= c->rows) { spin_unlock(&s_canvas_lock); return; }
    bool opened = implicit_open_locked(c);

    TextCell empty = { .fg = fg, .bg = bg, .ch = ' ' };
    for (uint32_t col = 0; col < c->cols; col++)
        c->cells[(size_t)line * c->cols + col] = empty;
    mark_row_full(c, (uint32_t)line);

    implicit_close_locked(c, opened);
    spin_unlock(&s_canvas_lock);
}

void CanvasClearToEol(uint32_t fg, uint32_t bg)
{
    if (!s_canvas.ready) return;
    spin_lock(&s_canvas_lock);
    CanvasState *c = &s_canvas;
    if (c->col >= c->cols || c->row >= c->rows) { spin_unlock(&s_canvas_lock); return; }
    bool opened = implicit_open_locked(c);

    TextCell empty = { .fg = fg, .bg = bg, .ch = ' ' };
    for (uint32_t col = c->col; col < c->cols; col++)
        c->cells[(size_t)c->row * c->cols + col] = empty;
    c->plan_row_dirty[c->row] = 1;
    if (c->col < c->plan_col_lo[c->row]) c->plan_col_lo[c->row] = (uint16_t)c->col;
    c->plan_col_hi[c->row] = (uint16_t)c->cols;

    implicit_close_locked(c, opened);
    spin_unlock(&s_canvas_lock);
}

/* =========================================================================
 *  Cursor + geometry
 * ========================================================================= */

/* Where the next character goes — and therefore where the caret goes, because
 * the caret is the one part of the console a person is actually watching.
 *
 * ‼ THIS MOVED THE CURSOR WITHOUT EVER SHOWING IT.
 *
 * Every other public writer in this file opens an implicit batch, so a call
 * made on its own commits at its own boundary. This one only mutated col/row
 * and left the glass to whoever committed next. That was invisible for as
 * long as the line editor lived in the kernel, because the same code path
 * always printed a character straight afterwards and the print committed for
 * both. With the editor in boxlib, moving the cursor is a Manifest of ONE op
 * — and a single-op Manifest is deliberately not wrapped in a Canvas batch
 * (manifest_exec.c: "the op's own per-call batch already commits at the right
 * boundary"), which was true of every op except this one. So nothing
 * committed, and the caret stayed where the last printed character had left
 * it.
 *
 * MEASURED 2026-09-10, UEFI 1280x800: type `abcdef`, press Left twice — the
 * caret does not move at all; type `X` and the line reads `abcdXef` with the
 * caret two cells past its end. In draft, whose every frame is a paint
 * followed by a cursor move, the caret stayed on the previous frame's line.
 * One defect, both symptoms. */
void CanvasSetCursor(int x, int y)
{
    if (!s_canvas.ready) return;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    spin_lock(&s_canvas_lock);
    CanvasState *c = &s_canvas;
    bool opened = implicit_open_locked(c);
    c->col = (uint32_t)x < c->cols ? (uint32_t)x : c->cols - 1u;
    c->row = (uint32_t)y < c->rows ? (uint32_t)y : c->rows - 1u;
    implicit_close_locked(c, opened);
    spin_unlock(&s_canvas_lock);
}

int CanvasGetCursorX(void)
{
    if (!s_canvas.ready) return 0;
    spin_lock(&s_canvas_lock);
    int x = (int)s_canvas.col;
    spin_unlock(&s_canvas_lock);
    return x;
}

int CanvasGetCursorY(void)
{
    if (!s_canvas.ready) return 0;
    spin_lock(&s_canvas_lock);
    int y = (int)s_canvas.row;
    spin_unlock(&s_canvas_lock);
    return y;
}

void CanvasUpdateCursor(void)
{
    if (!s_canvas.ready) return;
    spin_lock(&s_canvas_lock);
    /* Inside a batch, the commit will draw the caret.  At depth 0 we
     * trigger a tiny commit to refresh the visible caret position. */
    if (s_canvas.batch_depth == 0) {
        batch_begin_locked(&s_canvas);
        batch_end_locked(&s_canvas);
    }
    spin_unlock(&s_canvas_lock);
}

int CanvasGetCols(void) { return s_canvas.ready ? (int)s_canvas.cols : 0; }
int CanvasGetRows(void) { return s_canvas.ready ? (int)s_canvas.rows : 0; }
