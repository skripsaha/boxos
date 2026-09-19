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

/* ─── Pause burst (0xE1) state ──────────────────────────────────────────── */

/* How many bytes of an 0xE1 group are still to be swallowed. Pause is the
 * only key that carries this prefix, and its bytes must never reach the
 * modifier, lock or repeat logic — see keyboard_handle_scancode. */
static volatile uint8_t kb_e1_swallow = 0;

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
 * goes through the Touch IRQ ring and a knock, because TouchPublish would
 * take the TagFS registry lock and per-bucket spinlocks with IF=0 — the
 * deadlock pattern the ring exists to break. */
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

/* ‼ THE REPEAT IS WRITTEN FROM MORE THAN ONE CORE, AND HAD NOTHING HOLDING IT.
 *
 * The obvious reading is that all three touchers run on the BSP — PS/2 IRQ1,
 * the PIT's IRQ0, and the xHCI interrupt, which xhci.c points at
 * g_amp.bsp_lapic_id for MSI-X, MSI and the INTx fallback alike. It is wrong,
 * and the way it is wrong is specific to this machine: the xHCI EVENT RING IS
 * ALSO DRAINED SYNCHRONOUSLY, by whichever core is waiting for something on
 * it, from eight polling sites (xhci_command.c, xhci_msd.c, xhci_endpoint.c,
 * xhci_enumeration.c, xhci.c, boardroom.c). msd_gate_enter says so in words:
 * the holder is waiting for an answer off the event ring "on a core that is
 * not the one taking the controller's interrupt". And on this system the
 * VOLUME IS A USB STICK, so every block read drains that ring on the core
 * doing the reading — and the drain delivers keyboard transfer events with it.
 *
 * So: a writer on any core against a reader-writer on the BSP, with the event
 * ring's own lock between drains and NOTHING between a drain and the PIT.
 *
 * Two things come out of that, and the second is the serious one.
 *   - kb_event_t is three PACKED bytes, so held_event is written as sub-word
 *     stores and can be read half-updated: a new scancode with an old ascii,
 *     one wrong character at the moment a key is pressed.
 *   - `active` is written 1 by the arm and 0 by the release. A press and its
 *     release can be drained by two different cores, and if the arm's 1 lands
 *     after the release's 0, `active` stays 1 WITH NO KEY HELD — and the tick
 *     republishes it about thirty times a second until something else is
 *     pressed. A key that repeats forever while nobody is touching the
 *     keyboard, with nothing unplugged and no scan code lost.
 *
 * A leaf spinlock is the whole answer. It guards this record and nothing else,
 * it calls nothing, every one of its three holders already runs with interrupts
 * off, and the tick copies the event out before it publishes so the lock is
 * never held across the Touch ring. `active` is also stored LAST in the arm,
 * after the fields a reader would use — under the lock that is belt and
 * braces, but it means the publication order is right by construction rather
 * than by the lock's grace. */
static spinlock_t kb_repeat_lock;


/* ═══════════════════════════════════════════════════════════════════════════
   Internal helpers
   ═══════════════════════════════════════════════════════════════════════════ */

/* Public: feed characters from an external input source (the COM1 serial
 * console — see serial_console_init) as "keyboard" Touch events, so a real
 * keypress and an injected byte are indistinguishable to whoever listens.
 * Deferred (IRQ-safe) publish is mandatory: keyboard_inject runs in the COM1
 * IRQ handler, same as the PS/2 site — a direct TouchPublish would take TagFS
 * registry locks with IF=0 (the deadlock pattern the ring exists to break). scancode 0 says
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
    spin_lock(&kb_repeat_lock);
    kb_repeat.held_key         = key;
    kb_repeat.held_is_extended = is_extended;
    kb_repeat.held_event       = *event;
    kb_repeat.delay_passed     = 0;
    kb_repeat.next_repeat_tick = __atomic_load_n(&g_global_tick, __ATOMIC_RELAXED)
                                 + g_kb_repeat_delay_ticks;
    kb_repeat.active           = 1;   /* last: nothing may be seen half-armed */
    spin_unlock(&kb_repeat_lock);
}

