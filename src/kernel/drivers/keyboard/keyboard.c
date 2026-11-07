#include "keyboard.h"
#include "idt.h"   /* irq_register_handler + the kernel's own answerers */
#include "keymaps.h"
#include "klib.h"
#include "io.h"
#include "atomics.h"
#include "cpu_calibrate.h"
#include "irqchip.h"
#include "scheduler.h"   /* g_global_tick, g_timer_frequency */
#include "touch.h"
#include "screen.h"      /* ScreenSwapKnock — Ctrl+Alt+Esc is the machine's */
#include "logbook.h"
#include "amp.h"         /* amp_get_core_index, MAX_CORES — per-core prefix state */
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
static volatile TouchTag g_kbd_touch_full = TOUCH_TAG_INVALID;
static volatile TouchTag g_kbd_touch_bare = TOUCH_TAG_INVALID;

/* ─── Prefix state (0xE0 extended, 0xE1 Pause) — ONE SLOT PER CORE ────────
 *
 * ‼ A PREFIX BELONGS TO THE STREAM THAT SENT IT, AND THIS USED TO BE ONE BYTE.
 *
 * The 0xE0 prefix and the byte after it are one indivisible scan code
 * (Chapweske, "The PS/2 Keyboard Interface", Scan Codes: extended make codes
 * "can be identified by the fact that their first byte is E0h"). Two
 * independent transports feed keyboard_handle_scancode, and the header block
 * above says why they are not the same core: PS/2 IRQ1 is aimed at the BSP
 * alone (irqchip's IOAPIC destination is g_amp.bsp_lapic_id, and the PIC
 * fallback delivers to the BSP too), while a USB HID report is decoded by
 * whichever core drains the xHCI event ring — every block read off a USB
 * volume does that.
 *
 * With one shared byte the two streams consumed each other's prefixes. A PS/2
 * extended key delivers its two bytes as two separate IRQ1s paced by the PS/2
 * link (11-bit frames at 10-16.7 kHz, so ~0.66-1.1 ms apart), and any USB
 * report drained on another core inside that window took the prefix: the PS/2
 * letter published as an extended key and armed the repeat with
 * is_extended = 1, its genuine break code then failed the
 * `held_is_extended == is_extended` test in kb_release_repeat, and the phantom
 * extended key repeated at the typematic rate until some other key overwrote
 * the record. Both keystrokes destroyed, and one of them stuck.
 *
 * A lock does not fix that — it makes each access atomic, which a single byte
 * on x86 already is, and still lets a USB byte pair with a PS/2 prefix. A slot
 * per core does: the PS/2 stream lives on the BSP's slot and nothing else
 * writes it, and a USB report's two calls are adjacent on the draining core
 * with IF=0 (the xHCI event lock holds `cli` across the drain), so that pair
 * cannot be cut into at all. Linux keeps the same state per device rather than
 * per machine — `u8 emul` inside `struct atkbd`.
 *
 * ‼ WHAT IS LEFT, NAMED: a USB report drained ON THE BSP, between a PS/2
 * extended key's two IRQ1s, still shares the BSP's slot with the PS/2 stream
 * and takes its prefix. Closing that needs a prefix instance per TRANSPORT,
 * i.e. a second entry point for the USB path — keyboard.h and xhci_hid.c, not
 * this file. */
static volatile uint8_t kb_e0_pending[MAX_CORES];

/* How many bytes of an 0xE1 group are still to be swallowed, per core. Pause
 * is the only key that carries this prefix, and its bytes must never reach the
 * modifier, lock or repeat logic — see keyboard_handle_scancode. */
static volatile uint8_t kb_e1_swallow[MAX_CORES];

/* Esc is down as half of Ctrl+Alt+Esc (see keyboard_handle_scancode). A
 * keyboard's own typematic repeats a held key's make code, and the screen
 * turns over once per press, not once per repeat: the edge is the first make
 * after a break. One byte for every transport — PS/2 and USB HID can decode on
 * two cores at once, hence the exchange. */
static volatile uint8_t kb_screen_key_down;

