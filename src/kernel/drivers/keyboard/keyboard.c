#include "keyboard.h"
#include "keymaps.h"
#include "klib.h"
#include "io.h"
#include "atomics.h"
#include "cpu_calibrate.h"
#include "irqchip.h"
#include "scheduler.h"
#include "touch.h"
#include "logbook.h"
#include "kb_event.h"

uint32_t g_kb_repeat_delay_ticks;
uint32_t g_kb_repeat_rate_ticks;




static keyboard_state_t kb_state = {0};


static volatile uint16_t g_kbd_touch_full = TOUCH_TAG_INVALID;
static volatile uint16_t g_kbd_touch_bare = TOUCH_TAG_INVALID;


static volatile uint8_t kb_e0_pending = 0;


static uint8_t kb_led_state = 0;


static uint8_t kb_mods_now(void)
{
    return (uint8_t)(
        (__atomic_load_n(&kb_state.shift_pressed, __ATOMIC_RELAXED) ? KB_MOD_SHIFT : 0) |
        (__atomic_load_n(&kb_state.ctrl_pressed,  __ATOMIC_RELAXED) ? KB_MOD_CTRL  : 0) |
        (__atomic_load_n(&kb_state.alt_pressed,   __ATOMIC_RELAXED) ? KB_MOD_ALT   : 0));
}

static void kb_publish_event(const kb_event_t *ev)
{
    TouchTag full = __atomic_load_n(&g_kbd_touch_full, __ATOMIC_ACQUIRE);
    TouchTag bare = __atomic_load_n(&g_kbd_touch_bare, __ATOMIC_ACQUIRE);
    TouchPublishIrqPair(full, bare, ev, sizeof(*ev), 0, TOUCH_FLAG_KERNEL);
}


typedef struct {
    uint8_t  active;
    uint8_t  held_key;
    uint8_t  held_is_extended;
    kb_event_t held_event;
    uint64_t next_repeat_tick;
    uint8_t  delay_passed;
} KbRepeatState;

static KbRepeatState kb_repeat = {0};

static spinlock_t kb_repeat_lock;



void keyboard_inject(const char *chars, uint32_t count)
{
    for (uint32_t i = 0; i < count; i++) {
        kb_event_t kb_ev = { .scancode = 0, .ascii = chars[i], .mods = 0 };
        kb_publish_event(&kb_ev);
    }
}

static char translate_key(uint8_t key)
{
    if (key >= sizeof(scancode_to_ascii)) return 0;

    char base    = scancode_to_ascii[key];
    char shifted = scancode_to_ascii_shifted[key];
    int  is_alpha = (base >= 'a' && base <= 'z');

    uint8_t shift = __atomic_load_n(&kb_state.shift_pressed, __ATOMIC_RELAXED);
    if (is_alpha) {
        uint8_t caps = __atomic_load_n(&kb_state.caps_lock, __ATOMIC_RELAXED);
        return (caps ^ shift) ? shifted : base;
    }

    return shift ? shifted : base;
}

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
    kb_repeat.active           = 1;
    spin_unlock(&kb_repeat_lock);
}

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

static void kb_forget_repeat(void)
{
    spin_lock(&kb_repeat_lock);
    kb_repeat.active = 0;
    spin_unlock(&kb_repeat_lock);
}


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
    spinlock_init(&kb_repeat_lock);
    memset(&kb_repeat, 0, sizeof(kb_repeat));
    kb_e0_pending = 0;
    kb_led_state  = 0;

    debug_printf("[KEYBOARD] Initializing 8042 PS/2 controller...\n");

    kb_wait_input_buffer();
    outb(KEYBOARD_STATUS_PORT, 0xAD);

    kb_flush_output();

    kb_wait_input_buffer();
    outb(KEYBOARD_STATUS_PORT, 0x20);

    if (!kb_wait_output_buffer()) {
        debug_printf("[KEYBOARD] ERROR: Timeout reading CCB\n");
        return;
    }

    uint8_t ccb = inb(KEYBOARD_DATA_PORT);
    ccb |= 0x01;
    ccb &= ~0x10;

    kb_wait_input_buffer();
    outb(KEYBOARD_STATUS_PORT, 0x60);
    kb_wait_input_buffer();
    outb(KEYBOARD_DATA_PORT, ccb);

    kb_wait_input_buffer();
    outb(KEYBOARD_STATUS_PORT, 0xAE);

    kb_flush_output();

    irqchip_enable_irq(1);
    debug_printf("[KEYBOARD] IRQ1 enabled\n");

    kb_wait_input_buffer();
    outb(KEYBOARD_DATA_PORT, 0xFF);

    {
        uint64_t deadline = rdtsc() + cpu_ms_to_tsc(50);
        while (rdtsc() < deadline) {
            __asm__ volatile("pause");
        }
    }

    keyboard_set_leds(0, 0, 0);

    g_kb_repeat_delay_ticks = (KB_REPEAT_DELAY_MS * g_timer_frequency) / 1000;
    g_kb_repeat_rate_ticks  = (KB_REPEAT_RATE_MS * g_timer_frequency) / 1000;

    debug_printf("[KEYBOARD] 8042 initialization complete\n");
    debug_printf("[KEYBOARD] Repeat: delay=%u ticks (%u ms), rate=%u ticks (%u ms)\n",
                 g_kb_repeat_delay_ticks, KB_REPEAT_DELAY_MS,
                 g_kb_repeat_rate_ticks, KB_REPEAT_RATE_MS);

    TouchTag full = TOUCH_TAG_INVALID, bare = TOUCH_TAG_INVALID;
    TouchLogbookResolve("keyboard", &full, &bare);
    __atomic_store_n(&g_kbd_touch_full, full, __ATOMIC_RELEASE);
    __atomic_store_n(&g_kbd_touch_bare, bare, __ATOMIC_RELEASE);
    debug_printf("[KEYBOARD] Touch tag handles: full=%u bare=%u\n",
                 (unsigned)full, (unsigned)bare);
}


