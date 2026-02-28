#include "pic.h"
#include "io.h"
#include "klib.h"

static uint8_t pic1_mask = PIC_ALL_IRQS_DISABLED;
static uint8_t pic2_mask = PIC_ALL_IRQS_DISABLED;

void pic_init(void) {
    debug_printf("[PIC] Initializing Programmable Interrupt Controller...\n");
    
    uint8_t a1 = inb(PIC1_DATA);
    uint8_t a2 = inb(PIC2_DATA);
    
    debug_printf("[PIC] Current masks: PIC1=0x%02x, PIC2=0x%02x\n", a1, a2);
    
    // Initialize in cascade mode
    outb(PIC1_COMMAND, ICW1_INIT | ICW1_ICW4);
    outb(PIC2_COMMAND, ICW1_INIT | ICW1_ICW4);

    // ICW2: Master PIC IRQ 0-7 -> vectors 32-39, Slave IRQ 8-15 -> vectors 40-47
    outb(PIC1_DATA, 32);
    outb(PIC2_DATA, 40);

    // ICW3: Cascade configuration
    outb(PIC1_DATA, 4);     // Slave at IRQ2
    outb(PIC2_DATA, 2);     // Slave cascade identity

    outb(PIC1_DATA, ICW4_8086);
    outb(PIC2_DATA, ICW4_8086);
    
    debug_printf("[PIC] PIC initialized with vectors 32-47\n");
    debug_printf("[PIC] Master PIC (IRQ 0-7)  -> vectors 32-39\n");
    debug_printf("[PIC] Slave PIC  (IRQ 8-15) -> vectors 40-47\n");
    
    pic_set_mask(PIC_ALL_IRQS_DISABLED, PIC_ALL_IRQS_DISABLED);
    
    debug_printf("[PIC] All IRQs disabled by default\n");
    debug_printf("[PIC] %[S]PIC initialized successfully!%[D]\n");
}

void pic_disable(void) {
    debug_printf("[PIC] Disabling all PIC interrupts...\n");
    pic_set_mask(PIC_ALL_IRQS_DISABLED, PIC_ALL_IRQS_DISABLED);
}

void pic_enable_irq(uint8_t irq) {
    if (irq >= 16) {
        debug_printf("[PIC] %[E]Invalid IRQ %d (must be 0-15)%[D]\n", irq);
        return;
    }
    
    if (irq < 8) {
        pic1_mask &= ~(1 << irq);
        outb(PIC1_DATA, pic1_mask);
        debug_printf("[PIC] Enabled IRQ %d on Master PIC (mask: 0x%02x)\n", irq, pic1_mask);
    } else {
        uint8_t slave_irq = irq - 8;
        pic2_mask &= ~(1 << slave_irq);
        outb(PIC2_DATA, pic2_mask);

        // Also enable IRQ 2 (cascade line) on master
        pic1_mask &= ~(1 << 2);
        outb(PIC1_DATA, pic1_mask);
        
        debug_printf("[PIC] Enabled IRQ %d on Slave PIC (mask: 0x%02x)\n", irq, pic2_mask);
        debug_printf("[PIC] Enabled IRQ 2 (cascade) on Master PIC (mask: 0x%02x)\n", pic1_mask);
    }
}

void pic_disable_irq(uint8_t irq) {
    if (irq >= 16) {
        debug_printf("[PIC] %[E]Invalid IRQ %d (must be 0-15)%[D]\n", irq);
        return;
    }
    
    if (irq < 8) {
        pic1_mask |= (1 << irq);
        outb(PIC1_DATA, pic1_mask);
        debug_printf("[PIC] Disabled IRQ %d on Master PIC (mask: 0x%02x)\n", irq, pic1_mask);
    } else {
        uint8_t slave_irq = irq - 8;
        pic2_mask |= (1 << slave_irq);
        outb(PIC2_DATA, pic2_mask);
        debug_printf("[PIC] Disabled IRQ %d on Slave PIC (mask: 0x%02x)\n", irq, pic2_mask);
    }
}

void pic_send_eoi(uint8_t irq) {
    if (irq >= 8) {
        outb(PIC2_COMMAND, PIC_EOI);  // Send EOI to slave PIC
    }
    outb(PIC1_COMMAND, PIC_EOI);      // Send EOI to master PIC
}

void pic_set_mask(uint8_t mask1, uint8_t mask2) {
    pic1_mask = mask1;
    pic2_mask = mask2;
    outb(PIC1_DATA, mask1);
    outb(PIC2_DATA, mask2);
    debug_printf("[PIC] Set masks: Master=0x%02x, Slave=0x%02x\n", mask1, mask2);
}

uint16_t pic_get_irr(void) {
    outb(PIC1_COMMAND, 0x0A);
    outb(PIC2_COMMAND, 0x0A);
    return (inb(PIC2_COMMAND) << 8) | inb(PIC1_COMMAND);
}

uint16_t pic_get_isr(void) {
    outb(PIC1_COMMAND, 0x0B);
    outb(PIC2_COMMAND, 0x0B);
    return (inb(PIC2_COMMAND) << 8) | inb(PIC1_COMMAND);
}

void pic_test(void) {
    debug_printf("[PIC] %[H]Testing PIC configuration...%[D]\n");
    
    uint8_t master_mask = inb(PIC1_DATA);
    uint8_t slave_mask = inb(PIC2_DATA);
    
    debug_printf("[PIC] Current masks: Master=0x%02x, Slave=0x%02x\n", master_mask, slave_mask);
    debug_printf("[PIC] Expected masks: Master=0x%02x, Slave=0x%02x\n", pic1_mask, pic2_mask);
    
    if (master_mask == pic1_mask && slave_mask == pic2_mask) {
        debug_printf("[PIC] %[S]Mask verification: PASSED%[D]\n");
    } else {
        debug_printf("[PIC] %[E]Mask verification: FAILED%[D]\n");
    }
    
    uint16_t irr = pic_get_irr();
    uint16_t isr = pic_get_isr();
    
    debug_printf("[PIC] IRR (pending interrupts): 0x%04x\n", irr);
    debug_printf("[PIC] ISR (in-service interrupts): 0x%04x\n", isr);
    
    debug_printf("[PIC] %[H]Enabling Timer (IRQ 0) and Keyboard (IRQ 1)...%[D]\n");
    pic_enable_irq(0);
    pic_enable_irq(1);

    master_mask = inb(PIC1_DATA);
    debug_printf("[PIC] Master mask after enabling IRQ 0,1: 0x%02x\n", master_mask);

    if ((master_mask & 0x03) == 0) {
        debug_printf("[PIC] %[S]IRQ enabling test: PASSED%[D]\n");
    } else {
        debug_printf("[PIC] %[E]IRQ enabling test: FAILED%[D]\n");
    }
    
    debug_printf("[PIC] %[S]PIC test completed!%[D]\n");
    debug_printf("[PIC] %[W]Timer and Keyboard interrupts are now enabled%[D]\n");
}