/* ─── LED state ─────────────────────────────────────────────────────────── */

/* ‼ THE LAMP COMMAND IS TWO BYTES ON ONE LINE, AND TWO CORES CAN BEGIN IT AT
 * THE SAME INSTANT.
 *
 * keyboard_set_leds reads like the BSP's alone and is not, for the reason the
 * prefix state above is per-core: a USB lock key reaches it. Usage 0x39 maps
 * to set-1 0x3A in usb_to_ps2 (keymaps.h), xhci_hid.c hands that scancode to
 * keyboard_handle_scancode, and a report is decoded by whichever core drains
 * the xHCI event ring — every block read off a USB volume does that. So PS/2
 * IRQ1 on the BSP and a USB report on any other core can both be in here.
 *
 * With nothing between them the two writes to 0x60 interleave as 0xED / 0xED
 * / bitmap / bitmap, and the keyboard takes the second 0xED as the FIRST
 * command's lamp argument and the bitmaps as commands of their own —
 * Chapweske, Command Set: a command that takes "one more byte" takes whatever
 * the next byte on the line is. The lamps then disagree with the driver AND
 * the scan code stream carries the answers to two undefined commands.
 *
 * A leaf spinlock owns the conversation, taken with spin_trylock and never
 * spin_lock: the loser must not stand in IRQ context behind a holder that may
 * be inside kb_wait_input_buffer's 50 ms. It does not need to. The wish is
 * published BEFORE the lock is attempted, the holder re-reads it after every
 * exchange, and the loop re-checks once more after the unlock — so a wish
 * raised while the lock is held is carried out by whoever holds it, and one
 * raised in the instant between the holder's last read and its unlock is
 * picked up by that same holder's next turn round the loop. Nothing is
 * dropped and no core waits on another.
 *
 * ‼ WHAT IS LEFT, NAMED: the 0xFA between the two halves is not seen here.
 * Once IRQ1 is live the keyboard's answers arrive as interrupts and are
 * dropped by keyboard_handle_scancode's response filter, so a 0xFE ("could
 * not read that, send it again") is lost and that lamp stays wrong until the
 * key is pressed again. Polling 0x60 for it is not available — it would take
 * keystrokes out of the stream the shell is reading. Closing it means the
 * decoder, which already hears those bytes, driving the conversation; which
 * mechanism that is, is the owner's to pick. */
static spinlock_t kb_led_lock;

/* What the lock keys ask the lamps to show. Written by any core, before it
 * attempts kb_led_lock. */
static uint8_t kb_led_wanted = 0;

/* What the keyboard was last told to show. Written only under kb_led_lock;
 * read outside it by the loop condition in keyboard_set_leds. */
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
#define KBC_CMD_SET_SCANSET  0xF0   /* to the device: which scan code set    */

#define KBC_CTR_KBDINT       0x01   /* first port raises IRQ1                */
#define KBC_CTR_AUXINT       0x02   /* second port raises IRQ12              */
#define KBC_CTR_KBDDIS       0x10   /* first port disabled                   */
#define KBC_CTR_AUXDIS       0x20   /* second port disabled                  */
#define KBC_CTR_XLATE        0x40   /* set 2 off the wire, set 1 at 0x60     */

#define KBC_STR_OBF          0x01   /* 0x60 holds a byte for the system      */
#define KBC_STR_IBF          0x02   /* 0x60/0x64 still holds one of ours     */

/* The keyboard's own vocabulary back to the host. */
#define KB_RSP_ACK           0xFA   /* the byte was taken                    */
#define KB_RSP_RESEND        0xFE   /* could not read it — send it again     */
#define KB_RSP_ECHO          0xEE   /* answer to 0xEE                        */
#define KB_RSP_BAT_OK        0xAA   /* self-test passed                      */
#define KB_RSP_BAT_FAIL      0xFC   /* self-test failed                      */
#define KB_RSP_OVERRUN       0x00   /* scan codes were dropped               */
#define KB_RSP_ERROR         0xFF   /* scan codes were dropped               */