void keyboard_handle_scancode(uint8_t scancode)
{
    if (scancode == 0xE0) {
        kb_e0_pending = 1;
        return;
    }

    switch (scancode) {
        case 0xFA: return;
        case 0xFE: return;
        case 0xEE: return;
        case 0x00:
        case 0xFF:
            kb_e0_pending = 0;
            kb_forget_repeat();
            __atomic_store_n(&kb_state.shift_pressed, 0, __ATOMIC_RELAXED);
            __atomic_store_n(&kb_state.ctrl_pressed,  0, __ATOMIC_RELAXED);
            __atomic_store_n(&kb_state.alt_pressed,   0, __ATOMIC_RELAXED);
            return;
    }

    uint8_t is_release = scancode & 0x80;
    uint8_t key = scancode & 0x7F;

    if (kb_e0_pending) {
        kb_e0_pending = 0;

        if (key == 0x1D) {
            __atomic_store_n(&kb_state.ctrl_pressed, !is_release, __ATOMIC_RELAXED);
            return;
        }
        if (key == 0x38) {
            __atomic_store_n(&kb_state.alt_pressed, !is_release, __ATOMIC_RELAXED);
            return;
        }

        if (is_release) {
            kb_release_repeat(key, 1);
            return;
        }

        kb_event_t kb_ev = { .scancode = key, .ascii = 0,
                             .mods = (uint8_t)(kb_mods_now() | KB_MOD_EXTENDED) };
        kb_arm_repeat(key, 1, &kb_ev);
        kb_publish_event(&kb_ev);
        return;
    }


    if (key == 0x2A || key == 0x36) {
        __atomic_store_n(&kb_state.shift_pressed, !is_release, __ATOMIC_RELAXED);
        return;
    }
    if (key == 0x1D) {
        __atomic_store_n(&kb_state.ctrl_pressed, !is_release, __ATOMIC_RELAXED);
        return;
    }
    if (key == 0x38) {
        __atomic_store_n(&kb_state.alt_pressed, !is_release, __ATOMIC_RELAXED);
        return;
    }

    if (key == 0x3A && !is_release) {
        uint8_t caps = !__atomic_load_n(&kb_state.caps_lock, __ATOMIC_RELAXED);
        __atomic_store_n(&kb_state.caps_lock, caps, __ATOMIC_RELAXED);
        keyboard_set_leds(caps,
                          __atomic_load_n(&kb_state.num_lock,    __ATOMIC_RELAXED),
                          __atomic_load_n(&kb_state.scroll_lock, __ATOMIC_RELAXED));
        return;
    }
    if (key == 0x45 && !is_release) {
        uint8_t num = !__atomic_load_n(&kb_state.num_lock, __ATOMIC_RELAXED);
        __atomic_store_n(&kb_state.num_lock, num, __ATOMIC_RELAXED);
        keyboard_set_leds(__atomic_load_n(&kb_state.caps_lock,   __ATOMIC_RELAXED),
                          num,
                          __atomic_load_n(&kb_state.scroll_lock, __ATOMIC_RELAXED));
        return;
    }
    if (key == 0x46 && !is_release) {
        uint8_t scroll = !__atomic_load_n(&kb_state.scroll_lock, __ATOMIC_RELAXED);
        __atomic_store_n(&kb_state.scroll_lock, scroll, __ATOMIC_RELAXED);
        keyboard_set_leds(__atomic_load_n(&kb_state.caps_lock, __ATOMIC_RELAXED),
                          __atomic_load_n(&kb_state.num_lock,  __ATOMIC_RELAXED),
                          scroll);
        return;
    }

    if (is_release) {
        kb_release_repeat(key, 0);
        return;
    }

    __atomic_store_n(&kb_state.last_keycode, key, __ATOMIC_RELAXED);
    char ascii = translate_key(key);

    if (ascii != 0) {
        kb_event_t kb_ev = { .scancode = key, .ascii = ascii, .mods = kb_mods_now() };
        kb_arm_repeat(key, 0, &kb_ev);
        kb_publish_event(&kb_ev);
    }
}


void keyboard_timer_tick(void)
{
    uint64_t   now = __atomic_load_n(&g_global_tick, __ATOMIC_RELAXED);
    kb_event_t say;
    bool       due = false;

    spin_lock(&kb_repeat_lock);
    if (kb_repeat.active && now >= kb_repeat.next_repeat_tick) {
        say = kb_repeat.held_event;
        due = true;
        kb_repeat.delay_passed     = 1;
        kb_repeat.next_repeat_tick = now + g_kb_repeat_rate_ticks;
    }
    spin_unlock(&kb_repeat_lock);

    if (due) kb_publish_event(&say);
}

keyboard_state_t* keyboard_get_state(void)
{
    return &kb_state;
}
