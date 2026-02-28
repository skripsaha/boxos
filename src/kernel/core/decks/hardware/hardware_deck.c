#include "hardware_deck.h"
#include "klib.h"
#include "io.h"
#include "pit.h"
#include "rtc.h"
#include "pic.h"
#include "process.h"
#include "ata.h"
#include "keyboard.h"
#include "vga.h"
#include "serial.h"
#include "xhci.h"
#include "xhci_port.h"
#include "xhci_enumeration.h"
#include "acpi.h"
#include "tagfs.h"

// Forbidden ports (blocked even for system tag)
static const uint16_t forbidden_ports[] = {
    0x1F0, 0x1F1, 0x1F2, 0x1F3, 0x1F4, 0x1F5, 0x1F6, 0x1F7,  // ATA Primary
    0x170, 0x171, 0x172, 0x173, 0x174, 0x175, 0x176, 0x177,  // ATA Secondary
    0xCF8, 0xCFC,  // PCI config
    0xCF9,         // Reset control
    0x70, 0x71,    // CMOS/RTC
};

// Safe ports for non-system processes
static const uint16_t safe_read_ports[] = {
    0x80,   // POST diagnostic
    0x60,   // Keyboard data
    0x64,   // Keyboard status
    0x3F8,  // COM1 data
    0x3FD,  // COM1 line status
};

static const uint16_t safe_write_ports[] = {
    0x80,   // POST diagnostic
    0x3F8,  // COM1 data
};

static const size_t n_forbidden = sizeof(forbidden_ports) / sizeof(forbidden_ports[0]);
static const size_t n_safe_read = sizeof(safe_read_ports) / sizeof(safe_read_ports[0]);
static const size_t n_safe_write = sizeof(safe_write_ports) / sizeof(safe_write_ports[0]);

static uint16_t read_u16(const uint8_t* data) {
    return ((uint16_t)data[0] << 8) | data[1];
}

static uint32_t read_u32(const uint8_t* data) {
    return ((uint32_t)data[0] << 24) | ((uint32_t)data[1] << 16) |
           ((uint32_t)data[2] << 8) | data[3];
}

static __attribute__((unused)) uint64_t read_u64(const uint8_t* data) {
    return ((uint64_t)data[0] << 56) | ((uint64_t)data[1] << 48) |
           ((uint64_t)data[2] << 40) | ((uint64_t)data[3] << 32) |
           ((uint64_t)data[4] << 24) | ((uint64_t)data[5] << 16) |
           ((uint64_t)data[6] << 8) | data[7];
}

static void write_u16(uint8_t* data, uint16_t value) {
    data[0] = (value >> 8) & 0xFF;
    data[1] = value & 0xFF;
}

static void write_u32(uint8_t* data, uint32_t value) {
    data[0] = (value >> 24) & 0xFF;
    data[1] = (value >> 16) & 0xFF;
    data[2] = (value >> 8) & 0xFF;
    data[3] = value & 0xFF;
}

static void write_u64(uint8_t* data, uint64_t value) {
    data[0] = (value >> 56) & 0xFF;
    data[1] = (value >> 48) & 0xFF;
    data[2] = (value >> 40) & 0xFF;
    data[3] = (value >> 32) & 0xFF;
    data[4] = (value >> 24) & 0xFF;
    data[5] = (value >> 16) & 0xFF;
    data[6] = (value >> 8) & 0xFF;
    data[7] = value & 0xFF;
}

static bool hw_port_is_safe_read(uint16_t port) {
    for (size_t i = 0; i < n_safe_read; i++) {
        if (safe_read_ports[i] == port) {
            return true;
        }
    }
    return false;
}

static bool hw_port_is_safe_write(uint16_t port) {
    for (size_t i = 0; i < n_safe_write; i++) {
        if (safe_write_ports[i] == port) {
            return true;
        }
    }
    return false;
}

static bool hw_port_is_forbidden(uint16_t port) {
    for (size_t i = 0; i < n_forbidden; i++) {
        if (forbidden_ports[i] == port) {
            return true;
        }
    }
    return false;
}

static bool hw_irq_is_valid(uint8_t irq) {
    return (irq < 16 && irq != 0 && irq != 2);
}

static bool check_keyboard_access(uint32_t pid) {
    process_t* proc = process_find(pid);
    if (!proc) {
        return false;
    }

    return process_has_tag(proc, "hw_keyboard") ||
           process_has_tag(proc, "system");
}

static bool check_vga_access(uint32_t pid) {
    process_t* proc = process_find(pid);
    if (!proc) {
        return false;
    }

    return process_has_tag(proc, "hw_vga") ||
           process_has_tag(proc, "system");
}

static bool check_power_access(uint32_t pid) {
    process_t* proc = process_find(pid);
    if (!proc) {
        return false;
    }

    return process_has_tag(proc, "system") || process_has_tag(proc, "hw_power");
}

