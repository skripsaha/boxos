/*
 * form_souvenir.c — files a program gives back to the one who started it.
 *
 * A souvenir is one IPC message to the spawner: a magic word, a count, then
 * that many file ids, each eight bytes. Many files go in several messages,
 * each as large as a message can be. The kernel queues a message before the
 * sender's end is seen, so a shell that has waited for the program to be gone
 * finds every souvenir already pending and reads them without waiting.
 */

#include "box/form.h"
#include "box/core/cabin.h"
#include "box/core/result.h"
#include "box/error.h"
#include "box/ipc.h"
#include "box/memory.h"
#include "box/string.h"

#define SOUVENIR_MAGIC  0x56554F53u
#define SOUVENIR_HEAD   8u
#define SOUVENIR_FIRST  ((65535u - SOUVENIR_HEAD) / sizeof(file_id_t))

int souvenir_things(const file_id_t *ids, size_t count)
{
    if (count > 0 && !ids) return -ERR_INVALID_ARGUMENT;
    uint32_t spawner = cabin_info()->spawner_pid;
    if (spawner == 0 || count == 0) return 0;

    size_t   most = count < SOUVENIR_FIRST ? count : SOUVENIR_FIRST;
    uint8_t *message = malloc(SOUVENIR_HEAD + most * sizeof(file_id_t));
    if (!message) return -ERR_NO_MEMORY;

    int rc = 0;
    for (size_t done = 0; done < count && rc == 0; done += most) {
        uint32_t magic = SOUVENIR_MAGIC;
        uint32_t here  = (uint32_t)(count - done < most ? count - done : most);

        memcpy(message, &magic, sizeof(magic));
        memcpy(message + 4, &here, sizeof(here));
        memcpy(message + SOUVENIR_HEAD, ids + done, here * sizeof(file_id_t));
        rc = send(spawner, message, (uint16_t)(SOUVENIR_HEAD + here * sizeof(file_id_t)));
        if (rc < 0 && box_errno_of(rc) == ERR_PROCESS_NOT_FOUND) rc = 0;
    }
    free(message);
    return rc < 0 ? rc : 0;
}

/* ---- the shell's side ------------------------------------------------- */

typedef struct {
    Result *Held;
    size_t  Count;
    size_t  Capacity;
} SouvenirHold;

static bool is_souvenir(const Result *message, uint32_t from_pid, uint32_t *count)
{
    uint32_t magic, here;

    if (message->sender_pid != from_pid || message->data_addr == 0 ||
        message->data_length < SOUVENIR_HEAD)
        return false;
    const uint8_t *data = (const uint8_t *)(uintptr_t)message->data_addr;
    memcpy(&magic, data, sizeof(magic));
    memcpy(&here, data + 4, sizeof(here));
    if (magic != SOUVENIR_MAGIC ||
        (message->data_length - SOUVENIR_HEAD) / sizeof(file_id_t) != here ||
        (message->data_length - SOUVENIR_HEAD) % sizeof(file_id_t) != 0)
        return false;
    *count = here;
    return true;
}

static int hold_add(SouvenirHold *hold, const Result *message)
{
    if (hold->Count == hold->Capacity) {
        size_t  grown = hold->Capacity ? hold->Capacity * 2 : 16;
        Result *bigger = realloc(hold->Held, grown * sizeof(Result));
        if (!bigger) return -ERR_NO_MEMORY;
        hold->Held     = bigger;
        hold->Capacity = grown;
    }
    hold->Held[hold->Count++] = *message;
    return 0;
}

static void hold_give_back(SouvenirHold *hold, const uint8_t *taken)
{
    for (size_t i = 0; i < hold->Count; i++) {
        if (!taken || !taken[i]) result_restash(&hold->Held[i]);
    }
    free(hold->Held);
    hold->Held = NULL;
    hold->Count = hold->Capacity = 0;
}

static int hold_everything(SouvenirHold *hold)
{
    Result message;

    while (receive(&message)) {
        int rc = hold_add(hold, &message);
        if (rc != 0) {
            result_restash(&message);
            hold_give_back(hold, NULL);
            return rc;
        }
    }
    return 0;
}

int souvenir_collect(uint32_t from_pid, file_id_t **ids, size_t *count)
{
    if (!ids || !count) return -ERR_INVALID_ARGUMENT;
    *ids   = NULL;
    *count = 0;

    SouvenirHold hold = { NULL, 0, 0 };
    int rc = hold_everything(&hold);
    if (rc != 0) return rc;

    size_t   total = 0;
    uint32_t here;
    uint8_t *taken = hold.Count ? calloc(hold.Count, 1) : NULL;
    if (hold.Count && !taken) {
        hold_give_back(&hold, NULL);
        return -ERR_NO_MEMORY;
    }
    for (size_t i = 0; i < hold.Count; i++) {
        if (!is_souvenir(&hold.Held[i], from_pid, &here)) continue;
        taken[i] = 1;
        total += here;
    }

    file_id_t *all = total ? malloc(total * sizeof(file_id_t)) : NULL;
    if (total && !all) {
        hold_give_back(&hold, NULL);
        free(taken);
        return -ERR_NO_MEMORY;
    }
    size_t at = 0;
    for (size_t i = 0; i < hold.Count; i++) {
        if (!taken[i]) continue;
        is_souvenir(&hold.Held[i], from_pid, &here);
        memcpy(all + at, (const uint8_t *)(uintptr_t)hold.Held[i].data_addr + SOUVENIR_HEAD,
               here * sizeof(file_id_t));
        at += here;
    }
    hold_give_back(&hold, taken);
    free(taken);
    *ids   = all;
    *count = total;
    return 0;
}
