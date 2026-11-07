#ifndef BOX_SCREEN_H
#define BOX_SCREEN_H

#ifdef __cplusplus
extern "C" {
#endif

#include "box/types.h"
#include "boxos_screen.h"
#include "boxos_log_tone.h"

/*
 * The screen and the log ring that feeds it.
 *
 * The kernel lends the screen to one system program — the display — and keeps
 * one account of everything the machine says, the log ring. Every program's
 * text reaches the glass through the display, which draws the ring's lines in
 * their order beside its own; a program with no display says its text into
 * the ring itself (log_say) and the kernel's Emergency Screen shows it.
 */

/* The glass, mapped into this cabin (SYSTEM only). -ERR_BUSY: the program
 * that holds it is ending — wait for the `screen` Touch event
 * SCREEN_EVENT_FREE. -ERR_ACCESS_DENIED: a living program holds it. */
int screen_claim(ScreenDescription *out);
int screen_release(void);

/* A run of text into the ring, whole: [*start, *end) is where it went (either
 * may be NULL). 1..LOG_SAY_MAX bytes. */
#define LOG_SAY_MAX 4096u
int log_say(const char *bytes, uint32_t len, uint64_t *start, uint64_t *end);

/* The screen holder's bell: ring me at the end of the next utterance past
 * `from`. *news is set when one is there already — nothing was hung, and the
 * caller must not sleep. */
int log_bell(uint64_t from, bool *news);

/* The ring from `from`: at most `max` bytes into `bytes` and the Tone of each
 * into `tones`. Returns how many were copied, or -ERR_*. *oldest is where the
 * ring begins now (a reader the writers overtook learns its gap), *written
 * where it ends. */
int log_read_tones(uint64_t from, char *bytes, uint8_t *tones, uint32_t max,
                   uint64_t *oldest, uint64_t *written);

#ifdef __cplusplus
}
#endif

#endif /* BOX_SCREEN_H */
