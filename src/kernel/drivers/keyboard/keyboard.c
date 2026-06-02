#include "keyboard.h"
#include "keymaps.h"
#include "klib.h"
#include "io.h"
#include "atomics.h"
#include "cpu_calibrate.h"
#include "irqchip.h"
#include "scheduler.h"   /* g_global_tick, g_timer_frequency */
#include "touch.h"

/* Runtime repeat timing (calculated from ms based on timer frequency) */
uint32_t g_kb_repeat_delay_ticks;
uint32_t g_kb_repeat_rate_ticks;

/* ─── Circular character buffer ─────────────────────────────────────────── */

#define KEYBOARD_BUFFER_SIZE 256

static char keyboard_buffer[KEYBOARD_BUFFER_SIZE];
static volatile uint32_t kb_head = 0;
static volatile uint32_t kb_tail = 0;
static spinlock_t kb_lock = {0};

/* ─── Keyboard state ────────────────────────────────────────────────────── */

static keyboard_state_t kb_state = {0};

/* ─── Line discipline ───────────────────────────────────────────────────── */

static keyboard_line_state_t line_state = {0};
static spinlock_t line_lock = {0};
/* INVARIANT: readline_result is a singleton owned by the one thread that
 * calls keyboard_readline() (in BoxOS this is shell.bin / display.elf
 * stdin reader). If multiple threads ever read stdin concurrently the
 * pointer returned by keyboard_readline() will alias their data. Don't
 * call from more than one place. */
static char readline_result[KEYBOARD_LINE_BUFFER_SIZE];

/* ─── Touch tag handle cache (resolved once at keyboard_init) ──────────────
 *
 * keyboard_handle_scancode and keyboard_timer_tick both run in IRQ context
 * (PS/2 IRQ1 dispatch + PIT IRQ0 tick). TouchTagResolve is NOT IRQ-safe —
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

/* ─── Software key repeat state ─────────────────────────────────────────── */

typedef struct {
    uint8_t  active;                  /* 1 = a key is held down          */
    char     chars[KB_SEQ_MAX];       /* bytes to inject on repeat       */
    uint8_t  char_count;              /* 1 for normal, 3-4 for esc seqs  */
    uint8_t  held_key;               /* bare scancode of held key        */
    uint8_t  held_is_extended;       /* 1 if the held key was 0xE0-prefixed */
    uint64_t next_repeat_tick;       /* g_global_tick when next repeat fires */
    uint8_t  delay_passed;           /* 1 after initial delay elapsed    */
} KbRepeatState;

static KbRepeatState kb_repeat = {0};


/* ═══════════════════════════════════════════════════════════════════════════
   Internal helpers
   ═══════════════════════════════════════════════════════════════════════════ */

/* Push up to KB_SEQ_MAX bytes into the circular buffer.
   Called from IRQ context — spinlock save/restore handles IF. */
static void kb_push_chars(const char* chars, uint8_t count)
{
    spin_lock(&kb_lock);
    for (uint8_t i = 0; i < count; i++) {
        uint32_t next_head = (kb_head + 1) % KEYBOARD_BUFFER_SIZE;
        if (next_head == kb_tail) break;  /* buffer full — drop remainder */
        keyboard_buffer[kb_head] = chars[i];
        kb_head = next_head;
    }
    spin_unlock(&kb_lock);
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
static void kb_arm_repeat(const char* chars, uint8_t count,
                           uint8_t key, uint8_t is_extended)
{
    kb_repeat.active         = 1;
    kb_repeat.char_count     = count;
    kb_repeat.held_key       = key;
    kb_repeat.held_is_extended = is_extended;
    kb_repeat.delay_passed   = 0;
    for (uint8_t i = 0; i < count && i < KB_SEQ_MAX; i++)
        kb_repeat.chars[i] = chars[i];
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
    kb_head = 0;
    kb_tail = 0;
    memset(&kb_state, 0, sizeof(kb_state));
    memset(&kb_repeat, 0, sizeof(kb_repeat));
    kb_e0_pending = 0;
    kb_led_state  = 0;

    memset(&line_state, 0, sizeof(line_state));
    line_state.echo_enabled = 1;

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
     * The "keyboard" tag is a bare key (no value), so TouchTagResolve
     * fills only `bare`; `full` stays TOUCH_TAG_INVALID. That is still
     * correct for TouchPublishIrqPair which publishes to whichever id
     * is non-invalid. */
    TouchTag full = TOUCH_TAG_INVALID, bare = TOUCH_TAG_INVALID;
    TouchTagResolve("keyboard", &full, &bare);
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

        /* Look up extended key sequence */
        if (key < 128 && ext_key_table[key].len > 0) {
            const ExtKeySeq* ek = &ext_key_table[key];
            kb_push_chars(ek->seq, ek->len);
            kb_arm_repeat(ek->seq, ek->len, key, 1);
        }
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
        kb_push_chars(&ascii, 1);
        kb_arm_repeat(&ascii, 1, key, 0);

        struct { uint8_t scancode; char ascii; uint8_t mods; } kb_ev = {
            .scancode = key,
            .ascii    = ascii,
            .mods     = (uint8_t)(
                (__atomic_load_n(&kb_state.shift_pressed, __ATOMIC_RELAXED) ? 0x01 : 0) |
                (__atomic_load_n(&kb_state.ctrl_pressed,  __ATOMIC_RELAXED) ? 0x02 : 0) |
                (__atomic_load_n(&kb_state.alt_pressed,   __ATOMIC_RELAXED) ? 0x04 : 0)
            ),
        };
        /* IRQ context (PS/2 IRQ1 / xHCI HID IRQ): defer the publish via
         * the static-ring + irq_defer path. TouchPublish from here would
         * touch the TagFS registry lock and per-bucket spinlocks while
         * the CPU has IF=0, which is the deadlock pattern that
         * irq_defer was created to break (memory `irq_defer_done_2026_05_17`). */
        TouchTag full = __atomic_load_n(&g_kbd_touch_full, __ATOMIC_ACQUIRE);
        TouchTag bare = __atomic_load_n(&g_kbd_touch_bare, __ATOMIC_ACQUIRE);
        TouchPublishIrqPair(full, bare, &kb_ev, sizeof(kb_ev),
                            0, TOUCH_FLAG_KERNEL);
    }
}

