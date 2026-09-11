
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
#define TAG_MAX_BYTES  63u
#define COUNT_DIGITS   12

typedef enum MateVerdict {
    VERDICT_SKIPPED = 0,
    VERDICT_CHOSEN  = 1,
    VERDICT_GUARDED = 2
} MateVerdict;

typedef struct Crew {
    ProcMate    *mates;
    MateVerdict *verdict;
    int          found;
    uint32_t     total;
    int          matched;
    int          picked;
    int          guarded;
} Crew;

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

static const char *counted(int n, char *out, size_t cap)
{
    if (n == 1) return "one";
    if (n == 2) return "two";
    if (n == 3) return "three";
    return to_str(n, out, cap);
}

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

static void say_shortfall(const Crew *crew, const char *ask)
{
    printf("%color%u of the %u wearing %s are in this answer"
           " - ask more narrowly%color\n",
           COLOR_RED, (unsigned)crew->found, (unsigned)crew->total, ask,
           COLOR_DEFAULT);
}

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

static void end_crew(const Crew *crew, int *out_ended, int *out_refused)
{
    for (int i = 0; i < crew->found; i++) {
        if (crew->verdict[i] != VERDICT_CHOSEN) continue;

        char       *pretty = spaced_tags(crew->mates[i].tags);
        const char *name   = pretty ? pretty : crew->mates[i].tags;

        int rc = proc_finish(crew->mates[i].pid, crew->mates[i].generation);
        if (rc == 0) {
            (void)process_gone(crew->mates[i].pid, crew->mates[i].generation, NULL);
            printf("ended %color%s%color\n", COLOR_CYAN, name, COLOR_DEFAULT);
            (*out_ended)++;
            free(pretty);
            continue;
        }

        error_t why = box_errno_of(rc);
        if (why == ERR_PROCESS_NOT_FOUND) {
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