static int do_reboot(Event* event) {
    debug_printf("[Hardware] SYSTEM_REBOOT request from PID %u\n", event->pid);

    if (!check_power_access(event->pid)) {
        debug_printf("[Hardware] ERROR: PID %u lacks permission for reboot\n", event->pid);
        event->data[0] = HW_ERR_ACCESS_DENIED;
        event->state = EVENT_STATE_ACCESS_DENIED;
        return -5;
    }

    debug_printf("[Hardware] Rebooting system...\n");

    // Keyboard controller reset method (triple fault)
    outb(0x64, 0xFE);

    // Should never reach here
    while (1) {
        asm volatile("cli; hlt");
    }

    return 0;
}

static int do_shutdown(Event* event) {
    debug_printf("[Hardware] SYSTEM_SHUTDOWN request from PID %u\n", event->pid);

    if (!check_power_access(event->pid)) {
        debug_printf("[Hardware] ERROR: PID %u lacks permission for shutdown\n", event->pid);
        event->data[0] = HW_ERR_ACCESS_DENIED;
        event->state = EVENT_STATE_ACCESS_DENIED;
        return -5;
    }

    debug_printf("[Hardware] Shutting down system...\n");

    process_cleanup_queue_flush();

    tagfs_sync();

    ata_flush_cache(1);

    acpi_shutdown();

    return 0;
}