#define KB_SCANSET_1         0x01   /* argument to 0xF0: scan code set 1     */
#define KB_SCANSET_2         0x02   /* argument to 0xF0: scan code set 2     */

/* ─── How long an answer is waited for ────────────────────────────────────
 *
 * Both numbers come from the reference implementation rather than from this
 * file's judgement (Linux drivers/input/serio/libps2.c, __ps2_command): every
 * byte goes out as `ps2_do_sendbyte(ps2dev, ..., 200, 2)`, and the wait for
 * the reset's result is `msecs_to_jiffies(command == PS2_CMD_RESET_BAT ? 4000
 * : 500)`. Linux raises the ACK wait to 1000 ms for the reset byte alone, and
 * says why in the comment above it: "Some devices (Synaptics) perform the
 * reset before ACKing the reset command" — an AUX-port touchpad, which cannot
 * be the device here because the second port is disabled in the command byte
 * and nothing but the first port is ever addressed. The reset's
 * acknowledgement therefore keeps the ordinary 200 ms, and that 200 ms is the
 * entire cost of a controller whose first port has nothing plugged into it.
 *
 * Neither number is a guess at how long the work takes. Every wait below ends
 * on the BYTE; the number is only the point at which the device is declared
 * absent, and that verdict is said out loud through kprintf rather than
 * folded into "initialization complete". */
#define KB_ACK_MS            200
#define KB_BAT_MS           4000

/* The controller answered at init, is configured as this driver wants it, and
 * this driver has finished talking to the keyboard. Nothing may be sent to the
 * 8042 until it is true, for two reasons. A lock key pressed on a USB keyboard
 * must not spend 50 ms in IRQ context waiting on a controller that is not
 * there — and it must not drop its 0xED and a bitmap into 0x60 in the middle
 * of the reset/self-test/scan-set exchange at the end of kb_8042_init, whose
 * polls would then read the acknowledgements of those bytes as the answers to
 * their own. xhci_init runs before keyboard_init (main.c), so that path is
 * live throughout; the flag is therefore raised at the very END of
 * kb_8042_init, not as soon as the controller is configured. */
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

/* ─── Talking to the keyboard, not to the controller ──────────────────────
 *
 * Chapweske, "The PS/2 Keyboard Interface", Command Set: "Every byte sent to
 * the keyboard gets a response of 0xFA ('acknowledge') from the keyboard. The
 * only exceptions to this are the keyboard's response to the 'Resend' and
 * 'Echo' commands. The host should wait for an 'acknowledge' before sending
 * the next byte to the keyboard." So a device command is a conversation, and
 * the 8042's input-buffer bit is no part of it: that bit says the CONTROLLER
 * took the byte, and says nothing about whether the keyboard did.
 *
 * ‼ THESE TWO ARE USABLE ONLY WHILE IRQ1 IS STILL MASKED. Once the line is
 * unmasked the same answers arrive as interrupts, and a poll here would take
 * bytes out of the stream the shell is reading. Every caller is inside
 * kb_8042_init, ahead of its irqchip_enable_irq(1). */

/* The next thing the keyboard says. False = it said nothing before the
 * deadline, and then there is no byte — an invented one is worse than none.
 * Scan codes seen on the way are dropped on purpose: no decoder is running
 * yet, and a keystroke decoded out of order is worse than a keystroke a user
 * lost by leaning on a key while the port came up. */
static bool kb_await_response(uint8_t *out, uint32_t ms)
{
    uint64_t deadline = rdtsc() + cpu_ms_to_tsc(ms);
    do {
        if (inb(KEYBOARD_STATUS_PORT) & KBC_STR_OBF) {
            uint8_t b = inb(KEYBOARD_DATA_PORT);
            if (b == KB_RSP_ACK || b == KB_RSP_RESEND ||
                b == KB_RSP_BAT_OK || b == KB_RSP_BAT_FAIL ||
                b == KB_RSP_ECHO) {
                *out = b;
                return true;
            }
            /* Not an answer, and the deadline is still the bound: a port
             * whose output register floats 0xFF would otherwise hand back a
             * byte every time round and this would never end. */
            continue;
        }
        __asm__ volatile("pause");
    } while (rdtsc() < deadline);
    return false;
}

