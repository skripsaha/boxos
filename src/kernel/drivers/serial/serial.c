#include "serial.h"
#include "io.h"
#include "klib.h"
#include "klib_logring.h"
#include "keyboard.h"
#include "idt.h"
#include "irqchip.h"

#define SERIAL_DATA          0
#define SERIAL_INT_ENABLE    1
#define SERIAL_INT_ID        2
#define SERIAL_FIFO_CTRL     2
#define SERIAL_LINE_CTRL     3
#define SERIAL_MODEM_CTRL    4
#define SERIAL_LINE_STATUS   5
#define SERIAL_MODEM_STATUS  6

#define IER_ERBFI            0x01
#define IER_ETBEI            0x02
#define LSR_DATA_READY       0x01
#define LSR_THRE             0x20
#define IIR_NONE_PENDING     0x01
#define IIR_ID_MASK          0x0E
#define IIR_THRE             0x02
#define IIR_RX               0x04
#define IIR_RX_TIMEOUT       0x0C
#define IIR_LINE_STATUS      0x06
#define IIR_FIFO_ENABLED     0xC0

#define SERIAL_COM1_GSI  4

static spinlock_t g_serial_lock;

static bool     g_wire_present;
static bool     g_wire_live;
static uint32_t g_wire_fifo;
static uint64_t g_wire_pos;
static bool     g_wire_lf_owed;
static uint64_t g_wire_behind;

static volatile uint32_t g_wire_idle;

static char     g_wire_notice[80];
static uint32_t g_wire_notice_len;
static uint32_t g_wire_notice_pos;

static inline bool transmitter_empty(void)
{
    return (inb(SERIAL_PORT_COM1 + SERIAL_LINE_STATUS) & LSR_THRE) != 0;
}

static inline void put_wire(char c)
{
    outb(SERIAL_PORT_COM1 + SERIAL_DATA, (uint8_t)c);
}

void serial_init(void)
{
    spinlock_init(&g_serial_lock);

    outb(SERIAL_PORT_COM1 + SERIAL_INT_ENABLE, 0x00);
    outb(SERIAL_PORT_COM1 + SERIAL_LINE_CTRL, 0x80);
    outb(SERIAL_PORT_COM1 + SERIAL_DATA, 0x01);
    outb(SERIAL_PORT_COM1 + SERIAL_INT_ENABLE, 0x00);
    outb(SERIAL_PORT_COM1 + SERIAL_LINE_CTRL, 0x03);
    outb(SERIAL_PORT_COM1 + SERIAL_FIFO_CTRL, 0xC7);
    outb(SERIAL_PORT_COM1 + SERIAL_MODEM_CTRL, 0x0B);

    outb(SERIAL_PORT_COM1 + SERIAL_MODEM_CTRL, 0x1E);
    outb(SERIAL_PORT_COM1 + SERIAL_DATA, 0xAE);
    g_wire_present = inb(SERIAL_PORT_COM1 + SERIAL_DATA) == 0xAE;
    outb(SERIAL_PORT_COM1 + SERIAL_MODEM_CTRL, 0x0F);

    uint8_t iir = inb(SERIAL_PORT_COM1 + SERIAL_INT_ID);
    g_wire_fifo = ((iir & IIR_FIFO_ENABLED) == IIR_FIFO_ENABLED) ? 16u : 1u;
    g_wire_idle = 1u;
}


static bool wire_pending(void)
{
    return g_wire_notice_pos < g_wire_notice_len ||
           g_wire_lf_owed ||
           g_wire_pos < LogRingWritten();
}

static void wire_mind_the_gap(void)
{
    uint64_t written = LogRingWritten();
    uint64_t oldest  = written > (uint64_t)LOGRING_CAPACITY
                     ? written - (uint64_t)LOGRING_CAPACITY : 0;
    if (g_wire_pos >= oldest) return;

    uint64_t gap = oldest - g_wire_pos;
    g_wire_behind  += gap;
    g_wire_pos      = oldest;
    g_wire_lf_owed  = false;

    if (g_wire_notice_pos < g_wire_notice_len) return;
    g_wire_notice_len = (uint32_t)ksnprintf(g_wire_notice, sizeof(g_wire_notice),
                                            "\r\n[WIRE] %lu byte(s) fell behind "
                                            "(%lu since boot)\r\n",
                                            (unsigned long)gap,
                                            (unsigned long)g_wire_behind);
    g_wire_notice_pos = 0;
}

