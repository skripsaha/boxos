/*
 * Gangway — what the loader says, and where: COM1, driven by its own port
 * writes and so the one voice before the firmware's console is reached and
 * after ExitBootServices takes it away; and the firmware's console in between.
 */

#include "gangway.h"

/* =========================================================================
 * COM1 serial debug output (raw port I/O — works before UEFI console)
 *
 * Use these before g_st is valid.  UEFI runs at ring 0, so outb is legal.
 * QEMU -serial stdio will show this output on stdout.
 * ========================================================================= */

void Com1Init(void)
{
    uint16_t p;
    uint8_t  v;
    p = 0x3F9; v = 0x00; __asm__ volatile("outb %0,%1"::"a"(v),"Nd"(p));  /* IER=0     */
    p = 0x3FB; v = 0x80; __asm__ volatile("outb %0,%1"::"a"(v),"Nd"(p));  /* DLAB=1    */
    p = 0x3F8; v = 0x01; __asm__ volatile("outb %0,%1"::"a"(v),"Nd"(p));  /* div lo=1  */
    p = 0x3F9; v = 0x00; __asm__ volatile("outb %0,%1"::"a"(v),"Nd"(p));  /* div hi=0  */
    p = 0x3FB; v = 0x03; __asm__ volatile("outb %0,%1"::"a"(v),"Nd"(p));  /* 8N1       */
    p = 0x3FC; v = 0x03; __asm__ volatile("outb %0,%1"::"a"(v),"Nd"(p));  /* RTS+DTR   */
}

static void Com1Byte(char c)
{
    uint8_t  lsr;
    uint16_t lsr_port = 0x3FD;
    uint16_t dat_port = 0x3F8;
    do {
        __asm__ volatile("inb %1,%0":"=a"(lsr):"Nd"(lsr_port));
    } while (!(lsr & 0x20));
    uint8_t b = (uint8_t)c;
    __asm__ volatile("outb %0,%1"::"a"(b),"Nd"(dat_port));
}

void Com1Str(const char *s)
{
    for (; *s; s++) Com1Byte(*s);
}

/* =========================================================================
 * Utility: print to UEFI console
 * ========================================================================= */

/* Print a narrow ASCII string by widening to CHAR16 in a stack buffer,
 * then one output_string call. Pre-fix did one boot-service call per
 * character, which on serial console + heavy logging meant each line
 * cost ~80 round-trips through firmware. Batching is a ~10x reduction
 * in boot-time wall clock when the firmware funnels console through a
 * 115200-baud serial port (real HP/Lenovo enterprise default).
 *
 * Buffer is on stack so re-entrant from any context; sized for the
 * longest line we emit (~96 chars after rounding up). Strings longer
 * than the buffer are chunked. */
void Print(const char *msg)
{
    if (!g_st || !g_st->con_out || !msg) return;

    CHAR16 buf[129];   /* 128 chars + terminator */
    size_t pos = 0;
    for (size_t i = 0; msg[i]; i++) {
        buf[pos++] = (CHAR16)(unsigned char)msg[i];
        if (pos == 128) {
            buf[128] = 0;
            g_st->con_out->output_string(g_st->con_out, buf);
            pos = 0;
        }
    }
    if (pos > 0) {
        buf[pos] = 0;
        g_st->con_out->output_string(g_st->con_out, buf);
    }
}

void PrintHex64(uint64_t v)
{
    char buf[19];
    buf[0]  = '0'; buf[1] = 'x';
    for (int i = 15; i >= 2; i--) {
        uint8_t nibble = (uint8_t)(v & 0xF);
        buf[i] = (char)(nibble < 10 ? '0' + nibble : 'a' + nibble - 10);
        v >>= 4;
    }
    buf[18] = '\0';
    Print(buf);
}

void PrintDec(uint64_t v)
{
    if (v == 0) { Print("0"); return; }
    char buf[21];
    int  pos = 20;
    buf[pos]  = '\0';
    while (v && pos > 0) {
        buf[--pos] = (char)('0' + v % 10);
        v /= 10;
    }
    Print(buf + pos);
}

/* Fatal halt: print error, spin forever. */
void Panic(const char *msg)
{
    Print("\r\n[GANGWAY FATAL] ");
    Print(msg);
    Print("\r\n");
    for (;;) {
        __asm__ volatile ("cli; hlt");
    }
}
