/*
 * headcount — every carrier of a tag is told once, and every one answers.
 *
 * The oracle of system.broadcast. The parent claims the answer tag, starts
 * CREW children that carry the crew tag from boarding (a spawn tag), says one
 * word to that tag — once — and counts the answers. A child reads the one
 * message it is sent, answers on the answer tag and leaves. The answer is a
 * Touch: a full ring owes it, it is never lost, so the count is exact.
 *
 * Every child must answer. broadcast used to take the carriers of a tag into
 * a stack tray of 256 and stop when the tray was full, so the 257th carrier
 * was never told, and nothing said so. CREW is more than that tray was.
 *
 *   headcount            the parent — [HEADCOUNT] PASS / FAIL
 *   headcount answer     a child, started by the parent, never by hand
 */

#include "box/print.h"
#include "box/ipc.h"
#include "box/touch.h"
#include "box/system.h"
#include "box/luggage.h"
#include "box/string.h"
#include "box/error.h"

#define CREW       300
#define CREW_TAG   "headcount:crew"
#define ANSWER_TAG "headcount:answer"

static int answer(void)
{
    Result word;
    if (!receive_wait(&word, 0)) return 1;   /* no deadline: the word is coming */
    return touch_send(TOUCH_TAG_PAIR(ANSWER_TAG), NULL, 0, 0) < 0 ? 1 : 0;
}

int main(void)
{
    const char *role = luggage_word(1);
    if (role && strcmp(role, "answer") == 0) return answer();

    TouchTag answers = touch_pair_choose(touch_intern(ANSWER_TAG));
    if (answers == TOUCH_TAG_INVALID || touch_claim(answers, TOUCH_REST, 0, 0) != OK) {
        printf("[HEADCOUNT] FAIL: cannot claim %s\n", ANSWER_TAG);
        return 1;
    }

    printf("[HEADCOUNT] boarding %d\n", CREW);
    for (int i = 0; i < CREW; i++) {
        int pid = proc_exec_tagged("headcount answer", CREW_TAG);
        if (pid < 0) {
            printf("[HEADCOUNT] FAIL: child %d refused (%d)\n", i, pid);
            return 1;
        }
    }

    uint8_t word = 1;
    int rc = broadcast(CREW_TAG, &word, sizeof(word));
    if (rc != 0) {
        printf("[HEADCOUNT] FAIL: broadcast said %d\n", rc);
        return 1;
    }

    uint32_t heard = 0;
    while (heard < CREW) {
        Touch t;
        if (!touch_wait_tag(answers, &t, 30000)) {
            printf("[HEADCOUNT] FAIL: one word to %d, %u answered\n", CREW, heard);
            return 1;
        }
        heard++;
    }
    printf("[HEADCOUNT] PASS: one word to %d, %d answered\n", CREW, CREW);
    return 0;
}
