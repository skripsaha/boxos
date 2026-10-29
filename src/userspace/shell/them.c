#include "them.h"
#include "box/memory.h"

static file_id_t *g_files;
static size_t     g_count;
static bool       g_known;

void ThemKeep(file_id_t *ids, size_t count)
{
    free(g_files);
    g_files = ids;
    g_count = count;
    g_known = true;
}

void ThemShow(BoxFormLookup *lookup)
{
    lookup->Them      = g_files;
    lookup->ThemCount = g_count;
    lookup->ThemKnown = g_known;
}

const file_id_t *ThemFiles(size_t *count, bool *known)
{
    *count = g_count;
    *known = g_known;
    return g_files;
}