/* ─── Software key repeat (called from PIT IRQ0 every tick) ────────────── */

void keyboard_timer_tick(void)
{
    if (!kb_repeat.active) return;

    uint64_t now = __atomic_load_n(&g_global_tick, __ATOMIC_RELAXED);
    if (now < kb_repeat.next_repeat_tick) return;

    /* Fire repeat */
    kb_push_chars(kb_repeat.chars, kb_repeat.char_count);

    if (kb_repeat.char_count > 0 && kb_repeat.chars[0] != 0) {
        struct { uint8_t scancode; char ascii; uint8_t mods; } kb_ev = {
            .scancode = kb_repeat.held_key,
            .ascii    = kb_repeat.chars[0],
            .mods     = 0,
        };
        /* PIT IRQ0 context — defer via static ring + irq_defer (same
         * rationale as the PS/2 IRQ1 site above). */
        TouchTag full = __atomic_load_n(&g_kbd_touch_full, __ATOMIC_ACQUIRE);
        TouchTag bare = __atomic_load_n(&g_kbd_touch_bare, __ATOMIC_ACQUIRE);
        TouchPublishIrqPair(full, bare, &kb_ev, sizeof(kb_ev),
                            0, TOUCH_FLAG_KERNEL);
    }

    /* After initial delay, switch to fast repeat rate */
    if (!kb_repeat.delay_passed) {
        kb_repeat.delay_passed = 1;
    }
    kb_repeat.next_repeat_tick = now + g_kb_repeat_rate_ticks;
}

/* ─── Push raw sequence (used by USB HID extended keys) ────────────────── */

void keyboard_push_sequence(const char* seq, uint8_t len)
{
    if (len > KB_SEQ_MAX) len = KB_SEQ_MAX;
    kb_push_chars(seq, len);
}

/* ═══════════════════════════════════════════════════════════════════════════
   Buffer access API (unchanged interface)
   ═══════════════════════════════════════════════════════════════════════════ */

/* Hint readers — head/tail are written under kb_lock by the IRQ-side
 * producer; user-context probing without the lock is fine on x86 (aligned
 * 32-bit loads are atomic) but expressed via __atomic_load_n to avoid the
 * formal C11 data race vs the IRQ-side stores. */
int keyboard_has_input(void)
{
    uint32_t h = __atomic_load_n(&kb_head, __ATOMIC_RELAXED);
    uint32_t t = __atomic_load_n(&kb_tail, __ATOMIC_RELAXED);
    return h != t;
}

uint32_t keyboard_available(void)
{
    uint32_t h = __atomic_load_n(&kb_head, __ATOMIC_RELAXED);
    uint32_t t = __atomic_load_n(&kb_tail, __ATOMIC_RELAXED);
    if (h == t) return 0;
    if (h > t) return h - t;
    return KEYBOARD_BUFFER_SIZE - t + h;
}

char keyboard_getchar(void)
{
    if (__atomic_load_n(&kb_head, __ATOMIC_RELAXED) ==
        __atomic_load_n(&kb_tail, __ATOMIC_RELAXED))
        return 0;

    spin_lock(&kb_lock);
    /* Re-check under lock: another consumer may have drained between the
     * fast-path probe and here. */
    if (kb_head == kb_tail) {
        spin_unlock(&kb_lock);
        return 0;
    }
    char c = keyboard_buffer[kb_tail];
    kb_tail = (kb_tail + 1) % KEYBOARD_BUFFER_SIZE;
    spin_unlock(&kb_lock);

    return c;
}

