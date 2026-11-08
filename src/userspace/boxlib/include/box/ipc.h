#ifndef BOX_IPC_H
#define BOX_IPC_H

#ifdef __cplusplus
extern "C" {
#endif

#include "box/types.h"
#include "box/error.h"
#include "box/core/result.h"

int send(uint32_t target_pid, const void* data, uint16_t size);
int broadcast(const char* tag, const void* data, uint16_t size);

/* Ask the one strand carrying `tag` (system.ask) and stand until it answers.
 * The kernel gives exactly one answer: the carrier's, as this call's own reply
 * — up to `cap` bytes into `answer`, *answer_len of them — or, if the carrier's
 * life ends first, -ERR_PROCESS_NOT_FOUND in its place. No clock, no watching
 * for deaths, nothing else this strand receives meanwhile is touched.
 *   -ERR_ROUTE_NO_SUBSCRIBERS  nobody carries the tag
 *   -ERR_ROUTE_AMBIGUOUS       more than one strand carries it
 *   -ERR_BUFFER_TOO_SMALL      the answer was larger than `cap` */
int ask(const char *tag, const void *question, uint32_t len,
        void *answer, uint32_t cap, uint32_t *answer_len);

/* A question arrives through receive() as a Result of kind KCTX_ASK
 * (boxos_kctx.h): sender_pid is the asker, and its payload is AskFrom — who
 * asked and in which program, the kernel's word — followed by the question.
 * Answer it once with this; the asker gets `bytes` as its reply.
 *   -ERR_INVALID_STATE      the question is not this strand's to answer, or is
 *                           answered already
 *   -ERR_PROCESS_NOT_FOUND  the asker is dead: the answer went nowhere, and
 *                           what it handed out was handed to nobody */
int answer(const Result *question, const void *bytes, uint32_t len);

bool receive(Result* out);
bool receive_wait(Result* out, uint32_t timeout_ms);
/* A program's arguments are not a message: they are its Luggage, in its cabin
 * from the first instruction — see box/luggage.h. */


#ifdef __cplusplus
}
#endif

#endif // IPC_H