/* Cancel repeat if the released key matches the currently repeating key. */
static void kb_release_repeat(uint8_t key, uint8_t is_extended)
{
    spin_lock(&kb_repeat_lock);
    if (kb_repeat.active &&
        kb_repeat.held_key == key &&
        kb_repeat.held_is_extended == is_extended)
    {
        kb_repeat.active = 0;
    }
    spin_unlock(&kb_repeat_lock);
}

/* Everything about the repeat is now unknown — used where the transport says
 * it lost bytes. Silence is the only honest state after that: a repeat whose
 * break code may never come is the one shape this record cannot recover from
 * on its own. */
static void kb_forget_repeat(void)
{
    spin_lock(&kb_repeat_lock);
    kb_repeat.active = 0;
    spin_unlock(&kb_repeat_lock);
}

/* ═══════════════════════════════════════════════════════════════════════════
   8042 PS/2 controller helpers
   ═══════════════════════════════════════════════════════════════════════════ */

/* Commands, command-byte bits and status bits under the names the reference
 * implementation gives them (Linux include/linux/i8042.h). */
#define KBC_CMD_AUX_DISABLE  0xA7   /* second port off, its clock driven low */
#define KBC_CMD_KBD_DISABLE  0xAD   /* sets command-byte bit 4               */
#define KBC_CMD_KBD_ENABLE   0xAE   /* clears command-byte bit 4             */
#define KBC_CMD_READ_CTR     0x20   /* read the command byte                 */
#define KBC_CMD_WRITE_CTR    0x60   /* write the command byte                */
#define KBC_CMD_RESET_KBD    0xFF   /* to the device: reset and self-test    */
#define KBC_CMD_SET_LEDS     0xED   /* to the device: the three lock lamps   */

#define KBC_CTR_KBDINT       0x01   /* first port raises IRQ1                */
#define KBC_CTR_AUXINT       0x02   /* second port raises IRQ12              */
#define KBC_CTR_KBDDIS       0x10   /* first port disabled                   */
#define KBC_CTR_AUXDIS       0x20   /* second port disabled                  */

#define KBC_STR_OBF          0x01   /* 0x60 holds a byte for the system      */
#define KBC_STR_IBF          0x02   /* 0x60/0x64 still holds one of ours     */

/* The controller answered at init and is configured as this driver wants it.
 * Nothing may be sent to the 8042 until it is true: a lock key pressed on a
 * USB keyboard must not spend 50 ms in IRQ context waiting on a controller
 * that is not there. */
static bool g_kb_8042_alive = false;

/* The input buffer is free, so the next byte written is the controller's to
 * read. False when it never frees — and then NOTHING is written, because a
 * controller still holding the previous byte swallows this one whole and the
 * answer we would go on to wait for never comes. */
static bool kb_wait_input_buffer(void)
{
    uint64_t deadline = rdtsc() + cpu_ms_to_tsc(50);
    while ((inb(KEYBOARD_STATUS_PORT) & KBC_STR_IBF) && rdtsc() < deadline) {
        __asm__ volatile("pause");
    }
    return (inb(KEYBOARD_STATUS_PORT) & KBC_STR_IBF) == 0;
}

/* One byte to the command port (0x64) or to the data port (0x60), once the
 * controller can take it. False = not sent. */
static bool kb_send(uint16_t port, uint8_t byte)
{
    if (!kb_wait_input_buffer()) return false;
    outb(port, byte);
    return true;
}

static bool kb_wait_output_buffer(void)
{
    uint64_t deadline = rdtsc() + cpu_ms_to_tsc(50);
    while (!(inb(KEYBOARD_STATUS_PORT) & KBC_STR_OBF) && rdtsc() < deadline) {
        __asm__ volatile("pause");
    }
    return (inb(KEYBOARD_STATUS_PORT) & KBC_STR_OBF) != 0;
}

