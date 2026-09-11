
#include "box/print.h"
#include "box/luggage.h"
#include "box/system.h"
#include "box/string.h"
#include "box/memory.h"
#include "box/error.h"
#include "box/convert.h"

#define WORD_BYTES 16u
#define FIXED_WORDS 4u
#define QUOTED "\"two words\""
#define QUOTED_WORD "two words"

static size_t put_decimal(char *out, uint32_t v)
{
    char   tmp[11];
    size_t n = 0;
    do { tmp[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    for (size_t i = 0; i < n; i++) out[i] = tmp[n - 1 - i];
    return n;
}

static void generated_word(uint32_t i, char out[WORD_BYTES + 1])
{
    out[0] = 'w';
    out[1] = (char)('0' + (i / 1000) % 10);
    out[2] = (char)('0' + (i / 100) % 10);
    out[3] = (char)('0' + (i / 10) % 10);
    out[4] = (char)('0' + i % 10);
    out[5] = '-';
    for (uint32_t k = 0; k < 10; k++) out[6 + k] = (char)('a' + (i * 7 + k * 3) % 26);
    out[WORD_BYTES] = '\0';
}

static size_t write_line(char *line, uint32_t words, size_t body)
{
    size_t pos = 0;
    memcpy(line + pos, "luggagetest carry ", 18);
    pos += 18;
    pos += put_decimal(line + pos, words);
    line[pos++] = ' ';
    pos += put_decimal(line + pos, (uint32_t)body);
    line[pos++] = ' ';
    memcpy(line + pos, QUOTED, sizeof(QUOTED) - 1);
    pos += sizeof(QUOTED) - 1;
    for (uint32_t i = 0; i < words; i++) {
        char w[WORD_BYTES + 1];
        generated_word(i, w);
        line[pos++] = ' ';
        memcpy(line + pos, w, WORD_BYTES);
        pos += WORD_BYTES;
    }
    line[pos] = '\0';
    return pos;
}

static char *generated_line(uint32_t words, size_t *out_len)
{
    size_t head = 64;
    size_t len  = head + sizeof(QUOTED) + (size_t)words * (WORD_BYTES + 1) + 1;
    char  *line = malloc(len);
    if (!line) return NULL;

    size_t body = 0;
    for (int pass = 0; pass < 8; pass++) {
        size_t pos = write_line(line, words, body);
        if (pos == body) break;
        body = pos;
    }
    *out_len = strlen(line);
    return line;
}

static int child_fail(const char *what)
{
    printf("[LUGGAGE] child FAIL: %s\n", what);
    return 1;
}

static int carry(void)
{
    uint32_t count = luggage_word_count();
    if (count < FIXED_WORDS + 1) return child_fail("too few words");

    uint32_t words = to_uint(luggage_word(2));
    uint32_t bytes = to_uint(luggage_word(3));
    Luggage  l     = luggage();

    if (l.length != bytes)                        return child_fail("byte count differs from what was typed");
    if (count != FIXED_WORDS + 1 + words)         return child_fail("word count differs from what was typed");
    if (strcmp(luggage_word(0), "luggagetest"))   return child_fail("word 0 is not the program's name");
    if (strcmp(luggage_word(4), QUOTED_WORD))     return child_fail("the quoted word was not kept whole");
    if (luggage_tail(4)[0] != '"')                return child_fail("the tail does not start at the quote as typed");

    for (uint32_t i = 0; i < words; i++) {
        char w[WORD_BYTES + 1];
        generated_word(i, w);
        const char *got = luggage_word(FIXED_WORDS + 1 + i);
        if (!got || strcmp(got, w) != 0)          return child_fail("a generated word differs");
    }

    if (words > 0) {
        char last[WORD_BYTES + 1];
        generated_word(words - 1, last);
        if (strcmp(luggage_tail(count - 1), last)) return child_fail("the last tail is not the last word");
    }
    if (luggage_tail(count)[0] != '\0')           return child_fail("a tail past the end is not empty");
    if (luggage_word(count) != NULL)              return child_fail("a word past the end is not NULL");

    size_t expect_len = 0;
    char  *expect     = generated_line(words, &expect_len);
    if (!expect) return child_fail("no memory to regenerate the line");
    int same = (expect_len == l.length) && memcmp(expect, l.bytes, l.length) == 0;
    free(expect);
    if (!same) return child_fail("the line differs from what was typed");
    return 0;
}

static int send_child(uint32_t words, const char *what)
{
    size_t len  = 0;
    char  *line = generated_line(words, &len);
    if (!line) { printf("[LUGGAGE] FAIL: no memory for the %s line\n", what); return 1; }

    uint32_t gen = 0;
    int pid = proc_exec_gen(line, NULL, &gen);
    free(line);
    if (pid <= 0) {
        printf("[LUGGAGE] FAIL: could not start the %s child (error %d)\n", what,
               pid < 0 ? (int)box_errno_of(pid) : 0);
        return 1;
    }

    int32_t child_exit = 0;
    if (process_gone((uint32_t)pid, gen, &child_exit) != 0) {
        printf("[LUGGAGE] FAIL: the %s child's end could not be waited for\n", what);
        return 1;
    }
    if (child_exit != 0) {
        printf("[LUGGAGE] FAIL: the %s child (%u words, %u bytes) reported %d\n",
               what, words, (unsigned)len, (int)child_exit);
        return 1;
    }
    printf("[LUGGAGE] %s line carried: %u words, %u bytes\n", what, words, (unsigned)len);
    return 0;
}

int main(void)
{
    if (luggage_word_count() >= 2 && strcmp(luggage_word(1), "carry") == 0) {
        int rc = carry();
        exit(rc);
        return rc;
    }

    int failed = 0;
    failed += send_child(8, "short");
    failed += send_child(400, "long");

    if (failed == 0) println("[LUGGAGE] PASS");
    exit(failed ? 1 : 0);
    return failed ? 1 : 0;
}