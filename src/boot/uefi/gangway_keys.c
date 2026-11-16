/*
 * Gangway — the keyboard, as the firmware's console has it (ConIn, UEFI
 * §12.3): whether the person holds the key that asks for the panel, asked
 * once and never waited on (KeyPanelAsked); and, in the panel, the next key,
 * waited for on the console's own event — no polling, no timer (KeyNext).
 *
 * A key held down is a key the firmware's keyboard driver repeats, so while
 * the person holds Space there are strokes of it in the console's queue
 * whenever it is asked. Every stroke there is gets read; the others are keys
 * pressed while the firmware started, and go with the reading.
 */

#include "gangway.h"

/* More strokes than a queue holds: the reading ends even on a console that
 * never says it is empty. */
#define STROKES_MOST 256u

int KeyPanelAsked(void)
{
    EFI_SIMPLE_TEXT_INPUT_PROTOCOL *in = g_st->con_in;
    EFI_INPUT_KEY                   key;
    int                             asked = 0;

    if (!in) return 0;
    for (uint32_t n = 0; n < STROKES_MOST && !EFI_ERROR(in->read_key_stroke(in, &key)); n++)
        if (key.scan_code == 0 && key.unicode_char == (CHAR16)' ') asked = 1;
    return asked;
}

/* What a stroke is to the panel; KEY_NONE for one it does not use. */
static GangwayKey KeyOf(const EFI_INPUT_KEY *key)
{
    switch (key->scan_code) {
    case EFI_SCAN_UP:        return KEY_UP;
    case EFI_SCAN_DOWN:      return KEY_DOWN;
    case EFI_SCAN_LEFT:      return KEY_LEFT;
    case EFI_SCAN_RIGHT:     return KEY_RIGHT;
    case EFI_SCAN_HOME:      return KEY_HOME;
    case EFI_SCAN_END:       return KEY_END;
    case EFI_SCAN_PAGE_UP:   return KEY_PAGE_UP;
    case EFI_SCAN_PAGE_DOWN: return KEY_PAGE_DOWN;
    case EFI_SCAN_ESC:       return KEY_BACK;
    default:                 break;
    }
    if (key->scan_code) return KEY_NONE;
    if (key->unicode_char == (CHAR16)'\r' || key->unicode_char == (CHAR16)'\n') return KEY_ENTER;
    if (key->unicode_char == (CHAR16)'\b') return KEY_BACK;
    return KEY_NONE;
}

GangwayKey KeyNext(void)
{
    EFI_SIMPLE_TEXT_INPUT_PROTOCOL *in = g_st->con_in;

    for (;;) {
        EFI_INPUT_KEY key;
        UINTN         index;
        EFI_STATUS    status = in->read_key_stroke(in, &key);
        if (status == EFI_NOT_READY) {
            if (EFI_ERROR(g_bs->wait_for_event(1, &in->wait_for_key, &index))) return KEY_GONE;
            continue;
        }
        if (EFI_ERROR(status)) return KEY_GONE;
        GangwayKey k = KeyOf(&key);
        if (k != KEY_NONE) return k;
    }
}
