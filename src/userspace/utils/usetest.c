
#include "box/print.h"
#include "box/file.h"
#include "box/use.h"
#include "box/system.h"
#include "box/string.h"
#include "box/memory.h"
#include "box/error.h"

#define PROBE_TAG    "use:probe"
#define PROBE_INSIDE "use_probe_in"
#define PROBE_OUT    "use_probe_out"

static bool has_id(const uint32_t *ids, int count, uint32_t id)
{
    for (int i = 0; i < count; i++)
        if (ids[i] == id) return true;
    return false;
}

static int fail(const char *what, int rc)
{
    if (rc < 0) printf("[USE] FAIL: %s (error %d)\n", what, (int)box_errno_of(rc));
    else        printf("[USE] FAIL: %s\n", what);
    return 1;
}

int main(void)
{

    size_t saved_need = 0;
    int    saved_tags = use_get(NULL, 0, &saved_need);
    if (saved_tags < 0) return fail("use_get refused", saved_tags);
    char *saved = NULL;
    if (saved_need > 1) {
        saved = malloc(saved_need);
        if (!saved) return fail("no memory for the saved context", 0);
        if (use_get(saved, saved_need, NULL) < 0) return fail("use_get of the saved context", 0);
    }

    int verdict = 0;
    int rc;
    int inside_id = 0, out_id = 0;

    bool remembered = false;
    rc = use_set(PROBE_TAG, &remembered);
    if (rc < 0) { verdict = fail("use_set from a system program", rc); goto restore; }
    if (!remembered) { verdict = fail("the volume did not remember the context", 0); goto restore; }

    char got[128];
    rc = use_get(got, sizeof(got), NULL);
    if (rc != 1 || strcmp(got, PROBE_TAG) != 0) { verdict = fail("use_get does not echo the context", rc < 0 ? rc : 0); goto restore; }

    inside_id = create(PROBE_INSIDE, NULL);
    if (inside_id <= 0) { verdict = fail("create inside the context", inside_id); goto restore; }
    out_id = create_everywhere(PROBE_OUT, NULL);
    if (out_id <= 0) { verdict = fail("create everywhere", out_id); goto restore; }

    uint32_t ids[256];
    int n;

    n = query(NULL, ids, 256);
    if (n < 0) { verdict = fail("query inside", n); goto restore; }
    if (!has_id(ids, n, (uint32_t)inside_id)) { verdict = fail("a file created inside is not seen inside", 0); goto restore; }
    if (has_id(ids, n, (uint32_t)out_id))      { verdict = fail("a file created everywhere is seen inside", 0); goto restore; }

    n = query_everywhere(NULL, ids, 256);
    if (n < 0) { verdict = fail("query everywhere", n); goto restore; }
    if (!has_id(ids, n, (uint32_t)inside_id) || !has_id(ids, n, (uint32_t)out_id)) {
        verdict = fail("query everywhere does not see both files", 0);
        goto restore;
    }

    n = query_everywhere(PROBE_TAG, ids, 256);
    if (n < 0) { verdict = fail("query everywhere by the context tag", n); goto restore; }
    if (!has_id(ids, n, (uint32_t)inside_id) || has_id(ids, n, (uint32_t)out_id)) {
        verdict = fail("the context tag was not stamped on exactly the inside file", 0);
        goto restore;
    }

    rc = use_clear(&remembered);
    if (rc == 0 && !remembered) { verdict = fail("the volume did not remember the clear", 0); goto restore; }
    if (rc < 0) { verdict = fail("use_clear", rc); goto restore; }
    n = query(NULL, ids, 256);
    if (n < 0) { verdict = fail("query after clear", n); goto restore; }
    if (!has_id(ids, n, (uint32_t)inside_id) || !has_id(ids, n, (uint32_t)out_id)) {
        verdict = fail("after use_clear both files should be visible", 0);
        goto restore;
    }

restore:
    if (inside_id > 0) delete((uint32_t)inside_id);
    if (out_id > 0)    delete((uint32_t)out_id);
    if (saved) {
        use_set(saved, NULL);
        free(saved);
    } else {
        use_clear(NULL);
    }

    if (verdict == 0) println("[USE] PASS");
    exit(verdict);
    return verdict;
}