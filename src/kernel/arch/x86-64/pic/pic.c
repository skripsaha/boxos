#include "pic.h"
#include "io.h"
#include "klib.h"

static uint8_t pic1_mask = PIC_ALL_IRQS_DISABLED;
static uint8_t pic2_mask = PIC_ALL_IRQS_DISABLED;

/* OCW3 read-register commands (Intel 8259A datasheet, OCW3): RR=1 with
 * RIS=0 puts the Interrupt Request Register under the next read of the
 * command port, RIS=1 the In-Service Register. The selection is LATCHED in
 * the chip, so whoever changes it owes the file its previous setting back. */
#define PIC_OCW3_READ_IRR   0x0A
#define PIC_OCW3_READ_ISR   0x0B

/*
 * A pause the 8259A is allowed to need, bought with a bus cycle.
 *
 * The four ICW bytes are one sequence inside the chip, and on a fair number
 * of chipsets the chip (or the LPC bridge in front of it) does not latch one
 * byte before the next arrives. Linux inserts a delay after EVERY byte of
 * that sequence — asm/i8259.h, outb_pic(): "delay for some accesses to PIC on
 * motherboard or in chipset must be at least one microsecond, so be safe
 * here", udelay(2) — and uses plain outb everywhere else, which is why the
 * delay appears here and not on the EOI path.
 *
 * udelay is not available to us: pic_init() runs from kernel_main long before
 * the PIT or the TSC has been calibrated, so there is no proven clock to ask.
 * The delay is therefore a real I/O transaction rather than a counted loop —
 * a write to the POST diagnostic port 0x80, which every PC decodes onto the
 * slow LPC/ISA side and which costs about a microsecond there. Nothing is
 * waited FOR and nothing is abandoned: this is a bus round trip, not a
 * timeout.
 */
#define PIC_IO_DELAY_PORT   0x80

static inline void pic_io_wait(void) {
    outb(PIC_IO_DELAY_PORT, 0);
}

void pic_init(void) {
    debug_printf("[PIC] Initializing Programmable Interrupt Controller...\n");

    outb(PIC1_COMMAND, ICW1_INIT | ICW1_ICW4);
    pic_io_wait();
    outb(PIC2_COMMAND, ICW1_INIT | ICW1_ICW4);
    pic_io_wait();

    // ICW2: Master PIC IRQ 0-7 -> vectors 32-39, Slave IRQ 8-15 -> vectors 40-47
    outb(PIC1_DATA, 32);
    pic_io_wait();
    outb(PIC2_DATA, 40);
    pic_io_wait();

    outb(PIC1_DATA, 4);     // Slave at IRQ2
    pic_io_wait();
    outb(PIC2_DATA, 2);     // Slave cascade identity
    pic_io_wait();

    outb(PIC1_DATA, ICW4_8086);
    pic_io_wait();
    outb(PIC2_DATA, ICW4_8086);
    pic_io_wait();

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

/*
 * Did this line really go into service, or did the 8259A answer for nobody?
 *
 * Intel 8259A datasheet, interrupt sequence: if the request that raised INT
 * is gone by the time the second INTA arrives — a glitch, an unterminated
 * line, a device that withdrew between INTR and INTA — the chip still has to
 * hand the CPU a vector, and the one it hands over is IR7's. What it does
 * NOT do is set the In-Service bit for IR7, and that omission is the only
 * way to tell the invented interrupt from a real one: read the In-Service
 * Register and look.
 *
 * Asking matters because this driver ends a line with a NON-SPECIFIC EOI
 * (OCW2 = 0x20), which resets the highest-priority bit currently set in the
 * ISR — not the bit named by the vector that was delivered. On an invented
 * IR7 there is no bit of ours to reset, so that EOI would clear some other
 * device's in-service bit instead, and that device's next interrupt would
 * never be delivered.
 *
 * Linux asks the same question in the same place, i8259A_irq_real()
 * (arch/x86/kernel/i8259.c), and restores the IRR selection afterwards.
 */
static bool pic_irq_in_service(uint8_t irq) {
    uint16_t port = (irq < 8) ? PIC1_COMMAND : PIC2_COMMAND;
    uint8_t  bit  = (uint8_t)(1u << (irq & 7u));

    outb(port, PIC_OCW3_READ_ISR);
    uint8_t isr = inb(port);
    outb(port, PIC_OCW3_READ_IRR);

    return (isr & bit) != 0;
}

void pic_send_eoi(uint8_t irq) {
    /* IR7 on either chip is the vector the 8259A invents when it has nothing
     * to report, so those two lines — IRQ 7 and IRQ 15 — are the only ones
     * worth the two extra I/O cycles it costs to ask. */
    if ((irq == 7 || irq == 15) && !pic_irq_in_service(irq)) {
        /* Once per line, as Linux reports it: a spurious interrupt is a sign
         * of a hardware fault and the operator needs to know WHICH line, but
         * a line that glitches once tends to glitch again and a message per
         * occurrence would bury the log it belongs in. */
        static uint16_t told_spurious;
        uint16_t bit = (uint16_t)(1u << irq);
        if (!(told_spurious & bit)) {
            told_spurious |= bit;
            kprintf("[PIC] %[E]spurious IRQ %u: no in-service bit on the %s "
                    "PIC — EOI withheld from it%[D]\n",
                    irq, irq < 8 ? "master" : "slave");
        }

        /* The master is not told that the slave invented the vector: it took
         * a real cascade interrupt on IR2 and its in-service bit for IR2 is
         * set. That one is ended; the chip that invented the vector has
         * nothing to end. */
        if (irq >= 8) {
            outb(PIC1_COMMAND, PIC_EOI);
        }
        return;
    }

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
    outb(PIC1_COMMAND, PIC_OCW3_READ_IRR);
    outb(PIC2_COMMAND, PIC_OCW3_READ_IRR);
    return (inb(PIC2_COMMAND) << 8) | inb(PIC1_COMMAND);
}

uint16_t pic_get_isr(void) {
    outb(PIC1_COMMAND, PIC_OCW3_READ_ISR);
    outb(PIC2_COMMAND, PIC_OCW3_READ_ISR);
    uint16_t isr = (uint16_t)((inb(PIC2_COMMAND) << 8) | inb(PIC1_COMMAND));
    /* OCW3's read-register selection is latched: leave the chips where the
     * rest of this file expects to find them. */
    outb(PIC1_COMMAND, PIC_OCW3_READ_IRR);
    outb(PIC2_COMMAND, PIC_OCW3_READ_IRR);
    return isr;
}
