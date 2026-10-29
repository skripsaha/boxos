#include "box/print.h"
#include "box/system.h"
#include "box/error.h"
#include "box/timeouts.h"
#include "box/core/manifest.h"
#include "quilltest.h"

/*
 * quilltest escheat — Escheat asked on a book whose Tally names a rotten range
 * (logcheck book_escheat; spec_v2 §16.5 п.20):
 *
 *   refused   the program takes the system tag off itself first, then asks:
 *             a cabin wearing neither system nor god is refused — at the
 *             gate (ERR_ACCESS_DENIED) or past it (ERR_PERMISSION_DENIED),
 *             and nothing is let go
 *   act       asked as quilltest is, wearing system: what the act lets go —
 *             the rotten ranges, the files declared lost, the blocks let go,
 *             the tails lost, the DIGESTS leaves laid anew of the data — the
 *             next Tally names none of it
 */

typedef struct {
    uint64_t ranges;
    uint64_t files;
    uint64_t blocks;
    uint64_t tails;
    uint64_t digest_ranges;
} QuillEscheatSaid;

static int QuillEscheatAsk(QuillEscheatSaid* said)
{
    return MfCall1(DECK_STORAGE, STORAGE_OBJ_ESCHEAT, NULL, 0, NULL, 0, said, sizeof(*said), NULL,
                   BOX_ANSWER_GUARANTEED, NULL);
}

QuillVerdict QuillBookEscheat(const char *const *args)
{
    QuillEscheatSaid said = { 0 };
    bool             refused = args[0][0] == 'r';
    int              rc;

    if (refused && proc_tag_remove("system") != 0) {
        printf("[QUILL] escheat: the system tag would not come off this program\n");
        return QUILL_FAIL;
    }
    rc = QuillEscheatAsk(&said);
    if (refused) {
        bool denied = rc == ERR_ACCESS_DENIED || rc == ERR_PERMISSION_DENIED;
        printf("[QUILL] escheat %s: a cabin wearing neither system nor god is answered %d\n",
               denied ? "refused" : "NOT REFUSED", rc);
        return denied ? QUILL_PASS : QUILL_DEFECT;
    }
    printf("[QUILL] escheat %s: answered %d — %lu range(s), %lu file(s) lost, %lu block(s) let go, %lu tail(s) "
           "lost, %lu DIGESTS range(s) laid anew\n", rc == 0 ? "asked" : "FAILED", rc, (unsigned long)said.ranges,
           (unsigned long)said.files, (unsigned long)said.blocks, (unsigned long)said.tails,
           (unsigned long)said.digest_ranges);
    return rc == 0 && said.ranges ? QUILL_PASS : QUILL_DEFECT;
}