/* One byte to the keyboard, acknowledged. A 0xFE means the keyboard could not
 * read it and wants it again (Chapweske, Resend), so it goes out once more —
 * two attempts in all, which is what Linux's ps2_do_sendbyte allows. False =
 * the keyboard never took it, and the caller says so rather than proceeding
 * as though it had.
 *
 * ‼ A FAILURE CLEARS THE OUTPUT REGISTER, AND THAT IS NOT TIDINESS.
 *
 * The deadline expiring does not mean the answer will never come; it means it
 * had not come yet. A keyboard that finishes its self-test one millisecond
 * after the wait gave up leaves its 0xAA sitting at 0x60, where the NEXT
 * command's wait reads it as ITS answer — and from there every verdict in the
 * sequence is one step out of phase: a healthy keyboard reported as refusing
 * scan code set 2, and a lamp command reported as taken when its bitmap never
 * went out. One late byte would falsify everything after it, so the register
 * is emptied at the point the conversation is known to have broken. */
static bool kb_device_byte(uint8_t byte, uint32_t ms)
{
    for (unsigned attempt = 0; attempt < 2; attempt++) {
        if (!kb_send(KEYBOARD_DATA_PORT, byte)) break;
        uint8_t answer;
        if (!kb_await_response(&answer, ms)) break;
        if (answer == KB_RSP_ACK)    return true;
        if (answer != KB_RSP_RESEND) break;
    }
    kb_flush_output();
    return false;
}

/* A two-byte device command: 0xED + its lamp bitmap, 0xF0 + its set number.
 *
 * ‼ THE ARGUMENT IS NOT OPTIONAL, EVEN WHEN THE COMMAND BYTE WENT UNANSWERED.
 *
 * Chapweske's Command Set describes these as commands that take "one more
 * byte": once the command byte has reached the keyboard, the keyboard waits
 * and takes the NEXT byte on the line as that argument, whatever it is.
 * Withholding the argument because the command byte drew no acknowledgement
 * therefore leaves the device mid-command with IRQ1 about to go live, and the
 * first key the user presses is eaten as the missing parameter. So the
 * argument always goes out. On a device that was NOT waiting for one, the
 * argument is read as a command of its own, both values (0x01/0x02 for a set,
 * a lamp bitmap of 0x00-0x07) are undefined commands, and an undefined
 * command draws a 0xFE that the flush inside kb_device_byte discards.
 *
 * False = the keyboard did not acknowledge both halves, so what it is now set
 * to is unknown and the caller must say so. */
static bool kb_device_command(uint8_t cmd, uint8_t arg, uint32_t ms)
{
    bool cmd_ok = kb_device_byte(cmd, ms);
    bool arg_ok = kb_device_byte(arg, ms);
    return cmd_ok && arg_ok;
}

/* The controller's command byte, as the controller itself reports it. False =
 * it did not answer, and then there is no value — see kb_await_response. */
static bool kb_read_ccb(uint8_t *out)
{
    if (!kb_send(KEYBOARD_STATUS_PORT, KBC_CMD_READ_CTR)) return false;
    if (!kb_wait_output_buffer()) return false;
    *out = inb(KEYBOARD_DATA_PORT);
    return true;
}

/* ═══════════════════════════════════════════════════════════════════════════
   Public API
   ═══════════════════════════════════════════════════════════════════════════ */

