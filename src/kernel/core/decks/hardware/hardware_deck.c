#include "hardware_deck.h"
#include "error.h"
#include "klib.h"
#include "io.h"
#include "pit.h"
#include "rtc.h"
#include "pic.h"
#include "irqchip.h"
#include "process.h"
#include "vmm.h"
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
    return (irq < irqchip_max_irqs() && irq != 0 && irq != 2);
}

static bool check_keyboard_access(uint32_t pid) {
    process_t* proc = process_find(pid);
    if (!proc) {
        return false;
    }

    return process_has_tag(proc, "hw_kb") ||
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

static int do_reboot(Pocket* pocket) {
    debug_printf("[Hardware] SYSTEM_REBOOT request from PID %u\n", pocket->pid);

    if (!check_power_access(pocket->pid)) {
        debug_printf("[Hardware] ERROR: PID %u lacks permission for reboot\n", pocket->pid);
        pocket->error_code = ERR_ACCESS_DENIED;
        return -5;
    }

    debug_printf("[Hardware] Rebooting system...\n");

    // Keyboard controller reset method (triple fault)
    outb(0x64, 0xFE);

    while (1) {
        asm volatile("cli; hlt");
    }

    return 0;
}

static int do_shutdown(Pocket* pocket) {
    debug_printf("[Hardware] SYSTEM_SHUTDOWN request from PID %u\n", pocket->pid);

    if (!check_power_access(pocket->pid)) {
        debug_printf("[Hardware] ERROR: PID %u lacks permission for shutdown\n", pocket->pid);
        pocket->error_code = ERR_ACCESS_DENIED;
        return -5;
    }

    debug_printf("[Hardware] Shutting down system...\n");

    process_cleanup_queue_flush();

    tagfs_sync();

    ata_flush_cache(1);

    acpi_shutdown();

    return 0;
}

int hardware_deck_handler(Pocket* pocket) {
    if (!pocket) {
        return -1;
    }

    uint8_t opcode = pocket_get_opcode(pocket, pocket->current_prefix_idx);

    switch (opcode) {
        case HW_TIMER_GET_TICKS: {
            process_t* proc = process_find(pocket->pid);
            uint8_t* data = vmm_translate_user_addr(proc->cabin, pocket->data_addr, pocket->data_length);
            if (!data) return -1;

            uint64_t ticks = pit_get_ticks();
            write_u64(data, ticks);
            return 0;
        }

        case HW_TIMER_GET_MS: {
            process_t* proc = process_find(pocket->pid);
            uint8_t* data = vmm_translate_user_addr(proc->cabin, pocket->data_addr, pocket->data_length);
            if (!data) return -1;

            uint64_t ticks = pit_get_ticks();
            uint32_t freq = pit_get_frequency();

            if (freq == 0) {
                pocket->error_code = 1;
                return -1;
            }

            uint64_t ms = (ticks * 1000) / freq;
            write_u64(data, ms);
            return 0;
        }

        case HW_TIMER_GET_FREQ: {
            process_t* proc = process_find(pocket->pid);
            uint8_t* data = vmm_translate_user_addr(proc->cabin, pocket->data_addr, pocket->data_length);
            if (!data) return -1;

            uint32_t freq = pit_get_frequency();
            write_u32(data, freq);
            return 0;
        }

        case HW_TIMER_SCHEDULE: {
            pocket->error_code = 1;
            return -1;
        }

        case HW_TIMER_CANCEL: {
            pocket->error_code = 1;
            return -1;
        }

        case HW_RTC_GET_TIME: {
            process_t* proc = process_find(pocket->pid);
            uint8_t* data = vmm_translate_user_addr(proc->cabin, pocket->data_addr, pocket->data_length);
            if (!data) return -1;

            time_t t;
            rtc_get_boxtime(&t);
            write_u64(data + 0, t.seconds);
            write_u32(data + 8, t.nanosec);
            write_u16(data + 12, t.year);
            data[14] = t.month;
            data[15] = t.day;
            data[16] = t.hour;
            data[17] = t.minute;
            data[18] = t.second;
            data[19] = t.weekday;
            return 0;
        }

        case HW_RTC_GET_UNIX64: {
            process_t* proc = process_find(pocket->pid);
            uint8_t* data = vmm_translate_user_addr(proc->cabin, pocket->data_addr, pocket->data_length);
            if (!data) return -1;

            uint64_t secs = rtc_get_unix64();
            write_u64(data, secs);
            return 0;
        }

        case HW_RTC_GET_UPTIME: {
            process_t* proc = process_find(pocket->pid);
            uint8_t* data = vmm_translate_user_addr(proc->cabin, pocket->data_addr, pocket->data_length);
            if (!data) return -1;

            uint64_t ns = rtc_get_uptime_ns();
            write_u64(data, ns);
            return 0;
        }

        case HW_PORT_INB: {
            process_t* proc = process_find(pocket->pid);
            uint8_t* data = vmm_translate_user_addr(proc->cabin, pocket->data_addr, pocket->data_length);
            if (!data) return -1;

            uint16_t port = read_u16(data);

            if (hw_port_is_forbidden(port)) {
                debug_printf("[HW_DECK] FORBIDDEN port 0x%04x blocked (INB)\n", port);
                pocket->error_code = ERR_ACCESS_DENIED;
                return -5;
            }

            if (proc) {
                process_ref_inc(proc);

                if (!process_has_tag(proc, "system")) {
                    if (!hw_port_is_safe_read(port)) {
                        debug_printf("[HW_DECK] Port 0x%04x requires system tag (INB)\n", port);
                        pocket->error_code = ERR_ACCESS_DENIED;
                        process_ref_dec(proc);
                        return -5;
                    }
                }

                process_ref_dec(proc);
            }

            uint8_t value = inb(port);
            data[0] = value;
            return 0;
        }

        case HW_PORT_OUTB: {
            process_t* proc = process_find(pocket->pid);
            uint8_t* data = vmm_translate_user_addr(proc->cabin, pocket->data_addr, pocket->data_length);
            if (!data) return -1;

            uint16_t port = read_u16(data);
            uint8_t value = data[2];

            if (hw_port_is_forbidden(port)) {
                debug_printf("[HW_DECK] FORBIDDEN port 0x%04x blocked (OUTB)\n", port);
                pocket->error_code = ERR_ACCESS_DENIED;
                return -5;
            }

            if (proc) {
                process_ref_inc(proc);

                if (!process_has_tag(proc, "system")) {
                    if (!hw_port_is_safe_write(port)) {
                        debug_printf("[HW_DECK] Port 0x%04x requires system tag (OUTB)\n", port);
                        pocket->error_code = ERR_ACCESS_DENIED;
                        process_ref_dec(proc);
                        return -5;
                    }
                }

                process_ref_dec(proc);
            }

            outb(port, value);
            data[0] = 0;
            return 0;
        }

        case HW_PORT_INW: {
            process_t* proc = process_find(pocket->pid);
            uint8_t* data = vmm_translate_user_addr(proc->cabin, pocket->data_addr, pocket->data_length);
            if (!data) return -1;

            uint16_t port = read_u16(data);

            if (hw_port_is_forbidden(port)) {
                debug_printf("[HW_DECK] FORBIDDEN port 0x%04x blocked (INW)\n", port);
                pocket->error_code = ERR_ACCESS_DENIED;
                return -5;
            }

            if (proc) {
                process_ref_inc(proc);

                if (!process_has_tag(proc, "system")) {
                    if (!hw_port_is_safe_read(port)) {
                        debug_printf("[HW_DECK] Port 0x%04x requires system tag (INW)\n", port);
                        pocket->error_code = ERR_ACCESS_DENIED;
                        process_ref_dec(proc);
                        return -5;
                    }
                }

                process_ref_dec(proc);
            }

            uint16_t value = inw(port);
            write_u16(data, value);
            return 0;
        }

        case HW_PORT_OUTW: {
            process_t* proc = process_find(pocket->pid);
            uint8_t* data = vmm_translate_user_addr(proc->cabin, pocket->data_addr, pocket->data_length);
            if (!data) return -1;

            uint16_t port = read_u16(data);
            uint16_t value = read_u16(data + 2);

            if (hw_port_is_forbidden(port)) {
                debug_printf("[HW_DECK] FORBIDDEN port 0x%04x blocked (OUTW)\n", port);
                pocket->error_code = ERR_ACCESS_DENIED;
                return -5;
            }

            if (proc) {
                process_ref_inc(proc);

                if (!process_has_tag(proc, "system")) {
                    if (!hw_port_is_safe_write(port)) {
                        debug_printf("[HW_DECK] Port 0x%04x requires system tag (OUTW)\n", port);
                        pocket->error_code = ERR_ACCESS_DENIED;
                        process_ref_dec(proc);
                        return -5;
                    }
                }

                process_ref_dec(proc);
            }

            outw(port, value);
            data[0] = 0;
            return 0;
        }

        case HW_PORT_INL: {
            process_t* proc = process_find(pocket->pid);
            uint8_t* data = vmm_translate_user_addr(proc->cabin, pocket->data_addr, pocket->data_length);
            if (!data) return -1;

            uint16_t port = read_u16(data);

            if (hw_port_is_forbidden(port)) {
                debug_printf("[HW_DECK] FORBIDDEN port 0x%04x blocked (INL)\n", port);
                pocket->error_code = ERR_ACCESS_DENIED;
                return -5;
            }

            if (proc) {
                process_ref_inc(proc);

                if (!process_has_tag(proc, "system")) {
                    if (!hw_port_is_safe_read(port)) {
                        debug_printf("[HW_DECK] Port 0x%04x requires system tag (INL)\n", port);
                        pocket->error_code = ERR_ACCESS_DENIED;
                        process_ref_dec(proc);
                        return -5;
                    }
                }

                process_ref_dec(proc);
            }

            uint32_t value = inl(port);
            write_u32(data, value);
            return 0;
        }

        case HW_PORT_OUTL: {
            process_t* proc = process_find(pocket->pid);
            uint8_t* data = vmm_translate_user_addr(proc->cabin, pocket->data_addr, pocket->data_length);
            if (!data) return -1;

            uint16_t port = read_u16(data);
            uint32_t value = read_u32(data + 2);

            if (hw_port_is_forbidden(port)) {
                debug_printf("[HW_DECK] FORBIDDEN port 0x%04x blocked (OUTL)\n", port);
                pocket->error_code = ERR_ACCESS_DENIED;
                return -5;
            }

            if (proc) {
                process_ref_inc(proc);

                if (!process_has_tag(proc, "system")) {
                    if (!hw_port_is_safe_write(port)) {
                        debug_printf("[HW_DECK] Port 0x%04x requires system tag (OUTL)\n", port);
                        pocket->error_code = ERR_ACCESS_DENIED;
                        process_ref_dec(proc);
                        return -5;
                    }
                }

                process_ref_dec(proc);
            }

            outl(port, value);
            data[0] = 0;
            return 0;
        }

        case HW_IRQ_ENABLE: {
            process_t* proc = process_find(pocket->pid);
            uint8_t* data = vmm_translate_user_addr(proc->cabin, pocket->data_addr, pocket->data_length);
            if (!data) return -1;

            uint8_t irq = data[0];
            if (!hw_irq_is_valid(irq)) {
                pocket->error_code = ERR_ACCESS_DENIED;
                return -5;
            }
            irqchip_enable_irq(irq);
            data[0] = 0;
            return 0;
        }

        case HW_IRQ_DISABLE: {
            process_t* proc = process_find(pocket->pid);
            uint8_t* data = vmm_translate_user_addr(proc->cabin, pocket->data_addr, pocket->data_length);
            if (!data) return -1;

            uint8_t irq = data[0];
            if (!hw_irq_is_valid(irq)) {
                pocket->error_code = ERR_ACCESS_DENIED;
                return -5;
            }
            irqchip_disable_irq(irq);
            data[0] = 0;
            return 0;
        }

        case HW_IRQ_GET_ISR: {
            process_t* proc = process_find(pocket->pid);
            uint8_t* data = vmm_translate_user_addr(proc->cabin, pocket->data_addr, pocket->data_length);
            if (!data) return -1;

            uint32_t isr = irqchip_get_isr();
            write_u32(data, isr);
            return 0;
        }

        case HW_IRQ_GET_IRR: {
            process_t* proc = process_find(pocket->pid);
            uint8_t* data = vmm_translate_user_addr(proc->cabin, pocket->data_addr, pocket->data_length);
            if (!data) return -1;

            uint32_t irr = irqchip_get_irr();
            write_u32(data, irr);
            return 0;
        }

        case HW_IRQ_REGISTER: {
            pocket->error_code = 1;
            return -1;
        }

        case HW_IRQ_UNREGISTER: {
            pocket->error_code = 1;
            return -1;
        }

        case HW_IRQ_SEND_EOI: {
            process_t* proc = process_find(pocket->pid);
            uint8_t* data = vmm_translate_user_addr(proc->cabin, pocket->data_addr, pocket->data_length);
            if (!data) return -1;

            uint8_t irq = data[0];
            if (!hw_irq_is_valid(irq)) {
                pocket->error_code = ERR_ACCESS_DENIED;
                return -5;
            }
            irqchip_send_eoi(irq);
            data[0] = 0;
            return 0;
        }

        case HW_CPU_HALT: {
            process_t* proc = process_find(pocket->pid);
            uint8_t* data = vmm_translate_user_addr(proc->cabin, pocket->data_addr, pocket->data_length);
            if (!data) return -1;

            hlt();
            data[0] = 0;
            return 0;
        }

        case HW_DISK_INFO: {
            process_t* proc = process_find(pocket->pid);
            uint8_t* data = vmm_translate_user_addr(proc->cabin, pocket->data_addr, pocket->data_length);
            if (!data) return -1;

            uint8_t is_master = data[0];
            ATADevice* device = is_master ? &ata_primary_master : &ata_primary_slave;

            data[0] = device->exists;
            memcpy(&data[1], device->model, 40);
            memcpy(&data[41], device->serial, 20);

            // Big-endian encoding for portability (8 bytes for 48-bit LBA)
            uint64_t sectors = device->total_sectors;
            data[61] = (sectors >> 56) & 0xFF;
            data[62] = (sectors >> 48) & 0xFF;
            data[63] = (sectors >> 40) & 0xFF;
            data[64] = (sectors >> 32) & 0xFF;
            data[65] = (sectors >> 24) & 0xFF;
            data[66] = (sectors >> 16) & 0xFF;
            data[67] = (sectors >> 8) & 0xFF;
            data[68] = sectors & 0xFF;

            uint64_t size_mb = device->size_mb;
            data[69] = (size_mb >> 56) & 0xFF;
            data[70] = (size_mb >> 48) & 0xFF;
            data[71] = (size_mb >> 40) & 0xFF;
            data[72] = (size_mb >> 32) & 0xFF;
            data[73] = (size_mb >> 24) & 0xFF;
            data[74] = (size_mb >> 16) & 0xFF;
            data[75] = (size_mb >> 8) & 0xFF;
            data[76] = size_mb & 0xFF;

            return 0;
        }

        case HW_DISK_FLUSH: {
            process_t* proc = process_find(pocket->pid);
            uint8_t* data = vmm_translate_user_addr(proc->cabin, pocket->data_addr, pocket->data_length);
            if (!data) return -1;

            uint8_t is_master = data[0];
            int result = ata_flush_cache(is_master);
            data[0] = (result == 0) ? 0 : 1;
            return 0;
        }

        case HW_KEYBOARD_GETCHAR: {
            if (!check_keyboard_access(pocket->pid)) {
                pocket->error_code = HW_KB_ACCESS_DENIED;
                return -5;
            }

            process_t* proc = process_find(pocket->pid);
            uint8_t* data = vmm_translate_user_addr(proc->cabin, pocket->data_addr, pocket->data_length);
            if (!data) return -1;

            if (!keyboard_has_input()) {
                data[0] = 0;
                data[1] = 0;
                data[2] = 0;
                data[3] = HW_KB_NO_DATA;
                return 0;
            }

            char ch = keyboard_getchar();
            keyboard_state_t* kb_state = keyboard_get_state();

            data[0] = (uint8_t)ch;
            data[1] = kb_state->last_keycode;

            uint8_t modifiers = 0;
            if (kb_state->shift_pressed) modifiers |= 0x01;
            if (kb_state->ctrl_pressed)  modifiers |= 0x02;
            if (kb_state->alt_pressed)   modifiers |= 0x04;
            data[2] = modifiers;

            data[3] = HW_KB_SUCCESS;
            return 0;
        }

        case HW_KEYBOARD_READLINE: {
            if (!check_keyboard_access(pocket->pid)) {
                pocket->error_code = HW_KB_ACCESS_DENIED;
                return -5;
            }

            process_t* proc = process_find(pocket->pid);
            uint8_t* data = vmm_translate_user_addr(proc->cabin, pocket->data_addr, pocket->data_length);
            if (!data) return -1;

            uint8_t max_length = data[0];
            uint8_t echo_mode = data[1];

            if (max_length == 0 || max_length > 187) {
                max_length = 187;
            }

            keyboard_set_echo(echo_mode != 0);

            char line_buffer[188];
            memset(line_buffer, 0, sizeof(line_buffer));

            int line_ready = keyboard_readline_async(line_buffer, max_length);

            if (line_ready) {
                size_t len = strlen(line_buffer);

                data[0] = (len >> 24) & 0xFF;
                data[1] = (len >> 16) & 0xFF;
                data[2] = (len >> 8) & 0xFF;
                data[3] = len & 0xFF;

                size_t copy_len = len < 187 ? len : 187;
                memcpy(&data[4], line_buffer, copy_len);
                data[4 + copy_len] = '\0';

                data[pocket->data_length - 1] = HW_KB_SUCCESS;
                return 0;
            } else {
                data[0] = 0;
                data[1] = 0;
                data[2] = 0;
                data[3] = 0;
                data[pocket->data_length - 1] = HW_KB_WOULD_BLOCK;
                pocket->error_code = ERR_WOULD_BLOCK;
                return 0;
            }
        }

        case HW_KEYBOARD_STATUS: {
            if (!check_keyboard_access(pocket->pid)) {
                pocket->error_code = HW_KB_ACCESS_DENIED;
                return -5;
            }

            process_t* proc = process_find(pocket->pid);
            uint8_t* data = vmm_translate_user_addr(proc->cabin, pocket->data_addr, pocket->data_length);
            if (!data) return -1;

            uint32_t available = keyboard_available();

            data[0] = (available >> 24) & 0xFF;
            data[1] = (available >> 16) & 0xFF;
            data[2] = (available >> 8) & 0xFF;
            data[3] = available & 0xFF;

            uint32_t buffer_size = 256;
            data[4] = (buffer_size >> 24) & 0xFF;
            data[5] = (buffer_size >> 16) & 0xFF;
            data[6] = (buffer_size >> 8) & 0xFF;
            data[7] = buffer_size & 0xFF;

            return 0;
        }

        case HW_VGA_PUTCHAR: {
            if (!check_vga_access(pocket->pid)) {
                pocket->error_code = VGA_ERR_ACCESS_DENIED;
                return -5;
            }

            process_t* proc = process_find(pocket->pid);
            uint8_t* data = vmm_translate_user_addr(proc->cabin, pocket->data_addr, pocket->data_length);
            if (!data) return -1;

            uint8_t row = data[0];
            uint8_t col = data[1];
            uint8_t character = data[2];
            uint8_t color = data[3];

            if (row >= VGA_HEIGHT || col >= VGA_WIDTH) {
                data[0] = VGA_ERR_OUT_OF_BOUNDS;
                pocket->error_code = VGA_ERR_OUT_OF_BOUNDS;
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

            data[0] = VGA_SUCCESS;
            return 0;
        }

        case HW_VGA_PUTSTRING: {
            if (!check_vga_access(pocket->pid)) {
                pocket->error_code = VGA_ERR_ACCESS_DENIED;
                return -5;
            }

            process_t* proc = process_find(pocket->pid);
            uint8_t* data = vmm_translate_user_addr(proc->cabin, pocket->data_addr, pocket->data_length);
            if (!data) return -1;

            uint8_t length = data[0];
            uint8_t color  = data[1];
            uint8_t flags  = data[2];

            if (length > 185) {
                data[0] = VGA_ERR_STRING_TOO_LONG;
                pocket->error_code = VGA_ERR_STRING_TOO_LONG;
                return -1;
            }

            uint8_t old_color = vga_get_color();
            vga_set_color(color);

            uint8_t chars_written = 0;
            for (uint8_t i = 0; i < length && data[4 + i] != '\0'; i++) {
                char ch = data[4 + i];
                vga_print_char(ch, color);
                serial_putchar(ch);
                chars_written++;
            }

            if (!(flags & 0x02)) {
                vga_set_color(old_color);
            }

            uint8_t final_row = vga_get_cursor_position_y();
            uint8_t final_col = vga_get_cursor_position_x();

            data[0] = VGA_SUCCESS;
            data[1] = chars_written;
            data[2] = final_row;
            data[3] = final_col;
            return 0;
        }

        case HW_VGA_CLEAR_SCREEN: {
            if (!check_vga_access(pocket->pid)) {
                pocket->error_code = VGA_ERR_ACCESS_DENIED;
                return -5;
            }

            process_t* proc = process_find(pocket->pid);
            uint8_t* data = vmm_translate_user_addr(proc->cabin, pocket->data_addr, pocket->data_length);
            if (!data) return -1;

            uint8_t color = data[0];
            uint8_t old_color = vga_get_color();

            vga_set_color(color);
            vga_clear_screen();
            vga_set_color(old_color);

            data[0] = VGA_SUCCESS;
            return 0;
        }

        case HW_VGA_CLEAR_LINE: {
            if (!check_vga_access(pocket->pid)) {
                pocket->error_code = VGA_ERR_ACCESS_DENIED;
                return -5;
            }

            process_t* proc = process_find(pocket->pid);
            uint8_t* data = vmm_translate_user_addr(proc->cabin, pocket->data_addr, pocket->data_length);
            if (!data) return -1;

            uint8_t row = data[0];
            uint8_t color = data[1];

            if (row >= VGA_HEIGHT) {
                data[0] = VGA_ERR_OUT_OF_BOUNDS;
                pocket->error_code = VGA_ERR_OUT_OF_BOUNDS;
                return -1;
            }

            uint8_t old_color = vga_get_color();
            vga_set_color(color);
            vga_clear_line(row);
            vga_set_color(old_color);

            data[0] = VGA_SUCCESS;
            return 0;
        }

        case HW_VGA_CLEAR_TO_EOL: {
            if (!check_vga_access(pocket->pid)) {
                pocket->error_code = VGA_ERR_ACCESS_DENIED;
                return -5;
            }

            process_t* proc = process_find(pocket->pid);
            uint8_t* data = vmm_translate_user_addr(proc->cabin, pocket->data_addr, pocket->data_length);
            if (!data) return -1;

            vga_clear_to_eol();

            data[0] = VGA_SUCCESS;
            return 0;
        }

        case HW_VGA_GET_CURSOR: {
            process_t* proc = process_find(pocket->pid);
            uint8_t* data = vmm_translate_user_addr(proc->cabin, pocket->data_addr, pocket->data_length);
            if (!data) return -1;

            uint8_t row = vga_get_cursor_position_y();
            uint8_t col = vga_get_cursor_position_x();

            data[0] = VGA_SUCCESS;
            data[1] = row;
            data[2] = col;
            return 0;
        }

        case HW_VGA_SET_CURSOR: {
            if (!check_vga_access(pocket->pid)) {
                pocket->error_code = VGA_ERR_ACCESS_DENIED;
                return -5;
            }

            process_t* proc = process_find(pocket->pid);
            uint8_t* data = vmm_translate_user_addr(proc->cabin, pocket->data_addr, pocket->data_length);
            if (!data) return -1;

            uint8_t row = data[0];
            uint8_t col = data[1];

            if (row >= VGA_HEIGHT) row = VGA_HEIGHT - 1;
            if (col >= VGA_WIDTH) col = VGA_WIDTH - 1;

            vga_set_cursor_position(col, row);

            uint8_t clamped_row = vga_get_cursor_position_y();
            uint8_t clamped_col = vga_get_cursor_position_x();

            data[0] = VGA_SUCCESS;
            data[1] = clamped_row;
            data[2] = clamped_col;
            return 0;
        }

        case HW_VGA_SET_COLOR: {
            if (!check_vga_access(pocket->pid)) {
                pocket->error_code = VGA_ERR_ACCESS_DENIED;
                return -5;
            }

            process_t* proc = process_find(pocket->pid);
            uint8_t* data = vmm_translate_user_addr(proc->cabin, pocket->data_addr, pocket->data_length);
            if (!data) return -1;

            uint8_t color = data[0];
            uint8_t old_color = vga_get_color();

            vga_set_color(color);

            data[0] = VGA_SUCCESS;
            data[1] = old_color;
            return 0;
        }

        case HW_VGA_GET_COLOR: {
            process_t* proc = process_find(pocket->pid);
            uint8_t* data = vmm_translate_user_addr(proc->cabin, pocket->data_addr, pocket->data_length);
            if (!data) return -1;

            uint8_t color = vga_get_color();

            data[0] = VGA_SUCCESS;
            data[1] = color;
            return 0;
        }

        case HW_VGA_SCROLL_UP: {
            if (!check_vga_access(pocket->pid)) {
                pocket->error_code = VGA_ERR_ACCESS_DENIED;
                return -5;
            }

            process_t* proc = process_find(pocket->pid);
            uint8_t* data = vmm_translate_user_addr(proc->cabin, pocket->data_addr, pocket->data_length);
            if (!data) return -1;

            vga_scroll_up();

            data[0] = VGA_SUCCESS;
            return 0;
        }

        case HW_VGA_NEWLINE: {
            if (!check_vga_access(pocket->pid)) {
                pocket->error_code = VGA_ERR_ACCESS_DENIED;
                return -5;
            }

            process_t* proc = process_find(pocket->pid);
            uint8_t* data = vmm_translate_user_addr(proc->cabin, pocket->data_addr, pocket->data_length);
            if (!data) return -1;

            vga_print_newline();

            uint8_t new_row = vga_get_cursor_position_y();
            uint8_t new_col = vga_get_cursor_position_x();

            data[0] = VGA_SUCCESS;
            data[1] = new_row;
            data[2] = new_col;
            return 0;
        }

        case HW_VGA_GET_DIMENSIONS: {
            process_t* proc = process_find(pocket->pid);
            uint8_t* data = vmm_translate_user_addr(proc->cabin, pocket->data_addr, pocket->data_length);
            if (!data) return -1;

            data[0] = VGA_SUCCESS;
            data[1] = VGA_WIDTH;
            data[2] = VGA_HEIGHT;
            return 0;
        }

        case HW_SYSTEM_REBOOT:
            return do_reboot(pocket);

        case HW_SYSTEM_SHUTDOWN:
            return do_shutdown(pocket);

        case HW_USB_INIT: {
            process_t* proc = process_find(pocket->pid);
            if (!proc) {
                pocket->error_code = USB_ERR_ACCESS_DENIED;
                return -5;
            }

            process_ref_inc(proc);

            if (!process_has_tag(proc, "hw_usb") && !process_has_tag(proc, "system")) {
                pocket->error_code = USB_ERR_ACCESS_DENIED;
                process_ref_dec(proc);
                return -5;
            }

            int result = xhci_init();

            process_ref_dec(proc);

            if (result != 0) {
                pocket->error_code = USB_ERR_NO_CONTROLLER;
                return -1;
            }

            uint8_t* data = vmm_translate_user_addr(proc->cabin, pocket->data_addr, pocket->data_length);
            if (data) data[0] = 0x00;
            return 0;
        }

        case HW_USB_RESET: {
            process_t* proc = process_find(pocket->pid);
            if (!proc) {
                pocket->error_code = USB_ERR_ACCESS_DENIED;
                return -5;
            }

            process_ref_inc(proc);

            if (!process_has_tag(proc, "hw_usb") && !process_has_tag(proc, "system")) {
                pocket->error_code = USB_ERR_ACCESS_DENIED;
                process_ref_dec(proc);
                return -5;
            }

            xhci_controller_t* ctrl = xhci_get_controller();
            if (!ctrl) {
                pocket->error_code = USB_ERR_NOT_INITIALIZED;
                process_ref_dec(proc);
                return -1;
            }

            int result = xhci_reset(ctrl);

            process_ref_dec(proc);

            if (result != 0) {
                pocket->error_code = USB_ERR_RESET_TIMEOUT;
                return -1;
            }

            uint8_t* data = vmm_translate_user_addr(proc->cabin, pocket->data_addr, pocket->data_length);
            if (data) data[0] = 0x00;
            return 0;
        }

        case HW_USB_START: {
            process_t* proc = process_find(pocket->pid);
            if (!proc) {
                pocket->error_code = USB_ERR_ACCESS_DENIED;
                return -5;
            }

            process_ref_inc(proc);

            if (!process_has_tag(proc, "hw_usb") && !process_has_tag(proc, "system")) {
                pocket->error_code = USB_ERR_ACCESS_DENIED;
                process_ref_dec(proc);
                return -5;
            }

            xhci_controller_t* ctrl = xhci_get_controller();
            if (!ctrl) {
                pocket->error_code = USB_ERR_NOT_INITIALIZED;
                process_ref_dec(proc);
                return -1;
            }

            int result = xhci_start(ctrl);

            process_ref_dec(proc);

            if (result != 0) {
                pocket->error_code = USB_ERR_RESET_TIMEOUT;
                return -1;
            }

            uint8_t* data = vmm_translate_user_addr(proc->cabin, pocket->data_addr, pocket->data_length);
            if (data) data[0] = 0x00;
            return 0;
        }

        case HW_USB_STOP: {
            process_t* proc = process_find(pocket->pid);
            if (!proc) {
                pocket->error_code = USB_ERR_ACCESS_DENIED;
                return -5;
            }

            process_ref_inc(proc);

            if (!process_has_tag(proc, "hw_usb") && !process_has_tag(proc, "system")) {
                pocket->error_code = USB_ERR_ACCESS_DENIED;
                process_ref_dec(proc);
                return -5;
            }

            process_ref_dec(proc);

            uint8_t* data = vmm_translate_user_addr(proc->cabin, pocket->data_addr, pocket->data_length);
            if (data) data[0] = 0x00;
            return 0;
        }

        case HW_USB_PORT_STATUS: {
            process_t* proc = process_find(pocket->pid);
            if (!proc) {
                pocket->error_code = USB_ERR_ACCESS_DENIED;
                return -5;
            }

            process_ref_inc(proc);

            if (!process_has_tag(proc, "hw_usb") && !process_has_tag(proc, "system")) {
                pocket->error_code = USB_ERR_ACCESS_DENIED;
                process_ref_dec(proc);
                return -5;
            }

            uint8_t* data = vmm_translate_user_addr(proc->cabin, pocket->data_addr, pocket->data_length);
            if (!data) {
                process_ref_dec(proc);
                return -1;
            }

            uint8_t port = data[0];
            xhci_controller_t* ctrl = xhci_get_controller();

            if (!ctrl || !ctrl->initialized) {
                pocket->error_code = USB_ERR_NOT_INITIALIZED;
                process_ref_dec(proc);
                return -1;
            }

            if (port == 0 || port > ctrl->max_ports) {
                pocket->error_code = USB_ERR_INVALID_PORT;
                process_ref_dec(proc);
                return -1;
            }

            uint32_t portsc = xhci_get_port_status(ctrl, port);

            data[0] = 0x00;
            data[1] = (portsc >> 24) & 0xFF;
            data[2] = (portsc >> 16) & 0xFF;
            data[3] = (portsc >> 8) & 0xFF;
            data[4] = portsc & 0xFF;

            process_ref_dec(proc);
            return 0;
        }

        case HW_USB_PORT_RESET: {
            process_t* proc = process_find(pocket->pid);
            if (!proc) {
                pocket->error_code = USB_ERR_ACCESS_DENIED;
                return -5;
            }

            process_ref_inc(proc);

            if (!process_has_tag(proc, "hw_usb") && !process_has_tag(proc, "system")) {
                pocket->error_code = USB_ERR_ACCESS_DENIED;
                process_ref_dec(proc);
                return -5;
            }

            uint8_t* data = vmm_translate_user_addr(proc->cabin, pocket->data_addr, pocket->data_length);
            if (!data) {
                process_ref_dec(proc);
                return -1;
            }

            uint8_t port = data[0];
            xhci_controller_t* ctrl = xhci_get_controller();

            if (!ctrl || !ctrl->initialized) {
                pocket->error_code = USB_ERR_NOT_INITIALIZED;
                process_ref_dec(proc);
                return -1;
            }

            if (port == 0 || port > ctrl->max_ports) {
                pocket->error_code = USB_ERR_INVALID_PORT;
                process_ref_dec(proc);
                return -1;
            }

            if (!xhci_port_has_device(ctrl, port)) {
                pocket->error_code = USB_ERR_NO_DEVICE;
                process_ref_dec(proc);
                return -1;
            }

            int result = xhci_reset_port(ctrl, port);

            if (result != 0) {
                pocket->error_code = USB_ERR_RESET_TIMEOUT;
                process_ref_dec(proc);
                return -1;
            }

            data[0] = 0x00;

            process_ref_dec(proc);
            return 0;
        }

        case HW_USB_PORT_QUERY: {
            process_t* proc = process_find(pocket->pid);
            if (!proc) {
                pocket->error_code = USB_ERR_ACCESS_DENIED;
                return -5;
            }

            process_ref_inc(proc);

            if (!process_has_tag(proc, "hw_usb") && !process_has_tag(proc, "system")) {
                pocket->error_code = USB_ERR_ACCESS_DENIED;
                process_ref_dec(proc);
                return -5;
            }

            xhci_controller_t* ctrl = xhci_get_controller();

            if (!ctrl || !ctrl->initialized) {
                pocket->error_code = USB_ERR_NOT_INITIALIZED;
                process_ref_dec(proc);
                return -1;
            }

            uint8_t* data = vmm_translate_user_addr(proc->cabin, pocket->data_addr, pocket->data_length);
            if (!data) {
                process_ref_dec(proc);
                return -1;
            }

            data[0] = 0x00;
            data[1] = ctrl->max_ports;
            data[2] = ctrl->max_slots;
            data[3] = ctrl->irq_line;
            data[4] = ctrl->use_polling ? 1 : 0;

            process_ref_dec(proc);
            return 0;
        }

        case HW_USB_ENUM_DEVICE: {
            process_t* proc = process_find(pocket->pid);
            if (!proc) {
                pocket->error_code = USB_ERR_ACCESS_DENIED;
                return -5;
            }

            process_ref_inc(proc);

            if (!process_has_tag(proc, "hw_usb") && !process_has_tag(proc, "system")) {
                pocket->error_code = USB_ERR_ACCESS_DENIED;
                process_ref_dec(proc);
                return -5;
            }

            uint8_t* data = vmm_translate_user_addr(proc->cabin, pocket->data_addr, pocket->data_length);
            if (!data) {
                process_ref_dec(proc);
                return -1;
            }

            uint8_t port = data[0];

            xhci_controller_t* ctrl = xhci_get_controller();
            if (!ctrl || !ctrl->running) {
                pocket->error_code = USB_ERR_NO_CONTROLLER;
                process_ref_dec(proc);
                return -1;
            }

            int result = xhci_enumerate_device(ctrl, port);
            process_ref_dec(proc);

            if (result < 0) {
                if (result == -1) {
                    data[0] = USB_ERR_NO_CONTROLLER;
                } else if (result == -2) {
                    data[0] = USB_ERR_INVALID_PORT;
                } else if (result == -3) {
                    data[0] = USB_ERR_INVALID_PORT;
                } else if (result == -4) {
                    data[0] = 0x04;
                } else if (result == -5) {
                    data[0] = 0x02;
                } else if (result == -6) {
                    data[0] = 0x03;
                } else {
                    data[0] = 0xFF;
                }

                pocket->error_code = data[0];
                return -1;
            }

            data[0] = 0x00;
            return 0;
        }

        case HW_USB_GET_INFO: {
            process_t* proc = process_find(pocket->pid);
            if (!proc) {
                pocket->error_code = USB_ERR_ACCESS_DENIED;
                return -5;
            }

            process_ref_inc(proc);

            if (!process_has_tag(proc, "hw_usb") && !process_has_tag(proc, "system")) {
                pocket->error_code = USB_ERR_ACCESS_DENIED;
                process_ref_dec(proc);
                return -5;
            }

            uint8_t* data = vmm_translate_user_addr(proc->cabin, pocket->data_addr, pocket->data_length);
            if (!data) {
                process_ref_dec(proc);
                return -1;
            }

            uint8_t slot_id = data[0];

            xhci_controller_t* ctrl = xhci_get_controller();
            if (!ctrl || !ctrl->running) {
                pocket->error_code = USB_ERR_NO_CONTROLLER;
                process_ref_dec(proc);
                return -1;
            }

            xhci_device_slot_t* slot = xhci_get_device_slot(ctrl, slot_id);
            if (!slot) {
                pocket->error_code = USB_ERR_NO_DEVICE;
                process_ref_dec(proc);
                return -1;
            }

            data[0] = 0x00;
            data[1] = slot->slot_id;
            data[2] = slot->port_num;
            data[3] = slot->state;

            process_ref_dec(proc);
            return 0;
        }

        default: {
            pocket->error_code = 1;
            return -1;
        }
    }
}
