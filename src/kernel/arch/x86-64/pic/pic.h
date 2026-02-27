#ifndef PIC_H
#define PIC_H

#include "ktypes.h"

// PIC порты
#define PIC1_COMMAND    0x20
#define PIC1_DATA       0x21
#define PIC2_COMMAND    0xA0
#define PIC2_DATA       0xA1

// PIC команды
#define PIC_EOI         0x20    // End of Interrupt

// Инициализация PIC
#define ICW1_ICW4       0x01    // ICW4 (not) needed
#define ICW1_SINGLE     0x02    // Single (cascade) mode
#define ICW1_INTERVAL4  0x04    // Call address interval 4 (8)
#define ICW1_LEVEL      0x08    // Level triggered (edge) mode
#define ICW1_INIT       0x10    // Initialization - required!

#define ICW4_8086       0x01    // 8086/88 (MCS-80/85) mode
#define ICW4_AUTO       0x02    // Auto (normal) EOI
#define ICW4_BUF_SLAVE  0x08    // Buffered mode/slave
#define ICW4_BUF_MASTER 0x0C    // Buffered mode/master
#define ICW4_SFNM       0x10    // Special fully nested (not)

// IRQ маски
#define PIC_ALL_IRQS_DISABLED   0xFF
#define PIC_ALL_IRQS_ENABLED    0x00

// Функции
void pic_init(void);
void pic_disable(void);
void pic_enable_irq(uint8_t irq);
void pic_disable_irq(uint8_t irq);
void pic_send_eoi(uint8_t irq);
void pic_set_mask(uint8_t mask1, uint8_t mask2);
uint16_t pic_get_irr(void);
uint16_t pic_get_isr(void);
void pic_test(void);

#endif // PIC_H