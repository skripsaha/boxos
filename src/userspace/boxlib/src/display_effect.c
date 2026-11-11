/* display_effect.c — an effect over the whole glass: the questions any
 * program asks the display to lay one, and to lift it (box/display.h). */

#include "box/display.h"
#include "box/error.h"
#include "box/ipc.h"

static int effect_ask(const void *question, uint32_t length, uint8_t command, DisplayFrontReply *reply)
{
    uint32_t got = 0;
    int      rc  = ask(DISPLAY_TAG, question, length, reply, sizeof(*reply), &got);
    if (rc != OK) return rc;
    if (got != sizeof(*reply) || reply->command != command) return -ERR_CORRUPTED;
    return reply->error == OK ? OK : -reply->error;
}

int display_effect(uint32_t kinds, uint8_t strength, uint8_t axes, uint32_t color, uint32_t duration_ms)
{
    DisplayEffectRequest question = { DISP_CMD_EFFECT, strength, axes, 0, kinds, color, duration_ms };
    DisplayFrontReply    reply;
    return effect_ask(&question, sizeof(question), DISP_CMD_EFFECT, &reply);
}

int display_effect_lift(void)
{
    const uint8_t     question = DISP_CMD_EFFECT_LIFT;
    DisplayFrontReply reply;
    int               rc = effect_ask(&question, sizeof(question), DISP_CMD_EFFECT_LIFT, &reply);
    return rc == OK ? reply.heard : rc;
}
