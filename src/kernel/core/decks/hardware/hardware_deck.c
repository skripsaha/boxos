#include "hardware_deck.h"
#include "deck_utils.h"
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
#include "system_halt.h"

static bool hw_irq_is_valid(uint8_t irq) {
    return (irq < irqchip_max_irqs() && irq != 0 && irq != 2);
}

static int do_reboot(Pocket* pocket, process_t* proc) {
    debug_printf("[Hardware] SYSTEM_REBOOT request from PID %u\n", pocket->pid);
    (void)proc;
    system_halt(true);
    return 0;
}

static int do_shutdown(Pocket* pocket, process_t* proc) {
    debug_printf("[Hardware] SYSTEM_SHUTDOWN request from PID %u\n", pocket->pid);
    (void)proc;
    system_halt(false);
    return 0;
}

int hardware_deck_handler(Pocket* pocket, process_t* proc) {
    if (!pocket) {
        return -1;
    }

    uint8_t opcode = pocket_get_opcode(pocket, pocket->current_prefix_idx);

    switch (opcode) {
        case HW_TIMER_GET_TICKS: {
            uint8_t* data = vmm_translate_user_addr(proc->cabin, pocket->data_addr, pocket->data_length);
            if (!data) return -1;

            uint64_t ticks = pit_get_ticks();
            deck_write_u64(data, ticks);
            return 0;
        }

        case HW_TIMER_GET_MS: {
            uint8_t* data = vmm_translate_user_addr(proc->cabin, pocket->data_addr, pocket->data_length);
            if (!data) return -1;

            uint64_t ticks = pit_get_ticks();
            uint32_t freq = pit_get_frequency();

            if (freq == 0) {
                pocket->error_code = 1;
                return -1;
            }

            uint64_t ms = (ticks * 1000) / freq;
            deck_write_u64(data, ms);
            return 0;
        }

        case HW_TIMER_GET_FREQ: {
            uint8_t* data = vmm_translate_user_addr(proc->cabin, pocket->data_addr, pocket->data_length);
            if (!data) return -1;

            uint32_t freq = pit_get_frequency();
            deck_write_u32(data, freq);
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
            uint8_t* data = vmm_translate_user_addr(proc->cabin, pocket->data_addr, pocket->data_length);
            if (!data) return -1;

            time_t t;
            rtc_get_boxtime(&t);
            deck_write_u64(data + 0, t.seconds);
            deck_write_u32(data + 8, t.nanosec);
            deck_write_u16(data + 12, t.year);
            data[14] = t.month;
            data[15] = t.day;
            data[16] = t.hour;
            data[17] = t.minute;
            data[18] = t.second;
            data[19] = t.weekday;
            return 0;
        }

        case HW_RTC_GET_UNIX64: {
            uint8_t* data = vmm_translate_user_addr(proc->cabin, pocket->data_addr, pocket->data_length);
            if (!data) return -1;

            uint64_t secs = rtc_get_unix64();
            deck_write_u64(data, secs);
            return 0;
        }

        case HW_RTC_GET_UPTIME: {
            uint8_t* data = vmm_translate_user_addr(proc->cabin, pocket->data_addr, pocket->data_length);
            if (!data) return -1;

            uint64_t ns = rtc_get_uptime_ns();
            deck_write_u64(data, ns);
            return 0;
        }

        case HW_PORT_INB: {
            uint8_t* data = vmm_translate_user_addr(proc->cabin, pocket->data_addr, pocket->data_length);
            if (!data) return -1;

            uint16_t port = deck_read_u16(data);
            uint8_t value = inb(port);
            data[0] = value;
            return 0;
        }

        case HW_PORT_OUTB: {
            uint8_t* data = vmm_translate_user_addr(proc->cabin, pocket->data_addr, pocket->data_length);
            if (!data) return -1;

            uint16_t port = deck_read_u16(data);
            uint8_t value = data[2];
            outb(port, value);
            data[0] = 0;
            return 0;
        }

        case HW_PORT_INW: {
            uint8_t* data = vmm_translate_user_addr(proc->cabin, pocket->data_addr, pocket->data_length);
            if (!data) return -1;

            uint16_t port = deck_read_u16(data);
            uint16_t value = inw(port);
            deck_write_u16(data, value);
            return 0;
        }

        case HW_PORT_OUTW: {
            uint8_t* data = vmm_translate_user_addr(proc->cabin, pocket->data_addr, pocket->data_length);
            if (!data) return -1;

            uint16_t port = deck_read_u16(data);
            uint16_t value = deck_read_u16(data + 2);
            outw(port, value);
            data[0] = 0;
            return 0;
        }

        case HW_PORT_INL: {
            uint8_t* data = vmm_translate_user_addr(proc->cabin, pocket->data_addr, pocket->data_length);
            if (!data) return -1;

            uint16_t port = deck_read_u16(data);
            uint32_t value = inl(port);
            deck_write_u32(data, value);
            return 0;
        }

        case HW_PORT_OUTL: {
            uint8_t* data = vmm_translate_user_addr(proc->cabin, pocket->data_addr, pocket->data_length);
            if (!data) return -1;

            uint16_t port = deck_read_u16(data);
            uint32_t value = deck_read_u32(data + 2);
            outl(port, value);
            data[0] = 0;
            return 0;
        }

        case HW_IRQ_ENABLE: {
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
            uint8_t* data = vmm_translate_user_addr(proc->cabin, pocket->data_addr, pocket->data_length);
            if (!data) return -1;

            uint32_t isr = irqchip_get_isr();
            deck_write_u32(data, isr);
            return 0;
        }

        case HW_IRQ_GET_IRR: {
            uint8_t* data = vmm_translate_user_addr(proc->cabin, pocket->data_addr, pocket->data_length);
            if (!data) return -1;

            uint32_t irr = irqchip_get_irr();
            deck_write_u32(data, irr);
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
            uint8_t* data = vmm_translate_user_addr(proc->cabin, pocket->data_addr, pocket->data_length);
            if (!data) return -1;

            hlt();
            data[0] = 0;
            return 0;
        }

        case HW_DISK_INFO: {
            uint8_t* data = vmm_translate_user_addr(proc->cabin, pocket->data_addr, pocket->data_length);
            if (!data) return -1;

            uint8_t is_master = data[0];
            ATADevice* device = is_master ? &ata_primary_master : &ata_primary_slave;

            data[0] = device->exists;
            memcpy(&data[1], device->model, 40);
            memcpy(&data[41], device->serial, 20);
            deck_write_u64(data + 61, device->total_sectors);
            deck_write_u64(data + 69, device->size_mb);
            return 0;
        }

        case HW_DISK_FLUSH: {
            uint8_t* data = vmm_translate_user_addr(proc->cabin, pocket->data_addr, pocket->data_length);
            if (!data) return -1;

            uint8_t is_master = data[0];
            int result = ata_flush_cache(is_master);
            data[0] = (result == 0) ? 0 : 1;
            return 0;
        }

        case HW_KEYBOARD_GETCHAR: {
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
            uint8_t* data = vmm_translate_user_addr(proc->cabin, pocket->data_addr, pocket->data_length);
            if (!data || pocket->data_length < 8) return -1;

            /* Protocol: data[0..1] = uint16_t max_length (LE), data[2] = echo_mode
               Response: data[0..3] = uint32_t length, data[4..] = string, last byte = status */
            uint16_t max_length = (uint16_t)data[0] | ((uint16_t)data[1] << 8);
            uint8_t echo_mode = data[2];

            /* Dynamic limit: pocket data area minus header (4) and status (1) and NUL (1) */
            uint32_t data_capacity = pocket->data_length - 6;
            if (data_capacity > KEYBOARD_LINE_BUFFER_SIZE - 1)
                data_capacity = KEYBOARD_LINE_BUFFER_SIZE - 1;
            if (max_length == 0 || max_length > data_capacity)
                max_length = (uint16_t)data_capacity;

            keyboard_set_echo(echo_mode != 0);

            char line_buffer[KEYBOARD_LINE_BUFFER_SIZE];
            memset(line_buffer, 0, (size_t)max_length + 1);

            int line_ready = keyboard_readline_async(line_buffer, max_length);

            if (line_ready) {
                size_t len = strlen(line_buffer);

                deck_write_u32(data, (uint32_t)len);

                size_t copy_len = len < data_capacity ? len : data_capacity;
                memcpy(&data[4], line_buffer, copy_len);
                data[4 + copy_len] = '\0';

                data[pocket->data_length - 1] = HW_KB_SUCCESS;
                return 0;
            } else {
                deck_write_u32(data, 0);
                data[pocket->data_length - 1] = HW_KB_WOULD_BLOCK;
                pocket->error_code = ERR_WOULD_BLOCK;
                return 0;
            }
        }

        case HW_KEYBOARD_STATUS: {
            uint8_t* data = vmm_translate_user_addr(proc->cabin, pocket->data_addr, pocket->data_length);
            if (!data) return -1;

            uint32_t available = keyboard_available();
            deck_write_u32(data + 0, available);

            uint32_t buffer_size = KEYBOARD_LINE_BUFFER_SIZE;
            deck_write_u32(data + 4, buffer_size);

            return 0;
        }

        case HW_VGA_PUTCHAR: {
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
            uint8_t* data = vmm_translate_user_addr(proc->cabin, pocket->data_addr, pocket->data_length);
            if (!data) return -1;

            vga_clear_to_eol();

            data[0] = VGA_SUCCESS;
            return 0;
        }

        case HW_VGA_GET_CURSOR: {
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
            uint8_t* data = vmm_translate_user_addr(proc->cabin, pocket->data_addr, pocket->data_length);
            if (!data) return -1;

            uint8_t color = vga_get_color();

            data[0] = VGA_SUCCESS;
            data[1] = color;
            return 0;
        }

        case HW_VGA_SCROLL_UP: {
            uint8_t* data = vmm_translate_user_addr(proc->cabin, pocket->data_addr, pocket->data_length);
            if (!data) return -1;

            vga_scroll_up();

            data[0] = VGA_SUCCESS;
            return 0;
        }

        case HW_VGA_NEWLINE: {
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
            uint8_t* data = vmm_translate_user_addr(proc->cabin, pocket->data_addr, pocket->data_length);
            if (!data) return -1;

            data[0] = VGA_SUCCESS;
            data[1] = VGA_WIDTH;
            data[2] = VGA_HEIGHT;
            return 0;
        }

        case HW_SYSTEM_REBOOT:
            return do_reboot(pocket, proc);

        case HW_SYSTEM_SHUTDOWN:
            return do_shutdown(pocket, proc);

        case HW_USB_INIT: {
            int result = xhci_init();

            if (result != 0) {
                pocket->error_code = USB_ERR_NO_CONTROLLER;
                return -1;
            }

            uint8_t* data = vmm_translate_user_addr(proc->cabin, pocket->data_addr, pocket->data_length);
            if (data) data[0] = 0x00;
            return 0;
        }

        case HW_USB_RESET: {
            xhci_controller_t* ctrl = xhci_get_controller();
            if (!ctrl) {
                pocket->error_code = USB_ERR_NOT_INITIALIZED;
                return -1;
            }

            int result = xhci_reset(ctrl);

            if (result != 0) {
                pocket->error_code = USB_ERR_RESET_TIMEOUT;
                return -1;
            }

            uint8_t* data = vmm_translate_user_addr(proc->cabin, pocket->data_addr, pocket->data_length);
            if (data) data[0] = 0x00;
            return 0;
        }

        case HW_USB_START: {
            xhci_controller_t* ctrl = xhci_get_controller();
            if (!ctrl) {
                pocket->error_code = USB_ERR_NOT_INITIALIZED;
                return -1;
            }

            int result = xhci_start(ctrl);

            if (result != 0) {
                pocket->error_code = USB_ERR_RESET_TIMEOUT;
                return -1;
            }

            uint8_t* data = vmm_translate_user_addr(proc->cabin, pocket->data_addr, pocket->data_length);
            if (data) data[0] = 0x00;
            return 0;
        }

        case HW_USB_STOP: {
            uint8_t* data = vmm_translate_user_addr(proc->cabin, pocket->data_addr, pocket->data_length);
            if (data) data[0] = 0x00;
            return 0;
        }

        case HW_USB_PORT_STATUS: {
            uint8_t* data = vmm_translate_user_addr(proc->cabin, pocket->data_addr, pocket->data_length);
            if (!data) return -1;

            uint8_t port = data[0];
            xhci_controller_t* ctrl = xhci_get_controller();

            if (!ctrl || !ctrl->initialized) {
                pocket->error_code = USB_ERR_NOT_INITIALIZED;
                return -1;
            }

            if (port == 0 || port > ctrl->max_ports) {
                pocket->error_code = USB_ERR_INVALID_PORT;
                return -1;
            }

            uint32_t portsc = xhci_get_port_status(ctrl, port);

            data[0] = 0x00;
            deck_write_u32(data + 1, portsc);
            return 0;
        }

        case HW_USB_PORT_RESET: {
            uint8_t* data = vmm_translate_user_addr(proc->cabin, pocket->data_addr, pocket->data_length);
            if (!data) return -1;

            uint8_t port = data[0];
            xhci_controller_t* ctrl = xhci_get_controller();

            if (!ctrl || !ctrl->initialized) {
                pocket->error_code = USB_ERR_NOT_INITIALIZED;
                return -1;
            }

            if (port == 0 || port > ctrl->max_ports) {
                pocket->error_code = USB_ERR_INVALID_PORT;
                return -1;
            }

            if (!xhci_port_has_device(ctrl, port)) {
                pocket->error_code = USB_ERR_NO_DEVICE;
                return -1;
            }

            int result = xhci_reset_port(ctrl, port);

            if (result != 0) {
                pocket->error_code = USB_ERR_RESET_TIMEOUT;
                return -1;
            }

            data[0] = 0x00;
            return 0;
        }

        case HW_USB_PORT_QUERY: {
            xhci_controller_t* ctrl = xhci_get_controller();

            if (!ctrl || !ctrl->initialized) {
                pocket->error_code = USB_ERR_NOT_INITIALIZED;
                return -1;
            }

            uint8_t* data = vmm_translate_user_addr(proc->cabin, pocket->data_addr, pocket->data_length);
            if (!data) return -1;

            data[0] = 0x00;
            data[1] = ctrl->max_ports;
            data[2] = ctrl->max_slots;
            data[3] = ctrl->irq_line;
            data[4] = ctrl->use_polling ? 1 : 0;
            return 0;
        }

        case HW_USB_ENUM_DEVICE: {
            uint8_t* data = vmm_translate_user_addr(proc->cabin, pocket->data_addr, pocket->data_length);
            if (!data) return -1;

            uint8_t port = data[0];

            xhci_controller_t* ctrl = xhci_get_controller();
            if (!ctrl || !ctrl->running) {
                pocket->error_code = USB_ERR_NO_CONTROLLER;
                return -1;
            }

            int result = xhci_enumerate_device(ctrl, port);

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
            uint8_t* data = vmm_translate_user_addr(proc->cabin, pocket->data_addr, pocket->data_length);
            if (!data) return -1;

            uint8_t slot_id = data[0];

            xhci_controller_t* ctrl = xhci_get_controller();
            if (!ctrl || !ctrl->running) {
                pocket->error_code = USB_ERR_NO_CONTROLLER;
                return -1;
            }

            xhci_device_slot_t* slot = xhci_get_device_slot(ctrl, slot_id);
            if (!slot) {
                pocket->error_code = USB_ERR_NO_DEVICE;
                return -1;
            }

            data[0] = 0x00;
            data[1] = slot->slot_id;
            data[2] = slot->port_num;
            data[3] = slot->state;
            return 0;
        }

        default: {
            pocket->error_code = 1;
            return -1;
        }
    }
}
