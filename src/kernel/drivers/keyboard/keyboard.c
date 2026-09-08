#include "keyboard.h"
#include "keymaps.h"
#include "klib.h"
#include "io.h"
#include "atomics.h"
#include "cpu_calibrate.h"
#include "irqchip.h"
#include "scheduler.h"   /* g_global_tick, g_timer_frequency */
#include "touch.h"
#include "logbook.h"
#include "kb_event.h"    /* kb_event_t + KB_MOD_* — shared keyboard ABI */

/* Runtime repeat timing (calculated from ms based on timer frequency) */
uint32_t g_kb_repeat_delay_ticks;
uint32_t g_kb_repeat_rate_ticks;

/* ─── Circular character buffer ─────────────────────────────────────────── */


/* ─── Keyboard state ────────────────────────────────────────────────────── */

static keyboard_state_t kb_state = {0};


/* ─── Touch tag handle cache (resolved once at keyboard_init) ──────────────
 *
 * keyboard_handle_scancode and keyboard_timer_tick both run in IRQ context
 * (PS/2 IRQ1 dispatch + PIT IRQ0 tick). TouchLogbookResolve is NOT IRQ-safe —
 * it takes the TagFS registry lock and may intern a fresh tag, which can
 * kmalloc. We therefore resolve the "keyboard" tag once during driver
 * initialization (non-IRQ context, TagFS registry already up) and cache
 * the resulting handles here. IRQ-side code does an atomic load + passes
 * the handles to TouchPublishIrqPair.
 *
 * Until keyboard_init has run, both handles read as TOUCH_TAG_INVALID and
 * TouchPublishIrqPair becomes a no-op — early-boot IRQ fires (if any) are
 * silently discarded rather than touching the registry from IRQ context. */
static volatile uint16_t g_kbd_touch_full = TOUCH_TAG_INVALID;
static volatile uint16_t g_kbd_touch_bare = TOUCH_TAG_INVALID;

/* ─── Extended scancode (0xE0) state ────────────────────────────────────── */

static volatile uint8_t kb_e0_pending = 0;

/* ─── LED state ─────────────────────────────────────────────────────────── */

static uint8_t kb_led_state = 0;

/* ─── Publishing a key ──────────────────────────────────────────────────── */

static uint8_t kb_mods_now(void)
{
    return (uint8_t)(
        (__atomic_load_n(&kb_state.shift_pressed, __ATOMIC_RELAXED) ? KB_MOD_SHIFT : 0) |
        (__atomic_load_n(&kb_state.ctrl_pressed,  __ATOMIC_RELAXED) ? KB_MOD_CTRL  : 0) |
        (__atomic_load_n(&kb_state.alt_pressed,   __ATOMIC_RELAXED) ? KB_MOD_ALT   : 0));
}

/* Every key the machine hears leaves here as one Touch on the "keyboard"
 * tag. IRQ context (PS/2 IRQ1, xHCI HID, PIT IRQ0 for repeats): the publish
 * is deferred through the static ring + irq_defer, because TouchPublish
 * would take the TagFS registry lock and per-bucket spinlocks with IF=0 —
 * the deadlock pattern irq_defer exists to break. */
static void kb_publish_event(const kb_event_t *ev)
{
    TouchTag full = __atomic_load_n(&g_kbd_touch_full, __ATOMIC_ACQUIRE);
    TouchTag bare = __atomic_load_n(&g_kbd_touch_bare, __ATOMIC_ACQUIRE);
    TouchPublishIrqPair(full, bare, ev, sizeof(*ev), 0, TOUCH_FLAG_KERNEL);
}

/* ─── Software key repeat state ─────────────────────────────────────────── */

typedef struct {
    uint8_t  active;                  /* 1 = a key is held down          */
    uint8_t  held_key;               /* bare scancode of held key        */
    uint8_t  held_is_extended;       /* 1 if the held key was 0xE0-prefixed */
    kb_event_t held_event;           /* the Touch a repeat says again    */
    uint64_t next_repeat_tick;       /* g_global_tick when next repeat fires */
    uint8_t  delay_passed;           /* 1 after initial delay elapsed    */
} KbRepeatState;

static KbRepeatState kb_repeat = {0};


/* ═══════════════════════════════════════════════════════════════════════════
   Internal helpers
   ═══════════════════════════════════════════════════════════════════════════ */

/* Public: feed characters from an external input source (the COM1 serial
 * console — see serial_console_init) as "keyboard" Touch events, so a real
 * keypress and an injected byte are indistinguishable to whoever listens.
 * Deferred (IRQ-safe) publish is mandatory: keyboard_inject runs in the COM1
 * IRQ handler, same as the PS/2 site — a direct TouchPublish would take TagFS
 * registry locks with IF=0 (the irq_defer deadlock pattern). scancode 0 says
 * the key is synthetic. */
