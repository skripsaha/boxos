/* finish — end a running program by the tag that names it, never by a number.
 *
 * A person knows what is running by its name. Every file on the volume wears
 * the stem of its filename as its first tag and a program inherits its file's
 * tags, so "finish playtime" is the ordinary spelling and there is nothing to
 * look up first. A tag names a role, and several cabins may wear one — here
 * that is the normal case, not the exception, so this ends the whole crew of
 * a tag rather than an individual.
 *
 * Each one is ended by (pid, generation), the pair that names an incarnation.
 * A pid alone is a seat and seats are re-let: between the crew answer and the
 * ending a cabin can leave and another take its place, and an ending by seat
 * alone would land on the successor and report success.
 *
 *   finish playtime           -> ends every cabin wearing "playtime"
 *   finish playtime utility   -> only those wearing BOTH tags
 *   finish shell anyway       -> ends it although it runs the machine
 *
 * The guard on the bare tag "system" is the part a future reader must not
 * undo: the shell, the display daemon and this very command wear it, and
 * ending one leaves a person at a screen that cannot answer. "anyway" as the
 * last word is the whole of that consent — typed by hand, on purpose — and it
 * must stay the only way past the guard.
 */

#include "box/print.h"
#include "box/color.h"
#include "box/luggage.h"
#include "box/system.h"
#include "box/string.h"
#include "box/convert.h"
#include "box/memory.h"
#include "box/error.h"
#include "box/core/cabin.h"

#define USAGE_LINE     "Usage: finish <tag> [more tags...] [anyway]"
#define ANYWAY_WORD    "anyway"
#define SYSTEM_TAG     "system"
#define TAG_MAX_BYTES  63u      /* proc_crew refuses anything longer */
#define COUNT_DIGITS   12       /* room for any int to_str can write */

/* What was decided about one cabin. It is decided ONCE, in decide_crew, and
 * every pass afterwards only reads it. An earlier version derived "who is
 * guarded" a second time, from scratch and in a different order, purely to
 * print: two places holding one rule, where editing either alone gets a
 * system cabin killed with no notice, or a notice printed for a cabin that
 * dies anyway. */
typedef enum MateVerdict {
    VERDICT_SKIPPED = 0,        /* wears the wrong tags, or is this command */
    VERDICT_CHOSEN  = 1,
    VERDICT_GUARDED = 2
} MateVerdict;

typedef struct Crew {
    ProcMate    *mates;
    MateVerdict *verdict;       /* one per delivered mate, parallel to mates */
    int          found;         /* delivered into this one answer */
    uint32_t     total;         /* wearing the tag altogether */
    int          matched;       /* wearing every tag that was asked */
    int          picked;        /* chosen to be ended */
    int          guarded;       /* held back by the guard */
} Crew;

/* True iff comma-joined `list` holds exactly `token`. Element-exact, so "app"
 * never matches inside "apple": the kernel's own crew walk decides membership
 * this way (system_ops.c tag_list_contains), and a narrowing word judged by
 * substring instead would end a crew nobody asked for. */
static bool wears(const char *list, const char *token)
{
    size_t tlen = strlen(token);
    for (const char *p = list; p && *p; ) {
        const char *comma = strchr(p, ',');
        size_t elen = comma ? (size_t)(comma - p) : strlen(p);
        if (elen == tlen && memcmp(p, token, tlen) == 0) return true;
        if (!comma) break;
        p = comma + 1;
    }
    return false;
}

/* Small counts read as words — "two went ashore", not "2 went ashore". Past
 * three the digit is plainer than the spelling. `out` holds COUNT_DIGITS. */
static const char *counted(int n, char *out, size_t cap)
{
    if (n == 1) return "one";
    if (n == 2) return "two";
    if (n == 3) return "three";
    return to_str(n, out, cap);
}

/* The crew answer joins a cabin's tags with a bare comma; the space after it
 * is ours, because "playtime, utility" is how a person reads a list. NULL
 * when the heap is out, and the caller then says the joined form as it came. */
static char *spaced_tags(const char *list)
{
    size_t n = strlen(list), commas = 0;
    for (size_t i = 0; i < n; i++) if (list[i] == ',') commas++;

    char *out = (char *)malloc(n + commas + 1);
    if (!out) return NULL;

    size_t w = 0;
    for (size_t i = 0; i < n; i++) {
        out[w++] = list[i];
        if (list[i] == ',') out[w++] = ' ';
    }
    out[w] = '\0';
    return out;
}

/* The tags as the person typed them, space-joined — for the lines that must
 * name what was asked. "anyway" is never in it: it is permission, not a tag. */
static char *asked_phrase(int last)
{
    size_t room = 1;
    for (int i = 1; i <= last; i++) room += strlen(luggage_word((uint32_t)i)) + 1;

    char *out = (char *)malloc(room);
    if (!out) return NULL;

    size_t w = 0;
    for (int i = 1; i <= last; i++) {
        const char *word = luggage_word((uint32_t)i);
        size_t len = strlen(word);
        if (w) out[w++] = ' ';
        memcpy(out + w, word, len);
        w += len;
    }
    out[w] = '\0';
    return out;
}

