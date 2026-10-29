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

void ThemSave(ThemSaved *saved)
{
    saved->Files = g_files;
    saved->Count = g_count;
    saved->Known = g_known;
    g_files = NULL;
    g_count = 0;
    g_known = false;
}

void ThemRestore(ThemSaved *saved)
{
    free(g_files);
    g_files = saved->Files;
    g_count = saved->Count;
    g_known = saved->Known;
    saved->Files = NULL;
}

void ThemRelease(ThemSaved *saved)
{
    free(saved->Files);
    saved->Files = NULL;
}

void ThemReplace(file_id_t old, file_id_t made)
{
    for (size_t i = 0; i < g_count; i++) {
        if (g_files[i] == old) g_files[i] = made;
    }
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
