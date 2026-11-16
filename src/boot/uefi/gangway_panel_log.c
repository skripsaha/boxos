/*
 * Gangway — the panel's two journals, each shown on a page of its own until
 * the person goes back: this start's, what the loader has said so far
 * (JournalText), and what the kernel said in the start before, out of the
 * carry-over window it keeps through a reset (boxos_logkeep.h).
 *
 * The window is read where it lies and nothing is written into it: only when
 * the firmware's map has memory under every byte of it, and only when its
 * header is a header this layout's kernel wrote — its magic, its own size, a
 * bank's — is a bank shown as a log. The header names the bank the last start
 * that kept a log wrote, and which start that was; the last LOGKEEP_BANK_BYTES
 * of what it said are there, byte `p` at `p % LOGKEEP_BANK_BYTES`, and are
 * shown in the order they were said. Everything else is a sentence of why
 * there is no log to show.
 *
 * A page opens at its end, where the last was said; ↑↓, PgUp PgDn, Home End
 * move through it, and Esc or Enter goes back to the panel.
 */

#include "gangway.h"
#include "boxos_logkeep.h"
#include "video_colors.h"

/* What the kernel kept of the start before: up to two runs of text in the
 * order they were said, how many bytes were said, and which start it was. */
typedef struct {
    const char *first;
    uint32_t    first_bytes;
    const char *then;
    uint32_t    then_bytes;
    uint64_t    said;
    uint32_t    start;
} KeptLog;

/* Every byte of the window under memory the firmware's map has as RAM. */
static int WindowIsMemory(void)
{
    UINTN    size = 0, key = 0, step = 0;
    uint32_t version = 0;
    uint8_t *map     = NULL;
    uint64_t covered = 0;

    if (g_bs->get_memory_map(&size, NULL, &key, &step, &version) != EFI_BUFFER_TOO_SMALL || !step) return 0;
    size += 8u * step;
    if (EFI_ERROR(g_bs->allocate_pool(EfiLoaderData, size, (void **)&map))) return 0;
    if (EFI_ERROR(g_bs->get_memory_map(&size, (EFI_MEMORY_DESCRIPTOR *)map, &key, &step, &version))) {
        g_bs->free_pool(map);
        return 0;
    }
    for (UINTN at = 0; at + step <= size; at += step) {
        const EFI_MEMORY_DESCRIPTOR *d = (const EFI_MEMORY_DESCRIPTOR *)(map + at);
        if (d->type != EfiConventionalMemory && d->type != EfiLoaderCode && d->type != EfiLoaderData &&
            d->type != EfiBootServicesCode && d->type != EfiBootServicesData)
            continue;
        uint64_t from = d->physical_start, to = d->physical_start + d->number_of_pages * PAGE_4KB;
        if (from < LOGKEEP_PHYS) from = LOGKEEP_PHYS;
        if (to > LOGKEEP_PHYS + LOGKEEP_BYTES) to = LOGKEEP_PHYS + LOGKEEP_BYTES;
        if (to > from) covered += to - from;
    }
    g_bs->free_pool(map);
    return covered == LOGKEEP_BYTES;
}

/* The log in the window: NULL, or why there is none to show. */
static const char *Kept(KeptLog *log)
{
    if (!WindowIsMemory())
        return "this machine's firmware has no memory where the kernel keeps it — at 64 MiB";

    const volatile LogKeepHeader *h = (const volatile LogKeepHeader *)(uintptr_t)LOGKEEP_PHYS;
    if (h->magic != LOGKEEP_MAGIC)
        return "nothing is kept there — keep-log was off, the power was cut, or the firmware cleared the memory "
               "as it started";
    if (h->size != (uint32_t)sizeof(LogKeepHeader) || h->capacity != LOGKEEP_BANK_BYTES || h->current >= LOGKEEP_BANKS)
        return "what is kept there was laid out by a kernel of another layout";

    uint64_t    said = h->bytes[h->current];
    const char *bank = (const char *)(uintptr_t)(LOGKEEP_PHYS + LOGKEEP_HEADER +
                                                 (uint64_t)h->current * LOGKEEP_BANK_BYTES);
    if (!said) return "the start that kept it said nothing into it";
    log->said  = said;
    log->start = h->boot;
    if (said <= LOGKEEP_BANK_BYTES) {
        log->first       = bank;
        log->first_bytes = (uint32_t)said;
        log->then        = NULL;
        log->then_bytes  = 0;
    } else {
        uint32_t at      = (uint32_t)(said % LOGKEEP_BANK_BYTES);
        log->first       = bank + at;
        log->first_bytes = LOGKEEP_BANK_BYTES - at;
        log->then        = bank;
        log->then_bytes  = at;
    }
    return NULL;
}

/* The page: `title` on the top row, `body` on the rows between, scrolled; a
 * line on the last row of how to move and where in it the rows shown are. */