static void free_crew(Crew *crew)
{
    free(crew->verdict);
    free(crew->mates);
    crew->verdict = NULL;
    crew->mates   = NULL;
}

/* Asks who wears the tag and takes the room to judge them. Returns 0 with
 * `crew` filled, or -1 having already said why in the person's words. */
static int ask_crew(const char *ask, Crew *crew)
{
    crew->mates   = NULL;
    crew->verdict = NULL;
    crew->found   = 0;
    crew->total   = 0;
    crew->matched = 0;
    crew->picked  = 0;
    crew->guarded = 0;

    int found = proc_crew(ask, &crew->mates, &crew->total);
    if (found < 0) {
        error_t why = box_errno_of(found);
        /* An empty tag and a faulted transport answer ERR_INVALID_ARGUMENT
         * too. Blaming length for all three sent a person off to shorten a
         * tag that was already short, so length is said only where this
         * program has measured it and it is true. */
        if (why == ERR_INVALID_ARGUMENT && strlen(ask) > TAG_MAX_BYTES)
            printf("%color%s: a tag is at most %u bytes long%color\n",
                   COLOR_RED, ask, TAG_MAX_BYTES, COLOR_DEFAULT);
        else
            printf("%color%s: the crew could not be asked, error %u%color\n",
                   COLOR_RED, ask, (unsigned)why, COLOR_DEFAULT);
        return -1;
    }

    crew->found = found;
    if (found > 0) {
        crew->verdict = (MateVerdict *)malloc((size_t)found * sizeof(MateVerdict));
        if (!crew->verdict) {
            println("there is not enough memory to hold the crew");
            free(crew->mates);
            crew->mates = NULL;
            return -1;
        }
    }
    return 0;
}

/* Who is ended, decided before a word is printed and before anyone is
 * touched: the guard has to speak ahead of the crew listing, and the listing
 * has to be complete before the first cabin goes.
 *
 * Liveness is the kernel's promise here, not this program's guess: the crew
 * answer holds only cabins that were live when it was walked, so the DONE and
 * CRASHED test this pass used to carry — in two copies — could never fire. */
static void decide_crew(Crew *crew, int last, bool anyway)
{
    uint32_t self = cabin_info()->pid;

    for (int i = 0; i < crew->found; i++) {
        crew->verdict[i] = VERDICT_SKIPPED;

        bool all = true;
        for (int w = 2; w <= last && all; w++)
            all = wears(crew->mates[i].tags, luggage_word((uint32_t)w));
        if (!all) continue;
        crew->matched++;

        /* Never ourselves: this command wears its own name tag, and one that
         * ends itself half way through its work is a bug that reads as a
         * feature. */
        if (crew->mates[i].pid == self) continue;

        if (!anyway && wears(crew->mates[i].tags, SYSTEM_TAG)) {
            crew->verdict[i] = VERDICT_GUARDED;
            crew->guarded++;
            continue;
        }

        crew->verdict[i] = VERDICT_CHOSEN;
        crew->picked++;
    }
}

/* A crew too large for one answer was ended in part and reported as success:
 * the rest kept running and the status said the tag was clear. The kernel
 * hands back how many wear it altogether, so the shortfall is said before
 * anyone is ended, and the closing status carries it as well. */
static void say_shortfall(const Crew *crew, const char *ask)
{
    printf("%color%u of the %u wearing %s are in this answer"
           " - ask more narrowly%color\n",
           COLOR_RED, (unsigned)crew->found, (unsigned)crew->total, ask,
           COLOR_DEFAULT);
}

/* The guard names the CABIN it is holding back, by the tags that cabin
 * actually wears — not the tag that was asked for. "finish utility" reaches
 * the shell, and answering "utility wears system" would be a sentence about
 * the wrong thing: utility does not wear system, the shell does, and the
 * shell is what the person has to recognise before typing anyway. */
static void say_guarded(const Crew *crew, const char *phrase)
{
    if (crew->guarded == 0) return;

    for (int i = 0; i < crew->found; i++) {
        if (crew->verdict[i] != VERDICT_GUARDED) continue;
        char *pretty = spaced_tags(crew->mates[i].tags);
        printf("%color%s%color wears %s - this machine runs on it.\n",
               COLOR_CYAN, pretty ? pretty : crew->mates[i].tags, COLOR_DEFAULT,
               SYSTEM_TAG);
        free(pretty);
    }
    printf("if that is what you want: finish %s %s\n", phrase, ANYWAY_WORD);
}

/* The crew about to go, each named by the tags it wears — the same name every
 * outcome line uses, so one that fails can be told from its neighbours. */
