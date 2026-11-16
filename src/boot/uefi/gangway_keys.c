/*
 * Gangway — the keyboard, as the firmware's console has it (ConIn, UEFI
 * §12.3): whether the person holds the key that asks for the panel
 * (KeyPanelAsked); and, in the panel, the next key, waited for on the
 * console's own event — no polling (KeyNext).
 *
 * A key held down is a key the keyboard repeats — a PS/2 keyboard itself, a
 * USB one through the firmware's driver — so while the person holds Space a
 * stroke of it comes into the console's queue once a repeat. What the
 * firmware's start left there is not Gangway's to count on: a firmware may
 * take the strokes up before it hands over. So the keyboard is listened to
 * from Gangway's first moment (KeyListen) for one repeat at the least, and
 * the person holding Space is heard however quickly Gangway reaches the
 * question — the time it spends reading the kernel counts toward the
 * listening, and only what is left of it is waited.
 */

#include "gangway.h"

/* More strokes than a queue holds: the reading ends even on a console that
 * never says it is empty. */
#define STROKES_MOST 256u

/* How long a held key is listened for: one repeat of a PS/2 keyboard at its
 * rate after a reset, 10.9 a second — 92 ms — with room; a USB keyboard is
 * repeated every 31 ms by the firmware's driver (EDK2 UsbKbDxe). */
#define KEY_LISTEN_MS 100u

/* The listening's end, set at Gangway's first moment; NULL when the firmware
 * gave no timer, and the queue is read as it stands. */
static EFI_EVENT g_listen;

void KeyListen(void)
{
    if (EFI_ERROR(g_bs->create_event(EVT_TIMER, 0, NULL, NULL, &g_listen))) {
        g_listen = NULL;
        return;
    }
    if (EFI_ERROR(g_bs->set_timer(g_listen, TimerRelative, (uint64_t)KEY_LISTEN_MS * 10000u))) {
        (void)g_bs->close_event(g_listen);
        g_listen = NULL;
    }
}

/* Every stroke waiting read: whether one was Space. */
static int SpaceWaiting(EFI_SIMPLE_TEXT_INPUT_PROTOCOL *in)
{
    EFI_INPUT_KEY key;
    int           space = 0;
    for (uint32_t n = 0; n < STROKES_MOST && !EFI_ERROR(in->read_key_stroke(in, &key)); n++)
        if (key.scan_code == 0 && key.unicode_char == (CHAR16)' ') space = 1;
    return space;
}

int KeyPanelAsked(int listen)
{
    EFI_SIMPLE_TEXT_INPUT_PROTOCOL *in    = g_st->con_in;
    int                             asked = in && SpaceWaiting(in);

    while (in && !asked && listen && g_listen) {
        EFI_EVENT heard[2] = { in->wait_for_key, g_listen };
        UINTN     index    = 0;
        if (EFI_ERROR(g_bs->wait_for_event(2, heard, &index)) || index == 1) break;
        asked = SpaceWaiting(in);
    }
    if (g_listen) (void)g_bs->close_event(g_listen);
    g_listen = NULL;
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