static void Shown(const PanelPage *title, const PanelPage *body)
{
    static PanelPage foot;
    uint32_t         rows   = DrawRows();
    uint32_t         height = rows > 2u ? rows - 2u : 1u;
    uint32_t         filled = DrawCount(body->pieces, body->count);
    uint32_t         last   = filled > height ? filled - height : 0;
    uint32_t         skip   = last;

    for (;;) {
        PanelBegin(&foot);
        PanelPut(&foot, VIDEO_ATTR_HINT, " ↑↓ PgUp PgDn Home End · Esc back · rows ");
        PanelNum(&foot, VIDEO_ATTR_HINT, filled ? skip + 1u : 0u);
        PanelPut(&foot, VIDEO_ATTR_HINT, "–");
        PanelNum(&foot, VIDEO_ATTR_HINT, skip + height < filled ? skip + height : filled);
        PanelPut(&foot, VIDEO_ATTR_HINT, " of ");
        PanelNum(&foot, VIDEO_ATTR_HINT, filled);
        DrawPage(title->pieces, title->count, 0, 0, 1);
        DrawPage(body->pieces, body->count, skip, 1, height);
        DrawPage(foot.pieces, foot.count, 0, rows - 1u, 1);

        switch (KeyNext()) {
        case KEY_UP:        if (skip) skip--; break;
        case KEY_DOWN:      if (skip < last) skip++; break;
        case KEY_PAGE_UP:   skip = skip > height ? skip - height : 0; break;
        case KEY_PAGE_DOWN: skip = skip + height < last ? skip + height : last; break;
        case KEY_HOME:      skip = 0; break;
        case KEY_END:       skip = last; break;
        case KEY_LEFT:
        case KEY_RIGHT:     break;
        default:            return;
        }
    }
}

void PanelJournalShow(void)
{
    static PanelPage title, body;
    uint32_t         bytes, lost;
    const char      *journal = JournalText(&bytes, &lost);

    PrintAloud(1);
    Print("Gangway: the panel shows this start's journal\n");
    PrintAloud(0);

    PanelBegin(&title);
    PanelPut(&title, VIDEO_ATTR_INPUT, " This start's journal — ");
    PanelNum(&title, VIDEO_ATTR_INPUT, bytes);
    PanelPut(&title, VIDEO_ATTR_INPUT, " bytes");
    if (lost) {
        PanelPut(&title, VIDEO_ATTR_WARNING, ", and ");
        PanelNum(&title, VIDEO_ATTR_WARNING, lost);
        PanelPut(&title, VIDEO_ATTR_WARNING, " more said than it had room for");
    }
    PanelBegin(&body);
    PanelRaw(&body, VIDEO_ATTR_DEFAULT, journal, bytes);
    Shown(&title, &body);
}

void PanelPreviousShow(void)
{
    static PanelPage title, body;
    KeptLog          log  = { NULL, 0, NULL, 0, 0, 0 };
    const char      *none = Kept(&log);

    PrintAloud(1);
    Print("Gangway: the panel shows the previous start's log");
    if (none) {
        Print(" — there is none: ");
        Print(none);
    } else {
        Print(" (start ");
        PrintDec(log.start);
        Print(", ");
        PrintDec(log.said);
        Print(" bytes said)");
    }
    Print("\n");
    PrintAloud(0);

    PanelBegin(&title);
    PanelBegin(&body);
    if (none) {
        PanelPut(&title, VIDEO_ATTR_INPUT, " The previous start's log");
        PanelPut(&body, VIDEO_ATTR_WARNING, "\n There is none to show: ");
        PanelPut(&body, VIDEO_ATTR_WARNING, none);
        PanelPut(&body, VIDEO_ATTR_WARNING, ".\n\n");
        PanelPut(&body, VIDEO_ATTR_DEFAULT,
                 " The kernel keeps what it says through a reset when keep-log is on — in gangway.settings, or on\n"
                 " this panel for one start. It lasts through a reset, not through the power cut off.\n");
    } else {
        PanelPut(&title, VIDEO_ATTR_INPUT, " The previous start's log — start ");
        PanelNum(&title, VIDEO_ATTR_INPUT, log.start);
        PanelPut(&title, VIDEO_ATTR_INPUT, ", the last that kept one: ");
        PanelNum(&title, VIDEO_ATTR_INPUT, log.said);
        PanelPut(&title, VIDEO_ATTR_INPUT, " bytes said");
        if (log.said > LOGKEEP_BANK_BYTES) PanelPut(&title, VIDEO_ATTR_INPUT, ", the last 1 MiB of them kept");
        PanelRaw(&body, VIDEO_ATTR_DEFAULT, log.first, log.first_bytes);
        PanelRaw(&body, VIDEO_ATTR_DEFAULT, log.then, log.then_bytes);
    }
    Shown(&title, &body);
}