static void say_listing(const Crew *crew)
{
    for (int i = 0; i < crew->found; i++) {
        if (crew->verdict[i] != VERDICT_CHOSEN) continue;
        char *pretty = spaced_tags(crew->mates[i].tags);
        printf("%color%s%color - %u.%u s aboard\n",
               COLOR_CYAN, pretty ? pretty : crew->mates[i].tags, COLOR_DEFAULT,
               (unsigned)(crew->mates[i].cpu_us / 1000000u),
               (unsigned)((crew->mates[i].cpu_us % 1000000u) / 100000u));
        free(pretty);
    }
}

/* Ends the chosen, naming each one as the listing named it. These lines used
 * to repeat the tag that was typed, so a crew of three produced three
 * identical "ended display" lines and a failure among them belonged to
 * nobody in particular. */
static void end_crew(const Crew *crew, int *out_ended, int *out_refused)
{
    for (int i = 0; i < crew->found; i++) {
        if (crew->verdict[i] != VERDICT_CHOSEN) continue;

        char       *pretty = spaced_tags(crew->mates[i].tags);
        const char *name   = pretty ? pretty : crew->mates[i].tags;

        int rc = proc_finish(crew->mates[i].pid, crew->mates[i].generation);
        if (rc == 0) {
            /* The death is committed the moment finish answers, so asking for
             * it as a state returns at once and "ended" is not a guess. The
             * exit code is deliberately not read: a committed death carries
             * none, and "ended with 0" for something we killed is a small lie. */
            (void)process_gone(crew->mates[i].pid, crew->mates[i].generation, NULL);
            printf("ended %color%s%color\n", COLOR_CYAN, name, COLOR_DEFAULT);
            (*out_ended)++;
            free(pretty);
            continue;
        }

        error_t why = box_errno_of(rc);
        if (why == ERR_PROCESS_NOT_FOUND) {
            /* It left between the asking and the ending — the generation is
             * exactly what made the kernel say so instead of ending its
             * successor. Nothing failed, so nothing is refused. */
            printf("%s: already gone\n", name);
        } else if (why == ERR_ACCESS_DENIED) {
            printf("%color%s: this cabin has no authority over it%color\n",
                   COLOR_RED, name, COLOR_DEFAULT);
            (*out_refused)++;
        } else {
            printf("%color%s: refused, error %u%color\n",
                   COLOR_RED, name, (unsigned)why, COLOR_DEFAULT);
            (*out_refused)++;
        }
        free(pretty);
    }
}

/* The closing lines answer "what happened to the crew", so what was held back
 * belongs in them: saying only "one went ashore" left the person to work out
 * on their own that a daemon they named is still running. */
static void closing_report(const Crew *crew, int ended)
{
    char held[COUNT_DIGITS], reached[COUNT_DIGITS];

    if (ended == 0) println("nothing ended");
    else            printf("%s went ashore\n", counted(ended, held, sizeof(held)));

    if (crew->guarded > 0)
        printf("%s of %s held back\n",
               counted(crew->guarded, held, sizeof(held)),
               counted(crew->picked + crew->guarded, reached, sizeof(reached)));
}

int main(void)
{
    int argc = (int)luggage_word_count();
    if (argc < 2 || luggage_word(1)[0] == '\0') {
        println(USAGE_LINE);
        exit(1);
        return 1;
    }

    /* The LAST word is consent whenever it reads "anyway", with no exception:
     * "finish anyway" spends its only word on consent and lands on the usage
     * line below. Anywhere earlier it is a tag like any other, so the crew of
     * a tag called "anyway" is reached by putting a word after it — "finish
     * anyway anyway" is the shortest spelling. */
    int  last   = argc - 1;
    bool anyway = strcmp(luggage_word((uint32_t)last), ANYWAY_WORD) == 0;
    if (anyway) last--;

    if (last < 1) {
        println(USAGE_LINE);
        exit(1);
        return 1;
    }

    const char *ask    = luggage_word(1);
    char       *phrase = asked_phrase(last);
    if (!phrase) {
        println("there is not enough memory to read what you asked");
        exit(1);
        return 1;
    }

    Crew crew;
    if (ask_crew(ask, &crew) != 0) {
        free(phrase);
        exit(1);
        return 1;
    }

    decide_crew(&crew, last, anyway);

    if (crew.matched == 0) {
        printf("%s: nothing wears %s\n", phrase, last > 1 ? "those tags" : "that tag");
        free_crew(&crew);
        free(phrase);
        exit(1);
        return 1;
    }

    bool partial = crew.total > (uint32_t)crew.found;
    if (partial) say_shortfall(&crew, ask);
    say_guarded(&crew, phrase);
    say_listing(&crew);

    int ended = 0, refused = 0;
    end_crew(&crew, &ended, &refused);
    closing_report(&crew, ended);

    /* A status of 0 says the whole crew of the tag is gone, so every way of
     * not being whole has to reach it: a cabin this program refused to end is
     * a refusal, and so is a cabin that never came in the answer. */
    bool whole = ended > 0 && refused == 0 && crew.guarded == 0 && !partial;

    free_crew(&crew);
    free(phrase);

    if (whole) {
        exit(0);
        return 0;
    }
    exit(1);
    return 1;
}