void keyboard_inject(const char *chars, uint32_t count)
{
    for (uint32_t i = 0; i < count; i++) {
        kb_event_t kb_ev = { .scancode = 0, .ascii = chars[i], .mods = 0 };
        kb_publish_event(&kb_ev);
    }
}

/* Translate a bare scancode to ASCII with correct CapsLock+Shift behaviour.
   For letters: CapsLock XOR Shift → uppercase.
   For symbols: only Shift matters; CapsLock is ignored. */
static char translate_key(uint8_t key)
{
    if (key >= sizeof(scancode_to_ascii)) return 0;

    char base    = scancode_to_ascii[key];
    char shifted = scancode_to_ascii_shifted[key];
    int  is_alpha = (base >= 'a' && base <= 'z');

    uint8_t shift = __atomic_load_n(&kb_state.shift_pressed, __ATOMIC_RELAXED);
    if (is_alpha) {
        uint8_t caps = __atomic_load_n(&kb_state.caps_lock, __ATOMIC_RELAXED);
        /* XOR: exactly one of CapsLock/Shift active → uppercase */
        return (caps ^ shift) ? shifted : base;
    }

    return shift ? shifted : base;
}

/* Arm the software repeat timer for the given key. */
static void kb_arm_repeat(uint8_t key, uint8_t is_extended,
                          const kb_event_t *event)
{
    kb_repeat.active         = 1;
    kb_repeat.held_key       = key;
    kb_repeat.held_is_extended = is_extended;
    kb_repeat.held_event     = *event;
    kb_repeat.delay_passed   = 0;
    kb_repeat.next_repeat_tick = __atomic_load_n(&g_global_tick, __ATOMIC_RELAXED)
                                 + g_kb_repeat_delay_ticks;
}

