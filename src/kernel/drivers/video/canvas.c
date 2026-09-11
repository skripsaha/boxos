
#include "canvas.h"
#include "klib.h"
#include "vmm.h"
#include "touch.h"

#define CANVAS_MAX_DEFERRED_SCROLL  4096u

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

    uint32_t        col;
    uint32_t        row;

    uint32_t        caret_col;
    uint32_t        caret_row;
    bool            caret_visible;

    uint32_t        batch_depth;

    uint32_t        plan_scroll;
    uint8_t        *plan_row_dirty;
    uint16_t       *plan_col_lo;
    uint16_t       *plan_col_hi;

    bool            ready;
} CanvasState;

static CanvasState s_canvas;
static spinlock_t  s_canvas_lock;


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


static bool draw_caret_locked(CanvasState *c)
{
    DisplayBackend *be = c->be;
    if (!be->DrawCaret) return false;
    if (c->row >= c->rows || c->col >= c->cols) return false;

    const TextCell *cell = &c->cells[(size_t)c->row * c->cols + c->col];
    uint32_t fg = cell->fg;
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

    uint32_t dy = c->plan_scroll;
    if (dy > c->rows) dy = c->rows;

    if (c->caret_visible && c->caret_row >= dy) {
        uint32_t residue_row = c->caret_row - dy;
        if (dy > 0 || residue_row != c->row || c->caret_col != c->col)
            mark_cell_dirty(c, residue_row, c->caret_col);
    }

    if (dy > 0) {
        be->Scroll(be, dy);
    }

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

    if (draw_caret_locked(c)) {
        if (c->caret_row < blit_lo)       blit_lo = c->caret_row;
        if (c->caret_row + 1u > blit_hi)  blit_hi = c->caret_row + 1u;
    }

    if (dy > 0) {
        be->Present(be, 0, c->rows);
    } else if (blit_lo < blit_hi) {
        be->Present(be, blit_lo, blit_hi);
    }

    plan_reset(c);
}


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
    mark_row_full(c, c->rows - 1);

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

static bool implicit_open_locked(CanvasState *c)
{
    if (c->batch_depth == 0) { batch_begin_locked(c); return true; }
    return false;
}
static void implicit_close_locked(CanvasState *c, bool opened)
{
    if (opened) batch_end_locked(c);
}


void CanvasInit(DisplayBackend *be)
{
    if (!be) return;

    spinlock_init(&s_canvas_lock);

    s_canvas.be   = be;
    s_canvas.cols = be->cols;
    s_canvas.rows = be->rows;

    if (canvas_fits_static(be->cols, be->rows)) {
        s_canvas.cells          = s_static_cells;
        s_canvas.plan_row_dirty = s_static_dirty;
        s_canvas.plan_col_lo    = s_static_col_lo;
        s_canvas.plan_col_hi    = s_static_col_hi;
    } else {
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
    if (s_canvas.ready) TouchPublish("display:ready", NULL, 0);
}


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
    spin_force_release(&s_canvas_lock);
    s_canvas.batch_depth = 0;
    plan_reset(&s_canvas);
}


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
    if (s_canvas.batch_depth == 0) {
        batch_begin_locked(&s_canvas);
        batch_end_locked(&s_canvas);
    }
    spin_unlock(&s_canvas_lock);
}

int CanvasGetCols(void) { return s_canvas.ready ? (int)s_canvas.cols : 0; }
int CanvasGetRows(void) { return s_canvas.ready ? (int)s_canvas.rows : 0; }