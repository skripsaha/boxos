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
 * The journal
 *
 * Gangway boots quietly: what it says is kept, not printed. The firmware's
 * console is the screen in front of the operator, and on a desk machine it
 * still shows the board maker's logo — which is what should stay there until
 * the display program takes the glass. So every line goes into the journal,
 * and the kernel puts the journal into its log ring before it does anything
 * else (boarding_pass.h, BoardingJournal): the loader's lines open the log —
 * on the serial wire, in logsave, in the display — beside all that follows.
 *
 * Only a loader that stops shows what it said: Panic puts the whole journal on
 * the firmware's console and then why, because on a machine that will not
 * boot the account is the whole of the diagnosis.
 *
 * '\n' alone ends a line here, as in the kernel's ring; the '\r' a firmware
 * console wants is put back when the journal is shown.
 * ========================================================================= */

/* A boot says about 3 KiB here; one that probes a dozen media and passes over
 * every one of them says a few times that. What does not fit is counted, and
 * the count travels with the journal. */
#define JOURNAL_BYTES (32u * 1024u)

static char     g_journal[JOURNAL_BYTES];
static uint32_t g_journal_used = 0;
static uint32_t g_journal_lost = 0;

void Print(const char *msg)
{
    if (!msg) return;
    for (; *msg; msg++) {
        if (*msg == '\r') continue;
        if (g_journal_used < JOURNAL_BYTES) g_journal[g_journal_used++] = *msg;
        else                                g_journal_lost++;
    }
}

const char *JournalText(uint32_t *bytes, uint32_t *lost)
{
    *bytes = g_journal_used;
    *lost  = g_journal_lost;
    return g_journal;
}

/* Text on the firmware's console, '\n' as "\r\n", a batch to one
 * output_string call: a call a character cost ~80 firmware round trips a line
 * on a board that funnels its console through a 115200-baud serial port. */
static void ConsoleShow(const char *text, uint32_t bytes)
{
    if (!g_st || !g_st->con_out) return;

    CHAR16 buf[130];   /* 128, and the "\r\n" a last '\n' may make 129, and the terminator */
    size_t pos = 0;
    for (uint32_t i = 0; i < bytes; i++) {
        if (text[i] == '\n') buf[pos++] = '\r';
        buf[pos++] = (CHAR16)(unsigned char)text[i];
        if (pos >= 128) {
            buf[pos] = 0;
            g_st->con_out->output_string(g_st->con_out, buf);
            pos = 0;
        }
    }
    if (pos > 0) {
        buf[pos] = 0;
        g_st->con_out->output_string(g_st->con_out, buf);
    }
}

static void ConsoleSay(const char *text)
{
    uint32_t n = 0;
    while (text[n]) n++;
    ConsoleShow(text, n);
}

/* "0x" and all sixteen digits. It wrote fourteen once, from the top of a
 * nineteen-byte buffer it never filled — the high byte of every value was
 * dropped and two bytes of whatever the stack held were said after it. */
void PrintHex64(uint64_t v)
{
    char buf[19];
    buf[0]  = '0'; buf[1] = 'x';
    for (int i = 17; i >= 2; i--) {
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

/* Fatal halt: the journal on the firmware's console, then why — said there
 * directly, so a journal that ran out of room cannot swallow it — and nothing
 * more. */
void Panic(const char *msg)
{
    ConsoleShow(g_journal, g_journal_used);
    if (g_journal_lost) {
        char count[11];
        int  at = 10;
        uint32_t n = g_journal_lost;
        count[at] = '\0';
        do { count[--at] = (char)('0' + n % 10); n /= 10; } while (n && at > 0);
        ConsoleSay("\n(and ");
        ConsoleSay(count + at);
        ConsoleSay(" bytes more were said than the journal had room for)\n");
    }
    ConsoleSay("\n[GANGWAY FATAL] ");
    ConsoleSay(msg);
    ConsoleSay("\n");
    for (;;) {
        __asm__ volatile ("cli; hlt");
    }
}