void keyboard_set_leds(uint8_t caps, uint8_t num, uint8_t scroll)
{
    uint8_t bits = (uint8_t)((scroll ? 0x01 : 0) | (num ? 0x02 : 0) |
                             (caps ? 0x04 : 0));

    /* The wish goes up first, so a core that cannot get the lock has still
     * said what it wants and the holder will carry it out. */
    __atomic_store_n(&kb_led_wanted, bits, __ATOMIC_RELEASE);

    if (!g_kb_8042_alive) return;

    while (__atomic_load_n(&kb_led_wanted, __ATOMIC_ACQUIRE) !=
           __atomic_load_n(&kb_led_state,  __ATOMIC_ACQUIRE)) {

        /* Held means another core is mid-conversation and has already been
         * told, by the store above, what this one wants. */
        if (!spin_trylock(&kb_led_lock)) return;

        uint8_t want = __atomic_load_n(&kb_led_wanted, __ATOMIC_ACQUIRE);
        bool    sent = kb_send(KEYBOARD_DATA_PORT, KBC_CMD_SET_LEDS) &&
                       kb_send(KEYBOARD_DATA_PORT, want);

        /* What is remembered follows what actually went out: a byte the
         * controller never took must be tried again, not counted as done. */
        if (sent) __atomic_store_n(&kb_led_state, want, __ATOMIC_RELEASE);
        spin_unlock(&kb_led_lock);

        if (!sent) {
            kprintf("[KEYBOARD] the 8042 never freed its input buffer for the "
                    "lamp command; Caps/Num/Scroll lamps now disagree with the "
                    "driver until a lock key is pressed again\n");
            return;
        }
    }
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

    uint8_t ccb;
    if (!kb_read_ccb(&ccb)) {
        kprintf("[KEYBOARD] 8042 did not answer the command-byte read; "
                "re-enabling the PS/2 port and giving it up\n");
        (void)kb_send(KEYBOARD_STATUS_PORT, KBC_CMD_KBD_ENABLE);
        return false;
    }

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

    /* ‼ THE DECODER IS SET 1, AND NOTHING USED TO SAY SO.
     *
     * keymaps.h is a set-1 table (0x1E = 'a', 0x1C = Enter, 0x2A = Left
     * Shift) and keyboard_handle_scancode reads bit 7 as the release bit,
     * which is the set-1 convention. The keyboard itself does not speak set 1:
     * it speaks set 2, and after the reset this function sends it is set 2 by
     * definition (Chapweske, Reset — the defaults reloaded include "Scan code
     * set 2"). Set-1 bytes appear at 0x60 only because the controller
     * translates them, and that is command-byte bit 6 (Linux
     * include/linux/i8042.h: I8042_CTR_XLATE 0x40; EDK2's Ps2KbdCtrller writes
     * 0x67 under the comment "6: PC/XT translation mode"; coreboot writes
     * 0x65).
     *
     * This driver used to leave that bit at whatever firmware wrote, without
     * reading it, let alone asserting it — so the whole keymap rested on an
     * inherited setting. A machine handing over with translation clear is the
     * case Linux carries i8042_direct for, and there it would have fed raw set
     * 2 into a set-1 table: 0x1C decoding as Enter instead of 'a', and every
     * break code arriving as the prefix 0xF0, which falls through to the
     * release branch as a release of key 0x70 and leaves the next byte to be
     * read as a make.
     *
     * Asserting the bit is what every firmware named above already does — but
     * writing a bit is not the same as knowing it took, and the finding asks
     * the driver to KNOW the set rather than inherit it. A clone controller
     * that has no translator drops bit 6 on the floor, and a driver that
     * assumed it had stuck would then ask the device for set 2 and feed raw
     * set 2 to a set-1 table with its own hands. So the command byte is read
     * back below and the set asked of the keyboard follows what the
     * controller actually reports: set 2 when it translates, set 1 when it
     * does not — and set 1 straight off the wire is what this decoder reads
     * anyway, make codes with bit 7 as the release bit. */
    ccb |=  KBC_CTR_XLATE;

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

    /* What the controller says it took, not what was written into it. The
     * flush above is why this reads the command byte and not a scan code: the
     * port is enabled from the moment the value landed, so a key struck in
     * that window would otherwise be sitting at 0x60 ahead of the answer. */
    uint8_t want_set = KB_SCANSET_2;
    uint8_t ccb_now;
    if (!kb_read_ccb(&ccb_now)) {
        kprintf("[KEYBOARD] the 8042 would not read its command byte back; "
                "translation is taken to be on because that is what was "
                "written, and the keyboard is asked for scan code set 2\n");
    } else if (!(ccb_now & KBC_CTR_XLATE)) {
        want_set = KB_SCANSET_1;
        kprintf("[KEYBOARD] the 8042 did not take translation (command byte "
                "reads back 0x%02X); the keyboard is asked for scan code set "
                "1 instead, which this decoder reads untranslated\n",
                (unsigned)ccb_now);
    }

    /* ── The keyboard itself, while IRQ1 is still masked ──
     *
     * ‼ THE RESET IS NOT OVER WHEN THE BYTE HAS BEEN WRITTEN, AND THIS USED
     * TO SLEEP 50 ms AND CALL IT DONE. 0xFF is answered with 0xFA, then the
     * keyboard runs its Basic Assurance Test and sends 0xAA (passed) or 0xFC
     * (failed). Chapweske, Reset: that completion code "must be sent 500~750
     * milliseconds after power-on", and "most keyboards ignore their CLOCK
     * and DATA lines until after the BAT completion code has been sent" —
     * so 50 ms was short of it by an order of magnitude, and every byte the
     * old code sent next went into a keyboard that was not listening.
     *
     * ‼ AND A DEVICE THAT DID NOT ANSWER IS NOT ASKED AGAIN. Each byte below
     * costs its own KB_ACK_MS when nothing answers, so a run that carried on
     * regardless would charge a controller with an empty first port three
     * unanswered exchanges instead of one — the very boot time this finding
     * is about. The gates are therefore facts, not tidiness: nothing is sent
     * after a reset that drew no acknowledgement, and nothing is sent after a
     * self-test whose completion code never arrived, because until that code
     * has gone out the keyboard is not listening and both commands would be
     * ignored anyway. The whole cost of a first port with nothing on it is
     * the one KB_ACK_MS of the unanswered reset. */
    if (!kb_device_byte(KBC_CMD_RESET_KBD, KB_ACK_MS)) {
        kprintf("[KEYBOARD] nothing on the PS/2 port acknowledged a reset "
                "inside %u ms; no further byte is sent to a device that is "
                "not answering. The port stays open and whatever arrives on "
                "it later is decoded as set 1\n", (unsigned)KB_ACK_MS);
    } else {
        uint8_t bat;
        bool bat_done = kb_await_response(&bat, KB_BAT_MS);
        if (!bat_done) {
            /* The self-test result may yet be one instruction away, and read
             * as the answer to the next command it would put every verdict
             * after it out of phase. */
            kb_flush_output();
            kprintf("[KEYBOARD] the keyboard acknowledged its reset and then "
                    "sent no self-test result inside %u ms (the spec puts it "
                    "500-750 ms after power-on); nothing further is sent, "
                    "because a keyboard ignores its CLOCK and DATA lines "
                    "until that code has gone out. The port stays open and "
                    "the device's scan code set is whatever the reset left "
                    "it in\n", (unsigned)KB_BAT_MS);
        } else if (bat != KB_RSP_BAT_OK) {
            kprintf("[KEYBOARD] keyboard self-test answered 0x%02X, not 0xAA "
                    "(passed)\n", (unsigned)bat);
        }

        if (bat_done) {
            /* Which set the decoder reads is now stated at both ends: the
             * command byte was read back above, and this says the matching
             * set to the device. EDK2's InitKeyboard does exactly this pair
             * after its own reset ("Set Keyboard to use Scan Code Set 2":
             * 0xF0 then 0x02), and coreboot the same. A keyboard that will
             * not take it is named, because from then on the bytes at 0x60
             * are of an unknown set and the keymap is a guess. */
            if (!kb_device_command(KBC_CMD_SET_SCANSET, want_set, KB_ACK_MS)) {
                kprintf("[KEYBOARD] the keyboard would not take 'scan code "
                        "set %u'; its output is decoded as set 1 regardless, "
                        "and if the device is speaking another set the keys "
                        "will be wrong\n", (unsigned)want_set);
            }

            /* The lamps and kb_led_state have to agree from the first
             * instant. A reset that completed leaves all three dark by itself
             * (Chapweske: the keyboard "enables its three LED indicators" on
             * entering BAT "and turns them off when BAT has completed"), but
             * that is the device's word for it and kb_led_state = 0 would
             * otherwise be an assumption. So the command goes out for real —
             * with the 0xFA between its two halves that the spec asks for,
             * which is the one thing keyboard_set_leds cannot do once IRQ1
             * is live. */
            if (!kb_device_command(KBC_CMD_SET_LEDS, 0x00, KB_ACK_MS)) {
                kprintf("[KEYBOARD] the keyboard would not take the lamp "
                        "command; Caps/Num/Scroll lamps may disagree with the "
                        "driver until each is pressed once\n");
            }
        }
    }

    /* ‼ LAST, AND THE ORDER IS THE POINT. Everything above polls 0x60 for the
     * keyboard's own answers, and keyboard_set_leds — reachable from a USB
     * lock key the whole time, since xhci_init runs before keyboard_init —
     * would drop an 0xED and a bitmap into the middle of that conversation
     * and have its acknowledgements read as the answers to somebody else's
     * bytes. The flag is what keeps it out, so it is raised only once there
     * is nothing left to interrupt, and IRQ1 is unmasked in the same breath:
     * from here on the answers arrive as interrupts and nothing polls. */
    g_kb_8042_alive = true;
    /* Answered through the registry, like every other line. The 8042 used to
     * be `case 1` of a switch on the ISA number at the bottom of irq_handler,
     * which is the same arithmetic the vector pool removes. */
    irq_register_handler(1, IrqTheKeyboardAnswers, RIGHT_OF_WAY_A_PERSON_WAITS);
    irqchip_enable_irq(1);
    debug_printf("[KEYBOARD] IRQ1 enabled\n");

    debug_printf("[KEYBOARD] 8042 initialization complete\n");
    return true;
}

