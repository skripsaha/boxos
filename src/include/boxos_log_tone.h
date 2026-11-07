#ifndef BOXOS_LOG_TONE_H
#define BOXOS_LOG_TONE_H

/*
 * boxos_log_tone.h — Tone: what kind of text a byte of the log ring is,
 * shared kernel/userspace.
 *
 * The ring keeps one tone beside every byte, at the same position. The kernel
 * says WHAT the text is — an error, a success — and never which colour it is:
 * colour belongs to whoever draws (the display program by its theme, the
 * Emergency Screen by its own table). Serial and `logsave` carry the bytes
 * alone, exactly as said.
 *
 * kprintf sets a tone with %[E] %[S] %[W] %[H] and returns to PLAIN with %[D];
 * every kprintf begins PLAIN, so a line says its own tone and never inherits
 * the last one's. Text a program says into the ring (hw.log.say) is PLAIN.
 */

#define LOG_TONE_PLAIN    0u
#define LOG_TONE_ERROR    1u
#define LOG_TONE_SUCCESS  2u
#define LOG_TONE_WARNING  3u
#define LOG_TONE_HINT     4u
#define LOG_TONE_COUNT    5u

#endif /* BOXOS_LOG_TONE_H */