/* Cancel repeat if the released key matches the currently repeating key. */
static void kb_release_repeat(uint8_t key, uint8_t is_extended)
{
    if (kb_repeat.active &&
        kb_repeat.held_key == key &&
        kb_repeat.held_is_extended == is_extended)
    {
        kb_repeat.active = 0;
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
   8042 PS/2 controller helpers
   ═══════════════════════════════════════════════════════════════════════════ */

static void kb_wait_input_buffer(void)
{
    uint64_t deadline = rdtsc() + cpu_ms_to_tsc(50);
    while ((inb(KEYBOARD_STATUS_PORT) & 0x02) && rdtsc() < deadline) {
        __asm__ volatile("pause");
    }
}

static bool kb_wait_output_buffer(void)
{
    uint64_t deadline = rdtsc() + cpu_ms_to_tsc(50);
    while (!(inb(KEYBOARD_STATUS_PORT) & 0x01) && rdtsc() < deadline) {
        __asm__ volatile("pause");
    }
    return (inb(KEYBOARD_STATUS_PORT) & 0x01) != 0;
}

static void kb_flush_output(void)
{
    uint32_t timeout = 16;
    while ((inb(KEYBOARD_STATUS_PORT) & 0x01) && timeout--) {
        inb(KEYBOARD_DATA_PORT);
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
   Public API
   ═══════════════════════════════════════════════════════════════════════════ */

void keyboard_set_leds(uint8_t caps, uint8_t num, uint8_t scroll)
{
    uint8_t bits = (scroll ? 0x01 : 0) | (num ? 0x02 : 0) | (caps ? 0x04 : 0);
    if (bits == kb_led_state) return;
    kb_led_state = bits;

    kb_wait_input_buffer();
    outb(KEYBOARD_DATA_PORT, 0xED);
    kb_wait_input_buffer();
    outb(KEYBOARD_DATA_PORT, bits);
}

void keyboard_init(void)
{
    memset(&kb_state, 0, sizeof(kb_state));
    memset(&kb_repeat, 0, sizeof(kb_repeat));
    kb_e0_pending = 0;
    kb_led_state  = 0;

    debug_printf("[KEYBOARD] Initializing 8042 PS/2 controller...\n");

    kb_wait_input_buffer();
    outb(KEYBOARD_STATUS_PORT, 0xAD);   /* Disable keyboard */

    kb_flush_output();

    kb_wait_input_buffer();
    outb(KEYBOARD_STATUS_PORT, 0x20);   /* Read CCB */

    if (!kb_wait_output_buffer()) {
        debug_printf("[KEYBOARD] ERROR: Timeout reading CCB\n");
        return;
    }

    uint8_t ccb = inb(KEYBOARD_DATA_PORT);
    ccb |= 0x01;    /* Enable keyboard interrupt (IRQ1) */
    ccb &= ~0x10;   /* Clear "keyboard disabled" flag   */

    kb_wait_input_buffer();
    outb(KEYBOARD_STATUS_PORT, 0x60);   /* Write CCB */
    kb_wait_input_buffer();
    outb(KEYBOARD_DATA_PORT, ccb);

    kb_wait_input_buffer();
    outb(KEYBOARD_STATUS_PORT, 0xAE);   /* Re-enable keyboard port */

    kb_flush_output();

    irqchip_enable_irq(1);
    debug_printf("[KEYBOARD] IRQ1 enabled\n");

    kb_wait_input_buffer();
    outb(KEYBOARD_DATA_PORT, 0xFF);     /* Reset command */

    /* Wait ~50 ms for ACK (0xFA) and self-test result (0xAA) */
    {
        uint64_t deadline = rdtsc() + cpu_ms_to_tsc(50);
        while (rdtsc() < deadline) {
            __asm__ volatile("pause");
        }
    }

    /* Ensure all LEDs start off */
    keyboard_set_leds(0, 0, 0);

    /* Calculate repeat timing based on actual timer frequency */
    g_kb_repeat_delay_ticks = (KB_REPEAT_DELAY_MS * g_timer_frequency) / 1000;
    g_kb_repeat_rate_ticks  = (KB_REPEAT_RATE_MS * g_timer_frequency) / 1000;

    debug_printf("[KEYBOARD] 8042 initialization complete\n");
    debug_printf("[KEYBOARD] Repeat: delay=%u ticks (%u ms), rate=%u ticks (%u ms)\n",
                 g_kb_repeat_delay_ticks, KB_REPEAT_DELAY_MS,
                 g_kb_repeat_rate_ticks, KB_REPEAT_RATE_MS);

    /* Resolve and cache the "keyboard" Touch tag while we are still in
     * non-IRQ context. TouchInit ran inside guide_init earlier in the
     * boot sequence; the TagFS registry is fully online by the time
     * keyboard_init is called.
     *
     * The "keyboard" tag is a bare key (no value), so TouchLogbookResolve
     * fills only `bare`; `full` stays TOUCH_TAG_INVALID. That is still
     * correct for TouchPublishIrqPair which publishes to whichever id
     * is non-invalid. */
    TouchTag full = TOUCH_TAG_INVALID, bare = TOUCH_TAG_INVALID;
    TouchLogbookResolve("keyboard", &full, &bare);
    __atomic_store_n(&g_kbd_touch_full, full, __ATOMIC_RELEASE);
    __atomic_store_n(&g_kbd_touch_bare, bare, __ATOMIC_RELEASE);
    debug_printf("[KEYBOARD] Touch tag handles: full=%u bare=%u\n",
                 (unsigned)full, (unsigned)bare);
}

/* ─── Scancode handler (called from IRQ1) ──────────────────────────────── */

void keyboard_handle_scancode(uint8_t scancode)
{
    /* ── 0xE0 prefix: mark and wait for next byte ── */
    if (scancode == 0xE0) {
        kb_e0_pending = 1;
        return;
    }

    /* ── Filter controller response codes ── */
    switch (scancode) {
        case 0xFA: return;   /* ACK           */
        case 0xFE: return;   /* Resend        */
        case 0x00: return;   /* Error/overrun */
        case 0xFF: return;   /* Error         */
        case 0xEE: return;   /* Echo response */
    }

    uint8_t is_release = scancode & 0x80;
    uint8_t key = scancode & 0x7F;

    /* ══════════════════════════════════════════════
       Extended key (0xE0 prefix was received)
       ══════════════════════════════════════════════ */
    if (kb_e0_pending) {
        kb_e0_pending = 0;

        /* Extended modifiers */
        if (key == 0x1D) {   /* Right Ctrl */
            __atomic_store_n(&kb_state.ctrl_pressed, !is_release, __ATOMIC_RELAXED);
            return;
        }
        if (key == 0x38) {   /* Right Alt */
            __atomic_store_n(&kb_state.alt_pressed, !is_release, __ATOMIC_RELAXED);
            return;
        }

        if (is_release) {
            kb_release_repeat(key, 1);
            return;
        }

        /* An extended key is a KEY, not a character: it is published as
         * its scancode with ascii 0 and KB_MOD_EXTENDED, and an editor
         * answers it by name (arrows, Home/End, Delete). */
        kb_event_t kb_ev = { .scancode = key, .ascii = 0,
                             .mods = (uint8_t)(kb_mods_now() | KB_MOD_EXTENDED) };
        kb_arm_repeat(key, 1, &kb_ev);
        kb_publish_event(&kb_ev);
        return;
    }

    /* ══════════════════════════════════════════════
       Normal (non-extended) key
       ══════════════════════════════════════════════ */

    /* Modifiers — track press/release, no character output */
    if (key == 0x2A || key == 0x36) {      /* Left/Right Shift */
        __atomic_store_n(&kb_state.shift_pressed, !is_release, __ATOMIC_RELAXED);
        return;
    }
    if (key == 0x1D) {                     /* Left Ctrl */
        __atomic_store_n(&kb_state.ctrl_pressed, !is_release, __ATOMIC_RELAXED);
        return;
    }
    if (key == 0x38) {                     /* Left Alt */
        __atomic_store_n(&kb_state.alt_pressed, !is_release, __ATOMIC_RELAXED);
        return;
    }

    /* Toggle keys — act on press only. Toggle is a load+xor+store (3 ops);
     * since IRQ1 is the sole writer (BSP) and toggles arrive serialised by
     * the IRQ disable in spinlock paths used by readers, the load-then-store
     * pattern is safe. Snapshot the post-toggle vector for keyboard_set_leds. */
    if (key == 0x3A && !is_release) {      /* Caps Lock */
        uint8_t caps = !__atomic_load_n(&kb_state.caps_lock, __ATOMIC_RELAXED);
        __atomic_store_n(&kb_state.caps_lock, caps, __ATOMIC_RELAXED);
        keyboard_set_leds(caps,
                          __atomic_load_n(&kb_state.num_lock,    __ATOMIC_RELAXED),
                          __atomic_load_n(&kb_state.scroll_lock, __ATOMIC_RELAXED));
        return;
    }
    if (key == 0x45 && !is_release) {      /* Num Lock */
        uint8_t num = !__atomic_load_n(&kb_state.num_lock, __ATOMIC_RELAXED);
        __atomic_store_n(&kb_state.num_lock, num, __ATOMIC_RELAXED);
        keyboard_set_leds(__atomic_load_n(&kb_state.caps_lock,   __ATOMIC_RELAXED),
                          num,
                          __atomic_load_n(&kb_state.scroll_lock, __ATOMIC_RELAXED));
        return;
    }
    if (key == 0x46 && !is_release) {      /* Scroll Lock */
        uint8_t scroll = !__atomic_load_n(&kb_state.scroll_lock, __ATOMIC_RELAXED);
        __atomic_store_n(&kb_state.scroll_lock, scroll, __ATOMIC_RELAXED);
        keyboard_set_leds(__atomic_load_n(&kb_state.caps_lock, __ATOMIC_RELAXED),
                          __atomic_load_n(&kb_state.num_lock,  __ATOMIC_RELAXED),
                          scroll);
        return;
    }

    /* Key release — cancel repeat if it matches */
    if (is_release) {
        kb_release_repeat(key, 0);
        return;
    }

    /* Translate scancode → ASCII */
    __atomic_store_n(&kb_state.last_keycode, key, __ATOMIC_RELAXED);
    char ascii = translate_key(key);

    if (ascii != 0) {
        /* (Make-debounce was tried here but broke rapid-type input —
         * `qemu-input.sh type "memtest"` lost a 't' when break-t1
         * was delayed past make-t2. Real fix for Bochs host-typematic
         * passthrough needs time-based debounce or per-environment
         * policy, not unconditional same-key suppression.) */
        kb_event_t kb_ev = { .scancode = key, .ascii = ascii, .mods = kb_mods_now() };
        kb_arm_repeat(key, 0, &kb_ev);
        kb_publish_event(&kb_ev);
    }
}

/* ─── Software key repeat (called from PIT IRQ0 every tick) ────────────── */

void keyboard_timer_tick(void)
{
    if (!kb_repeat.active) return;

    uint64_t now = __atomic_load_n(&g_global_tick, __ATOMIC_RELAXED);
    if (now < kb_repeat.next_repeat_tick) return;

    /* Fire repeat */
    /* A repeat says the same key again — the same event, an arrow included.
     * Building it afresh from the poll-ring bytes once turned a held arrow
     * into a stream of ESC characters. */
    kb_publish_event(&kb_repeat.held_event);

    /* After initial delay, switch to fast repeat rate */
    if (!kb_repeat.delay_passed) {
        kb_repeat.delay_passed = 1;
    }
    kb_repeat.next_repeat_tick = now + g_kb_repeat_rate_ticks;
}

keyboard_state_t* keyboard_get_state(void)
{
    return &kb_state;
}

