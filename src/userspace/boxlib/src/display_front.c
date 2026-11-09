/* display_front.c — the program in front of the person, and the ones behind:
 * the questions a keeper asks the display (box/display.h). */

#include "box/display.h"
#include "box/error.h"
#include "box/ipc.h"
#include "box/string.h"

/* One question about the program (pid, generation): OK with *reply, or the
 * ask's refusal, or -ERR_CORRUPTED for an answer of another shape. */
static int front_ask(uint8_t command, uint32_t pid, uint32_t generation, DisplayFrontReply *reply)
{
    DisplayFrontRequest question = { command, { 0, 0, 0 }, pid, generation };
    uint32_t            got      = 0;
    int rc = ask(DISPLAY_TAG, &question, sizeof(question), reply, sizeof(*reply), &got);
    if (rc != OK) return rc;
    if (got != sizeof(*reply) || reply->command != command) return -ERR_CORRUPTED;
    return OK;
}

static int front_say(uint8_t command, uint32_t pid, uint32_t generation, uint32_t *display)
{
    DisplayFrontReply reply;
    int               rc = front_ask(command, pid, generation, &reply);
    if (rc != OK) return rc;
    if (display) *display = reply.display_pid;
    return reply.error == OK ? OK : -reply.error;
}

int display_front(uint32_t pid, uint32_t generation, uint32_t *display)
{
    return front_say(DISP_CMD_FRONT, pid, generation, display);
}

int display_back(uint32_t pid, uint32_t generation, uint32_t *display)
{
    return front_say(DISP_CMD_BACK, pid, generation, display);
}

int display_ask_end(uint32_t pid, uint32_t generation)
{
    DisplayFrontReply reply;
    int               rc = front_ask(DISP_CMD_ASK_END, pid, generation, &reply);
    if (rc != OK) return rc;
    if (reply.error != OK) return -reply.error;
    return reply.heard ? 1 : 0;
}

uint8_t console_notice_of(const void *payload, uint32_t length)
{
    ConsoleNotice notice;
    if (length != sizeof(notice)) return 0;
    memcpy(&notice, payload, sizeof(notice));
    return notice.mark == CONSOLE_NOTICE_MARK ? notice.kind : 0;
}
