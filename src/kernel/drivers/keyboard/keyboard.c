#include "keyboard.h"
#include "klib.h"
#include "io.h"
#include "atomics.h"
#include "cpu_calibrate.h"
#include "irqchip.h"

#define KEYBOARD_BUFFER_SIZE 256

static char keyboard_buffer[KEYBOARD_BUFFER_SIZE];
static volatile uint32_t kb_head = 0;
static volatile uint32_t kb_tail = 0;
static spinlock_t kb_lock = {0};

static keyboard_state_t kb_state = {0};

static keyboard_line_state_t line_state = {0};
static spinlock_t line_lock = {0};

static char readline_result[KEYBOARD_LINE_BUFFER_SIZE];

static const char scancode_to_ascii[] = {
    0,
    27,
    '1',
    '2',
    '3',
    '4',
    '5',
    '6',
    '7',
    '8',
    '9',
    '0',
    '-',
    '=',
    '\b',
    '\t',
    'q',
    'w',
    'e',
    'r',
    't',
    'y',
    'u',
    'i',
    'o',
    'p',
    '[',
    ']',
    '\n',
    0, // Ctrl
    'a',
    's',
    'd',
    'f',
    'g',
    'h',
    'j',
    'k',
    'l',
    ';',
    '\'',
    '`',
    0, // Left Shift
    '\\',
    'z',
    'x',
    'c',
    'v',
    'b',
    'n',
    'm',
    ',',
    '.',
    '/',
    0, // Right Shift
    '*',
    0,   // Alt
    ' ', // Space
    0,   // Caps Lock
};

static const char scancode_to_ascii_shifted[] = {
    0,
    27,
    '!',
    '@',
    '#',
    '$',
    '%',
    '^',
    '&',
    '*',
    '(',
    ')',
    '_',
    '+',
    '\b',
    '\t',
    'Q',
    'W',
    'E',
    'R',
    'T',
    'Y',
    'U',
    'I',
    'O',
    'P',
    '{',
    '}',
    '\n',
    0, // Ctrl
    'A',
    'S',
    'D',
    'F',
    'G',
    'H',
    'J',
    'K',
    'L',
    ':',
    '"',
    '~',
    0, // Left Shift
    '|',
    'Z',
    'X',
    'C',
    'V',
    'B',
    'N',
    'M',
    '<',
    '>',
    '?',
    0, // Right Shift
    '*',
    0,   // Alt
    ' ', // Space
    0,   // Caps Lock
};

// Wait for 8042 input buffer to be empty (ready to accept command)
// Timeout: ~50ms (PS/2 spec allows up to 20ms for command processing)
static void kb_wait_input_buffer(void)
{
    uint64_t deadline = rdtsc() + cpu_ms_to_tsc(50);
    while ((inb(KEYBOARD_STATUS_PORT) & 0x02) && rdtsc() < deadline) {
        __asm__ volatile("pause");
    }
}

// Wait for 8042 output buffer to have data (ready to read)
// Timeout: ~50ms
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

void keyboard_init(void)
{
    kb_head = 0;
    kb_tail = 0;
    kb_state.shift_pressed = 0;
    kb_state.ctrl_pressed = 0;
    kb_state.alt_pressed = 0;
    kb_state.caps_lock = 0;
    kb_state.num_lock = 0;
    kb_state.scroll_lock = 0;
    kb_state.last_keycode = 0;

    memset(&line_state, 0, sizeof(line_state));
    line_state.echo_enabled = 1;

    debug_printf("[KEYBOARD] Initializing 8042 PS/2 controller...\n");

    kb_wait_input_buffer();
    outb(KEYBOARD_STATUS_PORT, 0xAD);   // Disable keyboard

    kb_flush_output();

    kb_wait_input_buffer();
    outb(KEYBOARD_STATUS_PORT, 0x20);   // Read CCB

    if (!kb_wait_output_buffer()) {
        debug_printf("[KEYBOARD] ERROR: Timeout reading CCB\n");
        return;
    }

    uint8_t ccb = inb(KEYBOARD_DATA_PORT);

    ccb |= 0x01;   // Enable keyboard interrupt (IRQ1)
    ccb &= ~0x10;  // Clear "keyboard disabled" flag

    kb_wait_input_buffer();
    outb(KEYBOARD_STATUS_PORT, 0x60);   // Write CCB
    kb_wait_input_buffer();
    outb(KEYBOARD_DATA_PORT, ccb);

    kb_wait_input_buffer();
    outb(KEYBOARD_STATUS_PORT, 0xAE);   // Re-enable keyboard port

    kb_flush_output();

    irqchip_enable_irq(1);
    debug_printf("[KEYBOARD] IRQ1 enabled\n");

    kb_wait_input_buffer();
    outb(KEYBOARD_DATA_PORT, 0xFF);     // Reset command

    // Wait ~50ms for ACK (0xFA) and self-test result (0xAA) to arrive via IRQ1
    {
        uint64_t deadline = rdtsc() + cpu_ms_to_tsc(50);
        while (rdtsc() < deadline) {
            __asm__ volatile("pause");
        }
    }

    debug_printf("[KEYBOARD] 8042 initialization complete\n");
}