int hardware_deck_handler(Event* event) {
    if (!event) {
        return -1;
    }

    uint8_t opcode = event_get_opcode(event, event->current_prefix_idx);

    switch (opcode) {
        case HW_TIMER_GET_TICKS: {
            uint64_t ticks = pit_get_ticks();
            write_u64(event->data, ticks);
            event->state = EVENT_STATE_COMPLETED;
            return 0;
        }

        case HW_TIMER_GET_MS: {
            uint64_t ticks = pit_get_ticks();
            uint32_t freq = pit_get_frequency();

            if (freq == 0) {
                event->state = EVENT_STATE_ERROR;
                return -1;
            }

            uint64_t ms = (ticks * 1000) / freq;
            write_u64(event->data, ms);
            event->state = EVENT_STATE_COMPLETED;
            return 0;
        }

        case HW_TIMER_GET_FREQ: {
            uint32_t freq = pit_get_frequency();
            write_u32(event->data, freq);
            event->state = EVENT_STATE_COMPLETED;
            return 0;
        }

        case HW_TIMER_SCHEDULE: {
            event->state = EVENT_STATE_ERROR;
            return -1;
        }

        case HW_TIMER_CANCEL: {
            event->state = EVENT_STATE_ERROR;
            return -1;
        }

        case HW_RTC_GET_TIME: {
            time_t t;
            rtc_get_boxtime(&t);
            write_u64(event->data + 0, t.seconds);
            write_u32(event->data + 8, t.nanosec);
            write_u16(event->data + 12, t.year);
            event->data[14] = t.month;
            event->data[15] = t.day;
            event->data[16] = t.hour;
            event->data[17] = t.minute;
            event->data[18] = t.second;
            event->data[19] = t.weekday;
            event->state = EVENT_STATE_COMPLETED;
            return 0;
        }

        case HW_RTC_GET_UNIX64: {
            uint64_t secs = rtc_get_unix64();
            write_u64(event->data, secs);
            event->state = EVENT_STATE_COMPLETED;
            return 0;
        }

        case HW_RTC_GET_UPTIME: {
            uint64_t ns = rtc_get_uptime_ns();
            write_u64(event->data, ns);
            event->state = EVENT_STATE_COMPLETED;
            return 0;
        }

        case HW_PORT_INB: {
            uint16_t port = read_u16(event->data);

            if (hw_port_is_forbidden(port)) {
                debug_printf("[HW_DECK] FORBIDDEN port 0x%04x blocked (INB)\n", port);
                event->state = EVENT_STATE_ACCESS_DENIED;
                return -5;
            }

            process_t* proc = process_find(event->pid);
            if (proc) {
                process_ref_inc(proc);

                if (!process_has_tag(proc, "system")) {
                    if (!hw_port_is_safe_read(port)) {
                        debug_printf("[HW_DECK] Port 0x%04x requires system tag (INB)\n", port);
                    event->state = EVENT_STATE_ACCESS_DENIED;
                    process_ref_dec(proc);
                    return -5;
                    }
                }

                process_ref_dec(proc);
            }

            uint8_t value = inb(port);
            event->data[0] = value;
            event->state = EVENT_STATE_COMPLETED;
            return 0;
        }

        case HW_PORT_OUTB: {
            uint16_t port = read_u16(event->data);
            uint8_t value = event->data[2];

            if (hw_port_is_forbidden(port)) {
                debug_printf("[HW_DECK] FORBIDDEN port 0x%04x blocked (OUTB)\n", port);
                event->state = EVENT_STATE_ACCESS_DENIED;
                return -5;
            }

            process_t* proc = process_find(event->pid);
            if (proc) {
                process_ref_inc(proc);

                if (!process_has_tag(proc, "system")) {
                    if (!hw_port_is_safe_write(port)) {
                        debug_printf("[HW_DECK] Port 0x%04x requires system tag (OUTB)\n", port);
                    event->state = EVENT_STATE_ACCESS_DENIED;
                    process_ref_dec(proc);
                    return -5;
                    }
                }

                process_ref_dec(proc);
            }

            outb(port, value);
            event->data[0] = 0;
            event->state = EVENT_STATE_COMPLETED;
            return 0;
        }

        case HW_PORT_INW: {
            uint16_t port = read_u16(event->data);

            if (hw_port_is_forbidden(port)) {
                debug_printf("[HW_DECK] FORBIDDEN port 0x%04x blocked (INW)\n", port);
                event->state = EVENT_STATE_ACCESS_DENIED;
                return -5;
            }

            process_t* proc = process_find(event->pid);
            if (proc) {
                process_ref_inc(proc);

                if (!process_has_tag(proc, "system")) {
                    if (!hw_port_is_safe_read(port)) {
                        debug_printf("[HW_DECK] Port 0x%04x requires system tag (INW)\n", port);
                    event->state = EVENT_STATE_ACCESS_DENIED;
                    process_ref_dec(proc);
                    return -5;
                    }
                }

                process_ref_dec(proc);
            }

            uint16_t value = inw(port);
            write_u16(event->data, value);
            event->state = EVENT_STATE_COMPLETED;
            return 0;
        }

        case HW_PORT_OUTW: {
            uint16_t port = read_u16(event->data);
            uint16_t value = read_u16(event->data + 2);

            if (hw_port_is_forbidden(port)) {
                debug_printf("[HW_DECK] FORBIDDEN port 0x%04x blocked (OUTW)\n", port);
                event->state = EVENT_STATE_ACCESS_DENIED;
                return -5;
            }

            process_t* proc = process_find(event->pid);
            if (proc) {
                process_ref_inc(proc);

                if (!process_has_tag(proc, "system")) {
                    if (!hw_port_is_safe_write(port)) {
                        debug_printf("[HW_DECK] Port 0x%04x requires system tag (OUTW)\n", port);
                    event->state = EVENT_STATE_ACCESS_DENIED;
                    process_ref_dec(proc);
                    return -5;
                    }
                }

                process_ref_dec(proc);
            }

            outw(port, value);
            event->data[0] = 0;
            event->state = EVENT_STATE_COMPLETED;
            return 0;
        }

        case HW_PORT_INL: {
            uint16_t port = read_u16(event->data);

            if (hw_port_is_forbidden(port)) {
                debug_printf("[HW_DECK] FORBIDDEN port 0x%04x blocked (INL)\n", port);
                event->state = EVENT_STATE_ACCESS_DENIED;
                return -5;
            }

            process_t* proc = process_find(event->pid);
            if (proc) {
                process_ref_inc(proc);

                if (!process_has_tag(proc, "system")) {
                    if (!hw_port_is_safe_read(port)) {
                        debug_printf("[HW_DECK] Port 0x%04x requires system tag (INL)\n", port);
                    event->state = EVENT_STATE_ACCESS_DENIED;
                    process_ref_dec(proc);
                    return -5;
                    }
                }

                process_ref_dec(proc);
            }

            uint32_t value = inl(port);
            write_u32(event->data, value);
            event->state = EVENT_STATE_COMPLETED;
            return 0;
        }

        case HW_PORT_OUTL: {
            uint16_t port = read_u16(event->data);
            uint32_t value = read_u32(event->data + 2);

            if (hw_port_is_forbidden(port)) {
                debug_printf("[HW_DECK] FORBIDDEN port 0x%04x blocked (OUTL)\n", port);
                event->state = EVENT_STATE_ACCESS_DENIED;
                return -5;
            }

            process_t* proc = process_find(event->pid);
            if (proc) {
                process_ref_inc(proc);

                if (!process_has_tag(proc, "system")) {
                    if (!hw_port_is_safe_write(port)) {
                        debug_printf("[HW_DECK] Port 0x%04x requires system tag (OUTL)\n", port);
                    event->state = EVENT_STATE_ACCESS_DENIED;
                    process_ref_dec(proc);
                    return -5;
                    }
                }

                process_ref_dec(proc);
            }

            outl(port, value);
            event->data[0] = 0;
            event->state = EVENT_STATE_COMPLETED;
            return 0;
        }

        case HW_IRQ_ENABLE: {
            uint8_t irq = event->data[0];
            if (!hw_irq_is_valid(irq)) {
                event->state = EVENT_STATE_ACCESS_DENIED;
                return -5;
            }
            pic_enable_irq(irq);
            event->data[0] = 0;
            event->state = EVENT_STATE_COMPLETED;
            return 0;
        }

        case HW_IRQ_DISABLE: {
            uint8_t irq = event->data[0];
            if (!hw_irq_is_valid(irq)) {
                event->state = EVENT_STATE_ACCESS_DENIED;
                return -5;
            }
            pic_disable_irq(irq);
            event->data[0] = 0;
            event->state = EVENT_STATE_COMPLETED;
            return 0;
        }

        case HW_IRQ_GET_ISR: {
            uint16_t isr = pic_get_isr();
            write_u16(event->data, isr);
            event->state = EVENT_STATE_COMPLETED;
            return 0;
        }

        case HW_IRQ_GET_IRR: {
            uint16_t irr = pic_get_irr();
            write_u16(event->data, irr);
            event->state = EVENT_STATE_COMPLETED;
            return 0;
        }

        case HW_IRQ_REGISTER: {
            event->state = EVENT_STATE_ERROR;
            return -1;
        }

        case HW_IRQ_UNREGISTER: {
            event->state = EVENT_STATE_ERROR;
            return -1;
        }

        case HW_IRQ_SEND_EOI: {
            uint8_t irq = event->data[0];
            if (!hw_irq_is_valid(irq)) {
                event->state = EVENT_STATE_ACCESS_DENIED;
                return -5;
            }
            pic_send_eoi(irq);
            event->data[0] = 0;
            event->state = EVENT_STATE_COMPLETED;
            return 0;
        }

        case HW_CPU_HALT: {
            hlt();
            event->data[0] = 0;
            event->state = EVENT_STATE_COMPLETED;
            return 0;
        }

        case HW_DISK_INFO: {
            uint8_t is_master = event->data[0];
            ATADevice* device = is_master ? &ata_primary_master : &ata_primary_slave;

            // Pack device info into event.data
            event->data[0] = device->exists;
            memcpy(&event->data[1], device->model, 40);
            memcpy(&event->data[41], device->serial, 20);

            // Big-endian encoding for portability
            event->data[61] = (device->total_sectors >> 24) & 0xFF;
            event->data[62] = (device->total_sectors >> 16) & 0xFF;
            event->data[63] = (device->total_sectors >> 8) & 0xFF;
            event->data[64] = device->total_sectors & 0xFF;

            uint64_t size_mb = device->size_mb;
            event->data[65] = (size_mb >> 56) & 0xFF;
            event->data[66] = (size_mb >> 48) & 0xFF;
            event->data[67] = (size_mb >> 40) & 0xFF;
            event->data[68] = (size_mb >> 32) & 0xFF;
            event->data[69] = (size_mb >> 24) & 0xFF;
            event->data[70] = (size_mb >> 16) & 0xFF;
            event->data[71] = (size_mb >> 8) & 0xFF;
            event->data[72] = size_mb & 0xFF;

            event->state = EVENT_STATE_COMPLETED;
            return 0;
        }

        case HW_DISK_FLUSH: {
            uint8_t is_master = event->data[0];
            int result = ata_flush_cache(is_master);
            event->data[0] = (result == 0) ? 0 : 1;
            event->state = EVENT_STATE_COMPLETED;
            return 0;
        }

        case HW_KEYBOARD_GETCHAR: {
            if (!check_keyboard_access(event->pid)) {
                event->data[0] = 0;
                event->data[1] = 0;
                event->data[2] = 0;
                event->data[3] = HW_KB_ACCESS_DENIED;
                event->state = EVENT_STATE_ACCESS_DENIED;
                return -5;
            }

            if (!keyboard_has_input()) {
                event->data[0] = 0;
                event->data[1] = 0;
                event->data[2] = 0;
                event->data[3] = HW_KB_NO_DATA;
                event->state = EVENT_STATE_COMPLETED;
                return 0;
            }

            char ch = keyboard_getchar();
            keyboard_state_t* kb_state = keyboard_get_state();

            event->data[0] = (uint8_t)ch;
            event->data[1] = kb_state->last_keycode;

            uint8_t modifiers = 0;
            if (kb_state->shift_pressed) modifiers |= 0x01;
            if (kb_state->ctrl_pressed)  modifiers |= 0x02;
            if (kb_state->alt_pressed)   modifiers |= 0x04;
            event->data[2] = modifiers;

            event->data[3] = HW_KB_SUCCESS;
            event->state = EVENT_STATE_COMPLETED;

            return 0;
        }

        case HW_KEYBOARD_READLINE: {
            if (!check_keyboard_access(event->pid)) {
                memset(event->data, 0, EVENT_DATA_SIZE);
                event->data[EVENT_DATA_SIZE - 1] = HW_KB_ACCESS_DENIED;
                event->state = EVENT_STATE_ACCESS_DENIED;
                return -5;
            }

            uint8_t max_length = event->data[0];
            uint8_t echo_mode = event->data[1];

            if (max_length == 0 || max_length > 187) {
                max_length = 187;
            }

            keyboard_set_echo(echo_mode != 0);

            char line_buffer[188];
            memset(line_buffer, 0, sizeof(line_buffer));

            int line_ready = keyboard_readline_async(line_buffer, max_length);

            if (line_ready) {
                size_t len = strlen(line_buffer);

                event->data[0] = (len >> 24) & 0xFF;
                event->data[1] = (len >> 16) & 0xFF;
                event->data[2] = (len >> 8) & 0xFF;
                event->data[3] = len & 0xFF;

                size_t copy_len = len < 187 ? len : 187;
                memcpy(&event->data[4], line_buffer, copy_len);
                event->data[4 + copy_len] = '\0';

                event->data[EVENT_DATA_SIZE - 1] = HW_KB_SUCCESS;
                event->state = EVENT_STATE_COMPLETED;

                return 0;
            } else {
                event->data[0] = 0;
                event->data[1] = 0;
                event->data[2] = 0;
                event->data[3] = 0;
                event->data[EVENT_DATA_SIZE - 1] = HW_KB_WOULD_BLOCK;
                event->first_error = BOXOS_ERR_WOULD_BLOCK;
                event->state = EVENT_STATE_RETRY;

                return 0;
            }
        }

        case HW_KEYBOARD_STATUS: {
            if (!check_keyboard_access(event->pid)) {
                memset(event->data, 0, 192);
                event->state = EVENT_STATE_ACCESS_DENIED;
                return -5;
            }

            uint32_t available = keyboard_available();

            event->data[0] = (available >> 24) & 0xFF;
            event->data[1] = (available >> 16) & 0xFF;
            event->data[2] = (available >> 8) & 0xFF;
            event->data[3] = available & 0xFF;

            uint32_t buffer_size = 256;
            event->data[4] = (buffer_size >> 24) & 0xFF;
            event->data[5] = (buffer_size >> 16) & 0xFF;
            event->data[6] = (buffer_size >> 8) & 0xFF;
            event->data[7] = buffer_size & 0xFF;

            event->state = EVENT_STATE_COMPLETED;

            return 0;
        }

        case HW_VGA_PUTCHAR: {
            if (!check_vga_access(event->pid)) {
                event->data[0] = VGA_ERR_ACCESS_DENIED;
                event->state = EVENT_STATE_ACCESS_DENIED;
                return -5;
            }

            uint8_t row = event->data[0];
            uint8_t col = event->data[1];
            uint8_t character = event->data[2];
            uint8_t color = event->data[3];

            if (row >= VGA_HEIGHT || col >= VGA_WIDTH) {
                event->data[0] = VGA_ERR_OUT_OF_BOUNDS;
                event->state = EVENT_STATE_ERROR;
                return -1;
            }

            uint8_t old_x = vga_get_cursor_position_x();
            uint8_t old_y = vga_get_cursor_position_y();
            uint8_t old_color = vga_get_color();

            vga_set_cursor_position(col, row);
            vga_set_color(color);
            vga_print_char(character, color);

            vga_set_cursor_position(old_x, old_y);
            vga_set_color(old_color);

            event->data[0] = VGA_SUCCESS;
            event->state = EVENT_STATE_COMPLETED;
            return 0;
        }

        case HW_VGA_PUTSTRING: {
            if (!check_vga_access(event->pid)) {
                event->data[0] = VGA_ERR_ACCESS_DENIED;
                event->state = EVENT_STATE_ACCESS_DENIED;
                return -5;
            }

            uint8_t length = event->data[0];
            uint8_t color = event->data[1];
            uint8_t flags = event->data[2];

            if (length > 185) {
                event->data[0] = VGA_ERR_STRING_TOO_LONG;
                event->state = EVENT_STATE_ERROR;
                return -1;
            }

            uint8_t old_color = vga_get_color();
            vga_set_color(color);

            uint8_t chars_written = 0;
            for (uint8_t i = 0; i < length && event->data[4 + i] != '\0'; i++) {
                char ch = event->data[4 + i];
                vga_print_char(ch, color);
                serial_putchar(ch);
                chars_written++;
            }

            if (!(flags & 0x02)) {
                vga_set_color(old_color);
            }

            uint8_t final_row = vga_get_cursor_position_y();
            uint8_t final_col = vga_get_cursor_position_x();

            event->data[0] = VGA_SUCCESS;
            event->data[1] = chars_written;
            event->data[2] = final_row;
            event->data[3] = final_col;
            event->state = EVENT_STATE_COMPLETED;
            return 0;
        }

        case HW_VGA_CLEAR_SCREEN: {
            if (!check_vga_access(event->pid)) {
                event->data[0] = VGA_ERR_ACCESS_DENIED;
                event->state = EVENT_STATE_ACCESS_DENIED;
                return -5;
            }

            uint8_t color = event->data[0];
            uint8_t old_color = vga_get_color();

            vga_set_color(color);
            vga_clear_screen();
            vga_set_color(old_color);

            event->data[0] = VGA_SUCCESS;
            event->state = EVENT_STATE_COMPLETED;
            return 0;
        }

        case HW_VGA_CLEAR_LINE: {
            if (!check_vga_access(event->pid)) {
                event->data[0] = VGA_ERR_ACCESS_DENIED;
                event->state = EVENT_STATE_ACCESS_DENIED;
                return -5;
            }

            uint8_t row = event->data[0];
            uint8_t color = event->data[1];

            if (row >= VGA_HEIGHT) {
                event->data[0] = VGA_ERR_OUT_OF_BOUNDS;
                event->state = EVENT_STATE_ERROR;
                return -1;
            }

            uint8_t old_color = vga_get_color();
            vga_set_color(color);
            vga_clear_line(row);
            vga_set_color(old_color);

            event->data[0] = VGA_SUCCESS;
            event->state = EVENT_STATE_COMPLETED;
            return 0;
        }

        case HW_VGA_CLEAR_TO_EOL: {
            if (!check_vga_access(event->pid)) {
                event->data[0] = VGA_ERR_ACCESS_DENIED;
                event->state = EVENT_STATE_ACCESS_DENIED;
                return -5;
            }

            vga_clear_to_eol();

            event->data[0] = VGA_SUCCESS;
            event->state = EVENT_STATE_COMPLETED;
            return 0;
        }

        case HW_VGA_GET_CURSOR: {
            uint8_t row = vga_get_cursor_position_y();
            uint8_t col = vga_get_cursor_position_x();

            event->data[0] = VGA_SUCCESS;
            event->data[1] = row;
            event->data[2] = col;
            event->state = EVENT_STATE_COMPLETED;
            return 0;
        }

        case HW_VGA_SET_CURSOR: {
            if (!check_vga_access(event->pid)) {
                event->data[0] = VGA_ERR_ACCESS_DENIED;
                event->state = EVENT_STATE_ACCESS_DENIED;
                return -5;
            }

            uint8_t row = event->data[0];
            uint8_t col = event->data[1];

            if (row >= VGA_HEIGHT) row = VGA_HEIGHT - 1;
            if (col >= VGA_WIDTH) col = VGA_WIDTH - 1;

            vga_set_cursor_position(col, row);

            uint8_t clamped_row = vga_get_cursor_position_y();
            uint8_t clamped_col = vga_get_cursor_position_x();

            event->data[0] = VGA_SUCCESS;
            event->data[1] = clamped_row;
            event->data[2] = clamped_col;
            event->state = EVENT_STATE_COMPLETED;
            return 0;
        }

        case HW_VGA_SET_COLOR: {
            if (!check_vga_access(event->pid)) {
                event->data[0] = VGA_ERR_ACCESS_DENIED;
                event->state = EVENT_STATE_ACCESS_DENIED;
                return -5;
            }

            uint8_t color = event->data[0];
            uint8_t old_color = vga_get_color();

            vga_set_color(color);

            event->data[0] = VGA_SUCCESS;
            event->data[1] = old_color;
            event->state = EVENT_STATE_COMPLETED;
            return 0;
        }

        case HW_VGA_GET_COLOR: {
            uint8_t color = vga_get_color();

            event->data[0] = VGA_SUCCESS;
            event->data[1] = color;
            event->state = EVENT_STATE_COMPLETED;
            return 0;
        }

        case HW_VGA_SCROLL_UP: {
            if (!check_vga_access(event->pid)) {
                event->data[0] = VGA_ERR_ACCESS_DENIED;
                event->state = EVENT_STATE_ACCESS_DENIED;
                return -5;
            }

            vga_scroll_up();

            event->data[0] = VGA_SUCCESS;
            event->state = EVENT_STATE_COMPLETED;
            return 0;
        }

        case HW_VGA_NEWLINE: {
            if (!check_vga_access(event->pid)) {
                event->data[0] = VGA_ERR_ACCESS_DENIED;
                event->state = EVENT_STATE_ACCESS_DENIED;
                return -5;
            }

            vga_print_newline();

            uint8_t new_row = vga_get_cursor_position_y();
            uint8_t new_col = vga_get_cursor_position_x();

            event->data[0] = VGA_SUCCESS;
            event->data[1] = new_row;
            event->data[2] = new_col;
            event->state = EVENT_STATE_COMPLETED;
            return 0;
        }

        case HW_VGA_GET_DIMENSIONS: {
            event->data[0] = VGA_SUCCESS;
            event->data[1] = VGA_WIDTH;
            event->data[2] = VGA_HEIGHT;
            event->state = EVENT_STATE_COMPLETED;
            return 0;
        }

        case HW_SYSTEM_REBOOT:
            return do_reboot(event);

        case HW_SYSTEM_SHUTDOWN:
            return do_shutdown(event);

        case HW_USB_INIT: {
            process_t* proc = process_find(event->pid);
            if (!proc) {
                event->data[0] = USB_ERR_ACCESS_DENIED;
                event->state = EVENT_STATE_ACCESS_DENIED;
                return -5;
            }

            process_ref_inc(proc);

            if (!process_has_tag(proc, "hw_usb") && !process_has_tag(proc, "system")) {
                event->data[0] = USB_ERR_ACCESS_DENIED;
                    event->state = EVENT_STATE_ACCESS_DENIED;
                    process_ref_dec(proc);
                    return -5;
            }

            int result = xhci_init();

            process_ref_dec(proc);

            if (result != 0) {
                event->data[0] = USB_ERR_NO_CONTROLLER;
                event->state = EVENT_STATE_ERROR;
                return -1;
            }

            event->data[0] = 0x00;
            event->state = EVENT_STATE_COMPLETED;
            return 0;
        }

        case HW_USB_RESET: {
            process_t* proc = process_find(event->pid);
            if (!proc) {
                event->data[0] = USB_ERR_ACCESS_DENIED;
                event->state = EVENT_STATE_ACCESS_DENIED;
                return -5;
            }

            process_ref_inc(proc);

            if (!process_has_tag(proc, "hw_usb") && !process_has_tag(proc, "system")) {
                event->data[0] = USB_ERR_ACCESS_DENIED;
                    event->state = EVENT_STATE_ACCESS_DENIED;
                    process_ref_dec(proc);
                    return -5;
            }

            xhci_controller_t* ctrl = xhci_get_controller();
            if (!ctrl) {
                event->data[0] = USB_ERR_NOT_INITIALIZED;
                event->state = EVENT_STATE_ERROR;
                process_ref_dec(proc);
                return -1;
            }

            int result = xhci_reset(ctrl);

            process_ref_dec(proc);

            if (result != 0) {
                event->data[0] = USB_ERR_RESET_TIMEOUT;
                event->state = EVENT_STATE_ERROR;
                return -1;
            }

            event->data[0] = 0x00;
            event->state = EVENT_STATE_COMPLETED;
            return 0;
        }

        case HW_USB_START: {
            process_t* proc = process_find(event->pid);
            if (!proc) {
                event->data[0] = USB_ERR_ACCESS_DENIED;
                event->state = EVENT_STATE_ACCESS_DENIED;
                return -5;
            }

            process_ref_inc(proc);

            if (!process_has_tag(proc, "hw_usb") && !process_has_tag(proc, "system")) {
                event->data[0] = USB_ERR_ACCESS_DENIED;
                    event->state = EVENT_STATE_ACCESS_DENIED;
                    process_ref_dec(proc);
                    return -5;
            }

            xhci_controller_t* ctrl = xhci_get_controller();
            if (!ctrl) {
                event->data[0] = USB_ERR_NOT_INITIALIZED;
                event->state = EVENT_STATE_ERROR;
                process_ref_dec(proc);
                return -1;
            }

            int result = xhci_start(ctrl);

            process_ref_dec(proc);

            if (result != 0) {
                event->data[0] = USB_ERR_RESET_TIMEOUT;
                event->state = EVENT_STATE_ERROR;
                return -1;
            }

            event->data[0] = 0x00;
            event->state = EVENT_STATE_COMPLETED;
            return 0;
        }

        case HW_USB_STOP: {
            process_t* proc = process_find(event->pid);
            if (!proc) {
                event->data[0] = USB_ERR_ACCESS_DENIED;
                event->state = EVENT_STATE_ACCESS_DENIED;
                return -5;
            }

            process_ref_inc(proc);

            if (!process_has_tag(proc, "hw_usb") && !process_has_tag(proc, "system")) {
                event->data[0] = USB_ERR_ACCESS_DENIED;
                    event->state = EVENT_STATE_ACCESS_DENIED;
                    process_ref_dec(proc);
                    return -5;
            }

            process_ref_dec(proc);

            event->data[0] = 0x00;
            event->state = EVENT_STATE_COMPLETED;
            return 0;
        }

        case HW_USB_PORT_STATUS: {
            process_t* proc = process_find(event->pid);
            if (!proc) {
                event->data[0] = USB_ERR_ACCESS_DENIED;
                event->state = EVENT_STATE_ACCESS_DENIED;
                return -5;
            }

            process_ref_inc(proc);

            if (!process_has_tag(proc, "hw_usb") && !process_has_tag(proc, "system")) {
                event->data[0] = USB_ERR_ACCESS_DENIED;
                    event->state = EVENT_STATE_ACCESS_DENIED;
                    process_ref_dec(proc);
                    return -5;
            }

            uint8_t port = event->data[0];
            xhci_controller_t* ctrl = xhci_get_controller();

            if (!ctrl || !ctrl->initialized) {
                event->data[0] = USB_ERR_NOT_INITIALIZED;
                event->state = EVENT_STATE_ERROR;
                process_ref_dec(proc);
                return -1;
            }

            if (port == 0 || port > ctrl->max_ports) {
                event->data[0] = USB_ERR_INVALID_PORT;
                event->state = EVENT_STATE_ERROR;
                process_ref_dec(proc);
                return -1;
            }

            uint32_t portsc = xhci_get_port_status(ctrl, port);

            event->data[0] = 0x00;
            event->data[1] = (portsc >> 24) & 0xFF;
            event->data[2] = (portsc >> 16) & 0xFF;
            event->data[3] = (portsc >> 8) & 0xFF;
            event->data[4] = portsc & 0xFF;
            event->state = EVENT_STATE_COMPLETED;

            process_ref_dec(proc);
            return 0;
        }

        case HW_USB_PORT_RESET: {
            process_t* proc = process_find(event->pid);
            if (!proc) {
                event->data[0] = USB_ERR_ACCESS_DENIED;
                event->state = EVENT_STATE_ACCESS_DENIED;
                return -5;
            }

            process_ref_inc(proc);

            if (!process_has_tag(proc, "hw_usb") && !process_has_tag(proc, "system")) {
                event->data[0] = USB_ERR_ACCESS_DENIED;
                    event->state = EVENT_STATE_ACCESS_DENIED;
                    process_ref_dec(proc);
                    return -5;
            }

            uint8_t port = event->data[0];
            xhci_controller_t* ctrl = xhci_get_controller();

            if (!ctrl || !ctrl->initialized) {
                event->data[0] = USB_ERR_NOT_INITIALIZED;
                event->state = EVENT_STATE_ERROR;
                process_ref_dec(proc);
                return -1;
            }

            if (port == 0 || port > ctrl->max_ports) {
                event->data[0] = USB_ERR_INVALID_PORT;
                event->state = EVENT_STATE_ERROR;
                process_ref_dec(proc);
                return -1;
            }

            if (!xhci_port_has_device(ctrl, port)) {
                event->data[0] = USB_ERR_NO_DEVICE;
                event->state = EVENT_STATE_ERROR;
                process_ref_dec(proc);
                return -1;
            }

            int result = xhci_reset_port(ctrl, port);

            if (result != 0) {
                event->data[0] = USB_ERR_RESET_TIMEOUT;
                event->state = EVENT_STATE_ERROR;
                process_ref_dec(proc);
                return -1;
            }

            event->data[0] = 0x00;
            event->state = EVENT_STATE_COMPLETED;

            process_ref_dec(proc);
            return 0;
        }

        case HW_USB_PORT_QUERY: {
            process_t* proc = process_find(event->pid);
            if (!proc) {
                event->data[0] = USB_ERR_ACCESS_DENIED;
                event->state = EVENT_STATE_ACCESS_DENIED;
                return -5;
            }

            process_ref_inc(proc);

            if (!process_has_tag(proc, "hw_usb") && !process_has_tag(proc, "system")) {
                event->data[0] = USB_ERR_ACCESS_DENIED;
                    event->state = EVENT_STATE_ACCESS_DENIED;
                    process_ref_dec(proc);
                    return -5;
            }

            xhci_controller_t* ctrl = xhci_get_controller();

            if (!ctrl || !ctrl->initialized) {
                event->data[0] = USB_ERR_NOT_INITIALIZED;
                event->state = EVENT_STATE_ERROR;
                process_ref_dec(proc);
                return -1;
            }

            event->data[0] = 0x00;
            event->data[1] = ctrl->max_ports;
            event->data[2] = ctrl->max_slots;
            event->data[3] = ctrl->irq_line;
            event->data[4] = ctrl->use_polling ? 1 : 0;
            event->state = EVENT_STATE_COMPLETED;

            process_ref_dec(proc);
            return 0;
        }

        case HW_USB_ENUM_DEVICE: {
            process_t* proc = process_find(event->pid);
            if (!proc) {
                event->data[0] = USB_ERR_ACCESS_DENIED;
                event->state = EVENT_STATE_ACCESS_DENIED;
                return -5;
            }

            process_ref_inc(proc);

            if (!process_has_tag(proc, "hw_usb") && !process_has_tag(proc, "system")) {
                event->data[0] = USB_ERR_ACCESS_DENIED;
                    event->state = EVENT_STATE_ACCESS_DENIED;
                    process_ref_dec(proc);
                    return -5;
            }

            uint8_t port = event->data[0];

            xhci_controller_t* ctrl = xhci_get_controller();
            if (!ctrl || !ctrl->running) {
                event->data[0] = USB_ERR_NO_CONTROLLER;
                event->state = EVENT_STATE_ERROR;
                process_ref_dec(proc);
                return -1;
            }

            int result = xhci_enumerate_device(ctrl, port);
            process_ref_dec(proc);

            if (result < 0) {
                event->state = EVENT_STATE_ERROR;

                if (result == -1) {
                    event->data[0] = USB_ERR_NO_CONTROLLER;
                } else if (result == -2) {
                    event->data[0] = USB_ERR_INVALID_PORT;
                } else if (result == -3) {
                    event->data[0] = USB_ERR_INVALID_PORT;
                } else if (result == -4) {
                    event->data[0] = 0x04;
                } else if (result == -5) {
                    event->data[0] = 0x02;
                } else if (result == -6) {
                    event->data[0] = 0x03;
                } else {
                    event->data[0] = 0xFF;
                }

                return -1;
            }

            event->data[0] = 0x00;
            event->state = EVENT_STATE_COMPLETED;
            return 0;
        }

        case HW_USB_GET_INFO: {
            process_t* proc = process_find(event->pid);
            if (!proc) {
                event->data[0] = USB_ERR_ACCESS_DENIED;
                event->state = EVENT_STATE_ACCESS_DENIED;
                return -5;
            }

            process_ref_inc(proc);

            if (!process_has_tag(proc, "hw_usb") && !process_has_tag(proc, "system")) {
                event->data[0] = USB_ERR_ACCESS_DENIED;
                    event->state = EVENT_STATE_ACCESS_DENIED;
                    process_ref_dec(proc);
                    return -5;
            }

            uint8_t slot_id = event->data[0];

            xhci_controller_t* ctrl = xhci_get_controller();
            if (!ctrl || !ctrl->running) {
                event->data[0] = USB_ERR_NO_CONTROLLER;
                event->state = EVENT_STATE_ERROR;
                process_ref_dec(proc);
                return -1;
            }

            xhci_device_slot_t* slot = xhci_get_device_slot(ctrl, slot_id);
            if (!slot) {
                event->data[0] = USB_ERR_NO_DEVICE;
                event->state = EVENT_STATE_ERROR;
                process_ref_dec(proc);
                return -1;
            }

            event->data[0] = 0x00;
            event->data[1] = slot->slot_id;
            event->data[2] = slot->port_num;
            event->data[3] = slot->state;
            event->state = EVENT_STATE_COMPLETED;

            process_ref_dec(proc);
            return 0;
        }

        default: {
            event->state = EVENT_STATE_ERROR;
            return -1;
        }
    }
}
