/*
 * Gangway — what the loader says, and where: the journal, which every line
 * goes into; COM1, driven by its own port writes and so a voice before the
 * firmware's tables are reached and after ExitBootServices; and, when the
 * loader stops, the screen it draws on itself (gangway_draw.c), or the
 * firmware's console where there is no framebuffer to draw on.
 */

#include "gangway.h"
#include "video_colors.h"

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
 * Only a loader that stops shows what it said: Panic draws the journal and
 * then why, because on a machine that will not boot the account is the whole
 * of the diagnosis.
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

/* Text down COM1, '\n' as "\r\n". */
static void Com1Show(const char *text, uint32_t bytes)
{
    for (uint32_t i = 0; i < bytes; i++) {
        if (text[i] == '\n') Com1Byte('\r');
        Com1Byte(text[i]);
    }
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

/* Fatal halt: the journal, then why — said apart from it, so a journal that
 * ran out of room cannot swallow it — on the screen in the loader's own
 * letters, the journal's last lines when it holds no more, and the whole of it
 * down COM1; on the firmware's console only where there is no framebuffer to
 * draw on. Then nothing more. */
void Panic(const char *msg)
{
    static const char lost_open[]  = "\n(and ";
    static const char lost_close[] = " bytes more were said than the journal had room for)\n";
    static const char fatal[]      = "\n[GANGWAY FATAL] ";

    char     count[10];
    uint32_t digits = 0;
    for (uint32_t n = g_journal_lost; n; n /= 10) digits++;
    for (uint32_t n = g_journal_lost, at = digits; at > 0; n /= 10) count[--at] = (char)('0' + n % 10);
    uint32_t msg_bytes = 0;
    while (msg[msg_bytes]) msg_bytes++;
    uint32_t noted = g_journal_lost ? 1u : 0u;

    const DrawPiece account[] = {
        { g_journal,  g_journal_used,                    VIDEO_ATTR_DEFAULT },
        { lost_open,  noted * (sizeof(lost_open) - 1u),  VIDEO_ATTR_WARNING },
        { count,      digits,                            VIDEO_ATTR_WARNING },
        { lost_close, noted * (sizeof(lost_close) - 1u), VIDEO_ATTR_WARNING },
        { fatal,      sizeof(fatal) - 1u,                VIDEO_ATTR_ERROR   },
        { msg,        msg_bytes,                         VIDEO_ATTR_ERROR   },
        { "\n",       1u,                                VIDEO_ATTR_ERROR   },
    };
    const uint32_t pieces = sizeof(account) / sizeof(account[0]);

    int drawn = DrawEnd(account, pieces);
    for (uint32_t i = 0; i < pieces; i++) {
        Com1Show(account[i].text, account[i].bytes);
        if (!drawn) ConsoleShow(account[i].text, account[i].bytes);
    }
    for (;;) {
        __asm__ volatile ("cli; hlt");
    }
}