void keyboard_handle_scancode(uint8_t scancode)
{
    // Filter keyboard controller response codes
    switch (scancode) {
        case 0xFA:  // ACK
            return;
        case 0xFE:  // Resend
            return;
        case 0x00:  // Error/overrun
            return;
        case 0xFF:  // Error
            return;
        case 0xEE:  // Echo response
            return;
    }

    uint8_t is_release = scancode & 0x80;
    uint8_t key = scancode & 0x7F;

    if (key == 0x2A || key == 0x36)
    { // Left/Right Shift
        kb_state.shift_pressed = !is_release;
        return;
    }
    if (key == 0x1D)
    { // Ctrl
        kb_state.ctrl_pressed = !is_release;
        return;
    }
    if (key == 0x38)
    { // Alt
        kb_state.alt_pressed = !is_release;
        return;
    }
    if (key == 0x3A && !is_release)
    { // Caps Lock (toggle on press)
        kb_state.caps_lock = !kb_state.caps_lock;
        return;
    }

    if (is_release)
        return;

    char ascii = 0;
    if (key < sizeof(scancode_to_ascii))
    {
        if (kb_state.shift_pressed)
        {
            ascii = scancode_to_ascii_shifted[key];
        }
        else
        {
            ascii = scancode_to_ascii[key];
            if (kb_state.caps_lock && ascii >= 'a' && ascii <= 'z')
            {
                ascii -= 32;
            }
        }
    }

    if (ascii != 0)
    {
        spin_lock(&kb_lock);
        uint32_t next_head = (kb_head + 1) % KEYBOARD_BUFFER_SIZE;
        if (next_head != kb_tail)
        {
            keyboard_buffer[kb_head] = ascii;
            kb_head = next_head;
        }
        spin_unlock(&kb_lock);
    }
}

int keyboard_has_input(void)
{
    return kb_head != kb_tail;
}

uint32_t keyboard_available(void)
{
    if (kb_head == kb_tail)
    {
        return 0;
    }
    if (kb_head > kb_tail)
    {
        return kb_head - kb_tail;
    }
    return KEYBOARD_BUFFER_SIZE - kb_tail + kb_head;
}

char keyboard_getchar(void)
{
    if (kb_head == kb_tail)
    {
        return 0;
    }

    spin_lock(&kb_lock);
    char c = keyboard_buffer[kb_tail];
    kb_tail = (kb_tail + 1) % KEYBOARD_BUFFER_SIZE;
    spin_unlock(&kb_lock);

    return c;
}

char keyboard_getchar_blocking(void)
{
    while (!keyboard_has_input())
    {
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

keyboard_state_t *keyboard_get_state(void)
{
    return &kb_state;
}

static void line_process_char(char c)
{
    spin_lock(&line_lock);

    // Handle Enter before Ctrl+C check to avoid Enter being caught when Ctrl is held
    if (c == '\n' || c == '\r')
    {
        line_state.buffer[line_state.length] = '\0';
        line_state.line_ready = 1;
        if (line_state.echo_enabled)
        {
            kprintf("\n");
        }
        spin_unlock(&line_lock);
        return;
    }

    if (kb_state.ctrl_pressed && (c == 'c' || c == 'C'))
    {
        line_state.ctrl_c_pressed = 1;
        line_state.length = 0;
        line_state.cursor = 0;
        if (line_state.echo_enabled)
        {
            kprintf("^C\n");
        }
        spin_unlock(&line_lock);
        return;
    }

    if (c == '\b' || c == 127)
    {
        if (line_state.cursor > 0)
        {
            line_state.cursor--;
            line_state.length--;

            for (int i = line_state.cursor; i < line_state.length; i++)
            {
                line_state.buffer[i] = line_state.buffer[i + 1];
            }
            line_state.buffer[line_state.length] = '\0';

            if (line_state.echo_enabled)
            {
                kprintf("\b \b");
            }
        }
        spin_unlock(&line_lock);
        return;
    }

    if (c == '\t')
    {
        c = ' ';
    }

    if (c >= 32 && c < 127)
    {
        if (line_state.length < KEYBOARD_LINE_BUFFER_SIZE - 1)
        {
            line_state.buffer[line_state.cursor] = c;
            line_state.cursor++;
            line_state.length++;
            line_state.buffer[line_state.length] = '\0';

            if (line_state.echo_enabled)
            {
                kputchar(c);
            }
        }
    }

    spin_unlock(&line_lock);
}

char *keyboard_readline(void)
{
    keyboard_line_clear();

    while (1)
    {
        // Must enable interrupts before hlt: when called from syscall handler
        // IF is cleared by CPU, so keyboard IRQ can never fire without sti.
        while (!keyboard_has_input())
        {
            asm volatile("sti; hlt");
        }

        char c = keyboard_getchar();
        if (c == 0)
            continue;

        line_process_char(c);

        spin_lock(&line_lock);
        if (line_state.line_ready)
        {
            memcpy(readline_result, line_state.buffer, line_state.length + 1);
            spin_unlock(&line_lock);

            keyboard_line_clear();
            return readline_result;
        }

        if (line_state.ctrl_c_pressed)
        {
            spin_unlock(&line_lock);
            keyboard_line_clear();
            return NULL;
        }
        spin_unlock(&line_lock);
    }
}

int keyboard_readline_async(char *buf, int max)
{
    while (keyboard_has_input())
    {
        char c = keyboard_getchar();
        if (c != 0)
        {
            line_process_char(c);
        }
    }

    spin_lock(&line_lock);
    if (line_state.line_ready)
    {
        int len = line_state.length;
        if (len >= max)
            len = max - 1;
        memcpy(buf, line_state.buffer, len);
        buf[len] = '\0';
        spin_unlock(&line_lock);

        keyboard_line_clear();
        return 1;
    }
    spin_unlock(&line_lock);

    return 0;
}