void keyboard_init(void)
{
    memset(&kb_state, 0, sizeof(kb_state));
    spinlock_init(&kb_repeat_lock);
    spinlock_init(&kb_led_lock);
    memset(&kb_repeat, 0, sizeof(kb_repeat));
    memset((void *)kb_e0_pending, 0, sizeof(kb_e0_pending));
    memset((void *)kb_e1_swallow, 0, sizeof(kb_e1_swallow));
    kb_led_state  = 0;
    kb_led_wanted = 0;

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
    /* Which stream's prefix state this byte belongs to — see the comment on
     * kb_e0_pending. amp_get_core_index() is one %gs read once per-core data
     * is live and a table scan before that, and it is valid in every kernel
     * context including this one. */
    uint8_t core = amp_get_core_index();

    /* ── 0xE0 prefix: mark and wait for next byte ── */
    if (scancode == 0xE0) {
        kb_e0_pending[core] = 1;
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
        kb_e1_swallow[core] = 2;
        return;
    }

    /* ── Filter controller response codes ── */
    switch (scancode) {
        case KB_RSP_ACK:     return;   /* ACK           */
        case KB_RSP_RESEND:  return;   /* Resend        */
        case KB_RSP_ECHO:    return;   /* Echo response */
        case KB_RSP_OVERRUN:           /* Error / buffer overrun */
        case KB_RSP_ERROR:             /* Error                  */
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
            kb_e0_pending[core] = 0;
            kb_e1_swallow[core] = 0;
            __atomic_store_n(&kb_screen_key_down, 0, __ATOMIC_RELAXED);
            kb_forget_repeat();
            __atomic_store_n(&kb_state.shift_pressed, 0, __ATOMIC_RELAXED);
            __atomic_store_n(&kb_state.ctrl_pressed,  0, __ATOMIC_RELAXED);
            __atomic_store_n(&kb_state.alt_pressed,   0, __ATOMIC_RELAXED);
            return;
    }

    /* Inside an 0xE1 group: the byte belongs to Pause, not to the key its
     * value would otherwise name. */
    if (kb_e1_swallow[core]) {
        kb_e1_swallow[core]--;
        return;
    }

    uint8_t is_release = scancode & 0x80;
    uint8_t key = scancode & 0x7F;

    /* ══════════════════════════════════════════════
       Extended key (0xE0 prefix was received)
       ══════════════════════════════════════════════ */
    if (kb_e0_pending[core]) {
        kb_e0_pending[core] = 0;

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

    /* Toggle keys — act on press only.
     *
     * ‼ THIS USED TO SAY "IRQ1 IS THE SOLE WRITER (BSP)", AND IT WAS FALSE ON
     * BOTH COUNTS. The same decoder is fed by xhci_hid.c, whose reports are
     * processed by whichever core drains the xHCI event ring (see the block
     * on kb_e0_pending), so two cores can be here at once; and no lock is
     * taken anywhere on this path — the only lock the readers hold is not
     * this driver's, and kb_repeat_lock is never held here. A load, an xor
     * and a store from two cores loses one of two presses, and the lamp
     * bitmap sent afterwards is then built from a state nobody asked for.
     *
     * A single fetch_xor is the whole answer: the field is already
     * _Atomic uint8_t and only ever holds 0 or 1, so flipping bit 0 in one
     * instruction is the toggle, and the value it returns is the state BEFORE
     * the flip — the post-toggle state is that, inverted. The other two locks
     * are read as they are, which is what the lamps are: a picture of the
     * moment this key was pressed. */
    if (key == 0x3A && !is_release) {      /* Caps Lock */
        uint8_t caps = (uint8_t)(__atomic_fetch_xor(&kb_state.caps_lock, 1,
                                                    __ATOMIC_RELAXED) ^ 1);
        keyboard_set_leds(caps,
                          __atomic_load_n(&kb_state.num_lock,    __ATOMIC_RELAXED),
                          __atomic_load_n(&kb_state.scroll_lock, __ATOMIC_RELAXED));
        return;
    }
    if (key == 0x45 && !is_release) {      /* Num Lock */
        uint8_t num = (uint8_t)(__atomic_fetch_xor(&kb_state.num_lock, 1,
                                                   __ATOMIC_RELAXED) ^ 1);
        keyboard_set_leds(__atomic_load_n(&kb_state.caps_lock,   __ATOMIC_RELAXED),
                          num,
                          __atomic_load_n(&kb_state.scroll_lock, __ATOMIC_RELAXED));
        return;
    }
    if (key == 0x46 && !is_release) {      /* Scroll Lock */
        uint8_t scroll = (uint8_t)(__atomic_fetch_xor(&kb_state.scroll_lock, 1,
                                                      __ATOMIC_RELAXED) ^ 1);
        keyboard_set_leds(__atomic_load_n(&kb_state.caps_lock, __ATOMIC_RELAXED),
                          __atomic_load_n(&kb_state.num_lock,  __ATOMIC_RELAXED),
                          scroll);
        return;
    }

    /* ‼ CTRL+ALT+ESC IS THE MACHINE'S, AND NO PROGRAM EVER HEARS IT.
     *
     * It turns the glass over to the Emergency Screen and back (screen.h) —
     * the one way to see what the kernel says when the display program has
     * stopped drawing. It is caught here, in the decoder, because this is the
     * one place that knows the modifiers AND can keep the key from being
     * published: a kernel ear on the `keyboard` Touch would hear it after
     * every program had, and Esc ends half the programs on this machine. The
     * keyboard knows nothing more of the screen than that one call. */
    if (key == 0x01) {                     /* Escape */
        if (is_release) {
            __atomic_store_n(&kb_screen_key_down, 0, __ATOMIC_RELAXED);
        } else if (__atomic_load_n(&kb_state.ctrl_pressed, __ATOMIC_RELAXED) &&
                   __atomic_load_n(&kb_state.alt_pressed,  __ATOMIC_RELAXED)) {
            if (!__atomic_exchange_n(&kb_screen_key_down, 1, __ATOMIC_ACQ_REL))
                ScreenSwapKnock();
            return;
        }
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