static bool wire_emit_one(void)
{
    if (g_wire_notice_pos < g_wire_notice_len) {
        put_wire(g_wire_notice[g_wire_notice_pos++]);
        return true;
    }
    if (g_wire_lf_owed) {
        put_wire('\n');
        g_wire_lf_owed = false;
        g_wire_pos++;
        return true;
    }
    if (g_wire_pos >= LogRingWritten()) return false;

    char c = LogRingByteAt(g_wire_pos);
    if (c == '\n') {
        put_wire('\r');
        g_wire_lf_owed = true;
        return true;
    }
    put_wire(c);
    g_wire_pos++;
    return true;
}

static void wire_pump_locked(void)
{
    for (;;) {
        wire_mind_the_gap();
        if (!transmitter_empty()) return;

        uint32_t room = g_wire_fifo;
        while (room > 0 && wire_emit_one()) room--;
        if (room == 0) return;

        __atomic_store_n(&g_wire_idle, 1u, __ATOMIC_SEQ_CST);
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        if (!wire_pending()) return;
        __atomic_store_n(&g_wire_idle, 0u, __ATOMIC_SEQ_CST);
    }
}

static void wire_drain_locked(void)
{
    __atomic_store_n(&g_wire_idle, 0u, __ATOMIC_SEQ_CST);
    while (wire_pending()) {
        wire_pump_locked();
        while (wire_pending() && !transmitter_empty()) {
            asm volatile("pause");
        }
    }
}

void WireKick(void)
{
    if (!g_wire_present) return;

    if (!g_wire_live) {
        spin_lock(&g_serial_lock);
        wire_drain_locked();
        spin_unlock(&g_serial_lock);
        return;
    }

    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    if (!__atomic_load_n(&g_wire_idle, __ATOMIC_SEQ_CST)) return;

    spin_lock(&g_serial_lock);
    __atomic_store_n(&g_wire_idle, 0u, __ATOMIC_SEQ_CST);
    wire_pump_locked();
    spin_unlock(&g_serial_lock);
}

void WireDrain(void)
{
    if (!g_wire_present) return;
    spin_lock(&g_serial_lock);
    wire_drain_locked();
    spin_unlock(&g_serial_lock);
}

void WireForceRelease(void)
{
    LogRingForceRelease();
    spin_force_release(&g_serial_lock);
}


static void serial_com1_irq(void)
{
    for (;;) {
        uint8_t iir = inb(SERIAL_PORT_COM1 + SERIAL_INT_ID);
        if (iir & IIR_NONE_PENDING) return;

        switch (iir & IIR_ID_MASK) {
        case IIR_THRE:
            spin_lock(&g_serial_lock);
            __atomic_store_n(&g_wire_idle, 0u, __ATOMIC_SEQ_CST);
            wire_pump_locked();
            spin_unlock(&g_serial_lock);
            break;

        case IIR_RX:
        case IIR_RX_TIMEOUT:
            while (inb(SERIAL_PORT_COM1 + SERIAL_LINE_STATUS) & LSR_DATA_READY) {
                char c = (char)inb(SERIAL_PORT_COM1 + SERIAL_DATA);
                if (c == '\r') c = '\n';
                keyboard_inject(&c, 1);
            }
            break;

        case IIR_LINE_STATUS:
            (void)inb(SERIAL_PORT_COM1 + SERIAL_LINE_STATUS);
            break;

        default:
            (void)inb(SERIAL_PORT_COM1 + SERIAL_MODEM_STATUS);
            break;
        }
    }
}

void serial_console_init(void)
{
    if (!g_wire_present) return;

    outb(SERIAL_PORT_COM1 + SERIAL_FIFO_CTRL, 0x03);
    irq_register_handler(SERIAL_COM1_GSI, serial_com1_irq);
    irqchip_enable_irq(SERIAL_COM1_GSI);

    spin_lock(&g_serial_lock);
    g_wire_live = true;
    outb(SERIAL_PORT_COM1 + SERIAL_INT_ENABLE, IER_ERBFI | IER_ETBEI);
    __atomic_store_n(&g_wire_idle, 0u, __ATOMIC_SEQ_CST);
    wire_pump_locked();
    spin_unlock(&g_serial_lock);
}