static void kb_flush_output(void)
{
    uint32_t timeout = 16;
    while ((inb(KEYBOARD_STATUS_PORT) & KBC_STR_OBF) && timeout--) {
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
    if (!g_kb_8042_alive) return;

    /* The remembered state follows what actually went out: a command the
     * controller never took must be tried again, not counted as done. */
    if (!kb_send(KEYBOARD_DATA_PORT, KBC_CMD_SET_LEDS)) return;
    if (!kb_send(KEYBOARD_DATA_PORT, bits)) return;
    kb_led_state = bits;
}

/* The 8042's own half of the driver's start: everything that only a
 * controller answering can finish. Nothing here may take the rest of the
 * driver down with it — by the time this runs, the "keyboard" tag handles
 * and the repeat tick counts are already published, so a silent controller
 * costs the PS/2 port and nothing else.
 *
 * The 0xAD that opens the sequence stops the keyboard talking over the
 * configuration, and it is a debt: after 0xAD the first port stays disabled
 * until either 0xAE or a command byte written with bit 4 clear (Linux
 * I8042_CMD_KBD_ENABLE / I8042_CTR_KBDDIS; EDK2's Ps2KbdCtrller re-enables
 * by writing the command byte alone). A return that does neither leaves the
 * port dead for the rest of the boot, so every exit path below pays it or
 * says out loud why it cannot. */
static bool kb_8042_init(void)
{
    if (!kb_send(KEYBOARD_STATUS_PORT, KBC_CMD_KBD_DISABLE)) {
        kprintf("[KEYBOARD] 8042 never freed its input buffer; "
                "PS/2 port left untouched, other input sources unaffected\n");
        return false;
    }

    kb_flush_output();

    if (!kb_send(KEYBOARD_STATUS_PORT, KBC_CMD_READ_CTR) ||
        !kb_wait_output_buffer()) {
        kprintf("[KEYBOARD] 8042 did not answer the command-byte read; "
                "re-enabling the PS/2 port and giving it up\n");
        (void)kb_send(KEYBOARD_STATUS_PORT, KBC_CMD_KBD_ENABLE);
        return false;
    }

    uint8_t ccb = inb(KEYBOARD_DATA_PORT);
    ccb |=  KBC_CTR_KBDINT;   /* first port: IRQ1 on   */
    ccb &= ~KBC_CTR_KBDDIS;   /* first port: clock on  */

    /* ‼ THE SECOND PORT IS NOT OURS TO LEAVE RUNNING.
     *
     * One output register at 0x60 serves both ports. Nothing in this kernel
     * serves the second one: there is no IRQ12 handler and that line is never
     * unmasked. Firmware, though, does leave it running — EDK2's Ps2MouseDxe
     * enables the auxiliary interface and turns mouse reporting on, and
     * registers no ExitBootServices callback to turn either back off — so on
     * a board with a PS/2 mouse or a PS/2-attached touchpad the first
     * movement parks a byte in that register with OBF set. The controller
     * then cannot load the keyboard's next byte, IRQ1 stops firing, and the
     * keyboard is dead until reboot with nothing said. Linux states the
     * mechanism in i8042_check_aux in as many words — "otherwise keyboard may
     * not work" — and leaves an unserved port exactly as this does:
     * `i8042_ctr |= I8042_CTR_AUXDIS; i8042_ctr &= ~I8042_CTR_AUXINT;`. */
    ccb &= ~KBC_CTR_AUXINT;
    ccb |=  KBC_CTR_AUXDIS;

    if (!kb_send(KEYBOARD_STATUS_PORT, KBC_CMD_WRITE_CTR)) {
        kprintf("[KEYBOARD] 8042 would not take the command-byte write; "
                "re-enabling the PS/2 port and giving it up\n");
        (void)kb_send(KEYBOARD_STATUS_PORT, KBC_CMD_KBD_ENABLE);
        return false;
    }
    if (!kb_send(KEYBOARD_DATA_PORT, ccb)) {
        /* 0x60 was taken and the controller is waiting for its parameter.
         * Anything sent now is read as that parameter, so there is no
         * command left that could re-enable the port: say so and stop. */
        kprintf("[KEYBOARD] 8042 took the command-byte write and never asked "
                "for its value; PS/2 port left disabled\n");
        return false;
    }

    /* The command byte above already says it; the command says it again to
     * controllers that act on the command alone (EDK2 KbcDisableAux). */
    (void)kb_send(KEYBOARD_STATUS_PORT, KBC_CMD_AUX_DISABLE);

    if (!kb_send(KEYBOARD_STATUS_PORT, KBC_CMD_KBD_ENABLE)) {
        kprintf("[KEYBOARD] 8042 would not re-enable the PS/2 port\n");
        return false;
    }

    kb_flush_output();
    g_kb_8042_alive = true;

    irqchip_enable_irq(1);
    debug_printf("[KEYBOARD] IRQ1 enabled\n");

    (void)kb_send(KEYBOARD_DATA_PORT, KBC_CMD_RESET_KBD);

    /* Wait ~50 ms for ACK (0xFA) and self-test result (0xAA) */
    {
        uint64_t deadline = rdtsc() + cpu_ms_to_tsc(50);
        while (rdtsc() < deadline) {
            __asm__ volatile("pause");
        }
    }

    /* Ensure all LEDs start off */
    keyboard_set_leds(0, 0, 0);

    debug_printf("[KEYBOARD] 8042 initialization complete\n");
    return true;
}

void keyboard_init(void)
{
    memset(&kb_state, 0, sizeof(kb_state));
    spinlock_init(&kb_repeat_lock);
    memset(&kb_repeat, 0, sizeof(kb_repeat));
    kb_e0_pending = 0;
    kb_e1_swallow = 0;
    kb_led_state  = 0;

    /* ‼ THE DRIVER'S OWN STATE COMES FIRST, AND NOT FOR TIDINESS.
     *
     * The two tag handles below are the only ones in the tree: the xHCI-HID
     * path (keyboard_handle_scancode) and the COM1 wire (keyboard_inject)
     * both publish through them, and nothing else ever writes them. They used
     * to be resolved at the END of this function, downstream of the whole
     * 8042 sequence — so a controller that did not answer returned early and
     * left both at TOUCH_TAG_INVALID, on which TouchPublishIrqPair returns at
     * once: every USB keystroke and every byte off the serial line published
     * to nothing. A shell taking no input at all on a machine whose keyboard
     * works perfectly is the exact failure main.c's contract above this call
     * says must never happen again. A PS/2 controller's silence may cost the
     * PS/2 port and nothing more.
     *
     * The repeat tick counts move up for the same reason: they are read by
     * the PIT tick for repeats armed on ANY input path. */
    g_kb_repeat_delay_ticks = (KB_REPEAT_DELAY_MS * g_timer_frequency) / 1000;
    g_kb_repeat_rate_ticks  = (KB_REPEAT_RATE_MS * g_timer_frequency) / 1000;

    /* A count that rounds down to zero ticks is a repeat republished on every
     * single tick. One tick is the shortest interval this timer can express,
     * so it is the floor. */
    if (g_kb_repeat_delay_ticks == 0) g_kb_repeat_delay_ticks = 1;
    if (g_kb_repeat_rate_ticks  == 0) g_kb_repeat_rate_ticks  = 1;

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
    debug_printf("[KEYBOARD] Repeat: delay=%u ticks (%u ms), rate=%u ticks (%u ms)\n",
                 g_kb_repeat_delay_ticks, KB_REPEAT_DELAY_MS,
                 g_kb_repeat_rate_ticks, KB_REPEAT_RATE_MS);

    debug_printf("[KEYBOARD] Initializing 8042 PS/2 controller...\n");
    (void)kb_8042_init();
}

/* ─── Scancode handler (called from IRQ1) ──────────────────────────────── */

void keyboard_handle_scancode(uint8_t scancode)
{
    /* ── 0xE0 prefix: mark and wait for next byte ── */
    if (scancode == 0xE0) {
        kb_e0_pending = 1;
        return;
    }

    /* ── 0xE1 prefix: the Pause burst, which is no key this driver reports ──
     *
     * Set 1 — what the 8042 delivers with translation on — sends Pause as six
     * bytes, E1 1D 45 E1 9D C5, all of them at make time and nothing at all
     * on release (Brouwer, "Keyboard scancodes": "e1 1d 45 e1 9d c5 … nothing
     * at all upon release"). Read one byte at a time with no prefix state the
     * 0x1D lands on Left Ctrl and the 0x45 on Num Lock, so a single Pause
     * press used to invert kb_state.num_lock and fire an 0xED LED command
     * from inside IRQ1, in the middle of the burst. Each 0xE1 group is
     * swallowed whole — two bytes, exactly as Linux's atkbd does it with
     * `atkbd->emul = 2`, and the second 0xE1 re-arms the same count. EDK2's
     * Ps2KbdCtrller guards its entire modifier/lock switch with `if
     * (!Extend1)` for this one key. */
    if (scancode == 0xE1) {
        kb_e1_swallow = 2;
        return;
    }

    /* ── Filter controller response codes ── */
    switch (scancode) {
        case 0xFA: return;   /* ACK           */
        case 0xFE: return;   /* Resend        */
        case 0xEE: return;   /* Echo response */
        case 0x00:           /* Error / buffer overrun */
        case 0xFF:           /* Error                  */
            /* ‼ THIS IS THE KEYBOARD SAYING IT DROPPED SCAN CODES, and it used
             * to be discarded like an ACK. Everything derived from codes that
             * did not arrive is now a guess: a break code may be among them, so
             * a repeat could be armed on a key nobody is holding; the second
             * byte of an 0xE0 pair may be among them, so the next ordinary key
             * would read as extended; and a modifier's release may be among
             * them, so every following letter would come out shifted. None of
             * those heal by themselves — the evidence that would have healed
             * them is exactly what was lost. So the state that depends on codes
             * is cleared here, and the cost of an overrun becomes one report of
             * unknown modifiers instead of a repeat nothing can silence. */
            kb_e0_pending = 0;
            kb_e1_swallow = 0;
            kb_forget_repeat();
            __atomic_store_n(&kb_state.shift_pressed, 0, __ATOMIC_RELAXED);
            __atomic_store_n(&kb_state.ctrl_pressed,  0, __ATOMIC_RELAXED);
            __atomic_store_n(&kb_state.alt_pressed,   0, __ATOMIC_RELAXED);
            return;
    }

    /* Inside an 0xE1 group: the byte belongs to Pause, not to the key its
     * value would otherwise name. */
    if (kb_e1_swallow) {
        kb_e1_swallow--;
        return;
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
    uint64_t   now = __atomic_load_n(&g_global_tick, __ATOMIC_RELAXED);
    kb_event_t say;
    bool       due = false;

    /* Decide and take a COPY under the lock; publish outside it. The Touch IRQ
     * ring is not a place to stand holding a leaf lock, and the copy is what
     * makes the lock a leaf at all. */
    spin_lock(&kb_repeat_lock);
    if (kb_repeat.active && now >= kb_repeat.next_repeat_tick) {
        /* A repeat says the same key again — the same event, an arrow
         * included. Building it afresh from the poll-ring bytes once turned a
         * held arrow into a stream of ESC characters. */
        say = kb_repeat.held_event;
        due = true;
        kb_repeat.delay_passed     = 1;   /* the initial delay is behind us */
        kb_repeat.next_repeat_tick = now + g_kb_repeat_rate_ticks;
    }
    spin_unlock(&kb_repeat_lock);

    if (due) kb_publish_event(&say);
}

keyboard_state_t* keyboard_get_state(void)
{
    return &kb_state;
}