char keyboard_getchar_blocking(void)
{
    while (!keyboard_has_input()) {
        asm volatile("sti; hlt");
    }
    return keyboard_getchar();
}

void keyboard_flush(void)
{
    spin_lock(&kb_lock);
    kb_head = kb_tail = 0;
    spin_unlock(&kb_lock);
}

keyboard_state_t* keyboard_get_state(void)
{
    return &kb_state;
}

/* ═══════════════════════════════════════════════════════════════════════════
   Line discipline (unchanged logic)
   ═══════════════════════════════════════════════════════════════════════════ */

void keyboard_line_init(void)
{
    spin_lock(&line_lock);
    memset(&line_state, 0, sizeof(line_state));
    line_state.echo_enabled = 1;
    spin_unlock(&line_lock);
}

void keyboard_set_echo(bool enabled)
{
    spin_lock(&line_lock);
    line_state.echo_enabled = enabled ? 1 : 0;
    spin_unlock(&line_lock);
}

void keyboard_line_clear(void)
{
    spin_lock(&line_lock);
    line_state.length = 0;
    line_state.cursor = 0;
    line_state.line_ready = 0;
    line_state.ctrl_c_pressed = 0;
    memset(line_state.buffer, 0, KEYBOARD_LINE_BUFFER_SIZE);
    spin_unlock(&line_lock);
}

int keyboard_check_ctrl_c(void)
{
    spin_lock(&line_lock);
    int result = line_state.ctrl_c_pressed;
    line_state.ctrl_c_pressed = 0;
    spin_unlock(&line_lock);
    return result;
}

static void line_process_char(char c)
{
    spin_lock(&line_lock);

    /* Enter — complete line */
    if (c == '\n' || c == '\r') {
        line_state.buffer[line_state.length] = '\0';
        line_state.line_ready = 1;
        if (line_state.echo_enabled) {
            kprintf("\n");
        }
        spin_unlock(&line_lock);
        return;
    }

    /* Ctrl+C — atomic load: kb_state.ctrl_pressed is written from PS/2 IRQ
     * (BSP) and xHCI HID IRQ (any core), but read here from any user-thread
     * core. */
    if (__atomic_load_n(&kb_state.ctrl_pressed, __ATOMIC_RELAXED) &&
        (c == 'c' || c == 'C')) {
        line_state.ctrl_c_pressed = 1;
        line_state.length = 0;
        line_state.cursor = 0;
        if (line_state.echo_enabled) {
            kprintf("^C\n");
        }
        spin_unlock(&line_lock);
        return;
    }

    /* Backspace / DEL */
    if (c == '\b' || c == 127) {
        if (line_state.cursor > 0) {
            line_state.cursor--;
            line_state.length--;

            for (int i = line_state.cursor; i < line_state.length; i++) {
                line_state.buffer[i] = line_state.buffer[i + 1];
            }
            line_state.buffer[line_state.length] = '\0';

            if (line_state.echo_enabled) {
                kprintf("\b \b");
            }
        }
        spin_unlock(&line_lock);
        return;
    }

    /* Tab → space */
    if (c == '\t') {
        c = ' ';
    }

    /* Escape sequences pass through as individual bytes — the shell
       can decode them if it wants arrow key support later. For now
       non-printable bytes (like 0x1B) are silently dropped by the
       printable check below, which is fine. */

    /* Printable characters */
    if (c >= 32 && c < 127) {
        if (line_state.length < KEYBOARD_LINE_BUFFER_SIZE - 1) {
            line_state.buffer[line_state.cursor] = c;
            line_state.cursor++;
            line_state.length++;
            line_state.buffer[line_state.length] = '\0';

            if (line_state.echo_enabled) {
                kputchar(c);
            }
        }
    }

    spin_unlock(&line_lock);
}

char* keyboard_readline(void)
{
    keyboard_line_clear();

    while (1) {
        while (!keyboard_has_input()) {
            asm volatile("sti; hlt");
        }

        char c = keyboard_getchar();
        if (c == 0) continue;

        line_process_char(c);

        spin_lock(&line_lock);
        if (line_state.line_ready) {
            memcpy(readline_result, line_state.buffer, line_state.length + 1);
            spin_unlock(&line_lock);

            keyboard_line_clear();
            return readline_result;
        }

        if (line_state.ctrl_c_pressed) {
            spin_unlock(&line_lock);
            keyboard_line_clear();
            return NULL;
        }
        spin_unlock(&line_lock);
    }
}

int keyboard_readline_async(char* buf, int max)
{
    while (keyboard_has_input()) {
        char c = keyboard_getchar();
        if (c != 0) {
            line_process_char(c);
        }
    }

    spin_lock(&line_lock);
    if (line_state.line_ready) {
        int len = line_state.length;
        if (len >= max) len = max - 1;
        memcpy(buf, line_state.buffer, len);
        buf[len] = '\0';
        spin_unlock(&line_lock);

        keyboard_line_clear();
        return 1;
    }
    spin_unlock(&line_lock);

    return 0;
}
