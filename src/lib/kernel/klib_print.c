/* klib_print.c — formatted output for the kernel.
 *
 * Single source of truth for `%`-format parsing lives in `kvformat()`, which
 * runs against a `kfmt_ops_t` callback set. Two callback sets are wired:
 *   RING_OPS  emits into the log ring, each byte with its Tone
 *   BUF_OPS   emits into a bounded char buffer
 *
 * Why callback-driven rather than two parsers: the legacy split parser had
 * silently divergent feature sets — kvsnprintf knew %d/%u/%x/%s/%c only,
 * kprintf knew the full %lx/%p/%zu/%[X] set, and `panic()` calls into
 * kvsnprintf with format strings that include %lx and %p (then kprintf the
 * result). On the legacy code the panic banner contained literal "%lx" in
 * place of an address — fatal for real-HW post-mortem diagnostics. Now both
 * surfaces walk the same parser so feature parity is structural.
 *
 * kprintf draws NOTHING. What the kernel says goes into the log ring and only
 * there (klib_logring.h): the display program draws it in its order with
 * everything else, and while no program holds the screen the Emergency Screen
 * follows the ring onto the glass (emergency_screen.c) — from here, at the end
 * of each kprintf, under the same lock.
 *
 * Locking: kprintf serialises behind `g_kprintf_lock` (IRQ-safe spinlock),
 * which `console_lock_acquire/release` expose to the other hand that says
 * things into the ring (hw.log.say) and to whoever changes what the Emergency
 * Screen shows. The lock is BSS-zero before mem_init() runs spinlock_init();
 * BSS-zero happens to mean unlocked for the current spinlock_t layout. */
#include "klib.h"
#include "klib_logring.h"
#include "emergency_screen.h"
#include "atomics.h"
#include "cpu_calibrate.h"
#include "klib_panic.h" /* PanicClaimForMessage, PanicEndAccount — one claim, every other core stopped */

/* Module-local state — neither escapes outside this TU. */
static spinlock_t g_kprintf_lock;

/* The Tone the next byte of this kprintf is said in. Under g_kprintf_lock;
 * every kprintf begins PLAIN (boxos_log_tone.h). */
static uint8_t g_tone;

void console_lock_acquire(void)
{
    spin_lock(&g_kprintf_lock);
    PanicStandIfClaimedElsewhere(); /* the console is the panic's alone — see kvprintf */
}
void console_lock_release(void) { spin_unlock(&g_kprintf_lock); }

/* mem_init() runs before any kprintf-class call that needs the lock to be
 * formally initialised, so expose a hook it can drive. */
void klib_print_lock_init(void)
{
    spinlock_init(&g_kprintf_lock);
    LogRingLockInit();
}

/* ============================================================================
 *  Unified formatter
 * ========================================================================== */

typedef int  (*kfmt_emit_t)(void *ctx, char c);
typedef void (*kfmt_tone_t)(void *ctx, uint8_t tone);

typedef struct
{
    kfmt_emit_t emit_char;
    kfmt_tone_t set_tone;   /* NULL → tone escapes are no-ops */
} kfmt_ops_t;

static int kvformat(const kfmt_ops_t *ops, void *ctx,
                    const char *fmt, va_list args)
{
    int count = 0;

    while (*fmt)
    {
        if (*fmt != '%')
        {
            if (!ops->emit_char(ctx, *fmt))
                return count;
            ++count;
            ++fmt;
            continue;
        }

        ++fmt;  /* past '%' */

        /* BoxOS extensions: %[X] for tone/codepoint. The buffer emitter
         * supplies a NULL set_tone, so tone escapes drop silently when
         * formatting into a string. */
        if (*fmt == '[')
        {
            ++fmt;
            switch (*fmt)
            {
            case 'E':
                if (ops->set_tone) ops->set_tone(ctx, LOG_TONE_ERROR);
                break;
            case 'S':
                if (ops->set_tone) ops->set_tone(ctx, LOG_TONE_SUCCESS);
                break;
            case 'H':
                if (ops->set_tone) ops->set_tone(ctx, LOG_TONE_HINT);
                break;
            case 'D':
                if (ops->set_tone) ops->set_tone(ctx, LOG_TONE_PLAIN);
                break;
            case 'W':
                if (ops->set_tone) ops->set_tone(ctx, LOG_TONE_WARNING);
                break;
            case 'U':
            {
                uint32_t codepoint = va_arg(args, uint32_t);
                char enc[4];
                int n = utf8_encode(codepoint, enc);
                for (int i = 0; i < n; ++i)
                {
                    if (!ops->emit_char(ctx, enc[i])) return count;
                    ++count;
                }
                break;
            }
            default:
                break;
            }
            while (*fmt && *fmt != ']') ++fmt;
            if (*fmt == ']') ++fmt;
            continue;
        }

        int left_align = 0, pad_zero = 0;
        if (*fmt == '-')      { left_align = 1; ++fmt; }
        else if (*fmt == '0') { pad_zero = 1;   ++fmt; }

        int pad_width = 0;
        while (*fmt >= '0' && *fmt <= '9')
        {
            pad_width = pad_width * 10 + (*fmt - '0');
            ++fmt;
        }

        /* Precision. It existed in ten format strings across the ACPI driver
         * before it existed here, and every one of them printed the literal
         * ".4s" and then handed the NEXT argument to the wrong conversion —
         * so "[ACPI] Table %.4s checksum failed (len=%u)" reported a length
         * that was really a pointer. Diagnostics that lie are worse than
         * diagnostics that are missing, and these are the diagnostics of the
         * subsystem you reach for when a machine you have never seen refuses
         * to boot.
         *
         * For %s it is what makes a four-character ACPI signature printable
         * at all: those are four bare bytes with no NUL after them, so the
         * length must come from the format and not from the data. For the
         * numeric conversions the value is parsed and ignored, which is still
         * strictly better than leaving the '.' to fall through into the
         * conversion switch as if it were one. */
        int precision = -1;                     /* -1 = unspecified */
        if (*fmt == '.')
        {
            ++fmt;
            precision = 0;
            if (*fmt == '*')
            {
                precision = va_arg(args, int);
                if (precision < 0) precision = -1;
                ++fmt;
            }
            else
            {
                while (*fmt >= '0' && *fmt <= '9')
                {
                    precision = precision * 10 + (*fmt - '0');
                    ++fmt;
                }
            }
        }

        int longflag = 0, longlongflag = 0, sizeflag = 0;
        if (*fmt == 'z')
        {
            sizeflag = 1;
            ++fmt;
        }
        else
        {
            while (*fmt == 'l')
            {
                ++fmt;
                if (longflag) { longlongflag = 1; longflag = 0; }
                else          { longflag = 1; }
            }
        }

        char num_buf[32];
        const char *str = NULL;
        size_t str_len = 0;
        int negative = 0;
        int hex_upper = 0;
        int emit_0x = 0;

        switch (*fmt)
        {
        case 'd':
        case 'i':
        {
            long long v;
            if      (sizeflag)     v = (long long)va_arg(args, size_t);
            else if (longlongflag) v = va_arg(args, long long);
            else if (longflag)     v = va_arg(args, long);
            else                   v = va_arg(args, int);
            if (v < 0) { negative = 1; v = -v; }
            utoa64((uint64_t)v, num_buf, 10);
            str = num_buf;
            str_len = strlen(num_buf);
            break;
        }
        case 'u':
        {
            unsigned long long v;
            if      (sizeflag)     v = (unsigned long long)va_arg(args, size_t);
            else if (longlongflag) v = va_arg(args, unsigned long long);
            else if (longflag)     v = va_arg(args, unsigned long);
            else                   v = va_arg(args, unsigned int);
            utoa64((uint64_t)v, num_buf, 10);
            str = num_buf;
            str_len = strlen(num_buf);
            break;
        }
        case 'X':
            hex_upper = 1;
            /* fallthrough */
        case 'x':
        {
            unsigned long long v;
            if      (sizeflag)     v = (unsigned long long)va_arg(args, size_t);
            else if (longlongflag) v = va_arg(args, unsigned long long);
            else if (longflag)     v = va_arg(args, unsigned long);
            else                   v = va_arg(args, unsigned int);
            utoa64((uint64_t)v, num_buf, 16);
            if (hex_upper)
                for (char *p = num_buf; *p; ++p) *p = (char)toupper(*p);
            str = num_buf;
            str_len = strlen(num_buf);
            break;
        }
        case 'p':
        {
            void *p = va_arg(args, void *);
            utoa64((uint64_t)(uintptr_t)p, num_buf, 16);
            str = num_buf;
            str_len = strlen(num_buf);
            emit_0x    = 1;
            pad_width  = 16;
            pad_zero   = 1;
            left_align = 0;
            break;
        }
        case 's':
        {
            str = va_arg(args, const char *);
            if (!str) str = "(null)";
            if (precision >= 0)
            {
                /* Bounded, and it must NOT walk off looking for a NUL: the
                 * argument is allowed to have none. Stop at the precision or
                 * at a terminator, whichever comes first — which is exactly
                 * what C requires of "%.Ns". */
                str_len = 0;
                while (str_len < (size_t)precision && str[str_len] != '\0')
                    ++str_len;
            }
            else
            {
                str_len = strlen(str);
            }
            break;
        }
        case 'c':
        {
            num_buf[0] = (char)va_arg(args, int);
            num_buf[1] = '\0';
            str = num_buf;
            str_len = 1;
            break;
        }
        case '%':
        {
            num_buf[0] = '%';
            num_buf[1] = '\0';
            str = num_buf;
            str_len = 1;
            break;
        }
        case 'f':
        {
            /* Kernel: -mno-sse, no FPU state in printf path. */
            str = "<no-float>";
            str_len = strlen(str);
            break;
        }
        default:
        {
            if (!ops->emit_char(ctx, '%')) return count;
            ++count;
            if (*fmt)
            {
                if (!ops->emit_char(ctx, *fmt)) return count;
                ++count;
                ++fmt;
            }
            continue;
        }
        }

        ++fmt;

        size_t prefix_len = (negative ? 1 : 0) + (emit_0x ? 2 : 0);
        size_t total_len  = prefix_len + str_len;
        int pad = ((int)total_len < pad_width) ? pad_width - (int)total_len : 0;

#define EMIT(c) do { if (!ops->emit_char(ctx, (c))) return count; ++count; } while (0)

        if (left_align)
        {
            if (negative) EMIT('-');
            if (emit_0x)  { EMIT('0'); EMIT('x'); }
            for (size_t i = 0; i < str_len; ++i) EMIT(str[i]);
            for (int i = 0; i < pad; ++i)        EMIT(' ');
        }
        else if (pad_zero)
        {
            if (negative) EMIT('-');
            if (emit_0x)  { EMIT('0'); EMIT('x'); }
            for (int i = 0; i < pad; ++i)        EMIT('0');
            for (size_t i = 0; i < str_len; ++i) EMIT(str[i]);
        }
        else
        {
            for (int i = 0; i < pad; ++i)        EMIT(' ');
            if (negative) EMIT('-');
            if (emit_0x)  { EMIT('0'); EMIT('x'); }
            for (size_t i = 0; i < str_len; ++i) EMIT(str[i]);
        }

#undef EMIT
    }

    return count;
}

/* --- Ring ops: kprintf's bytes into the log ring, each in its tone. --- */
static int RingEmit(void *ctx, char c)
{
    (void)ctx;
    LogRingPut(c, g_tone);
    return 1;
}
static void RingTone(void *ctx, uint8_t tone)
{
    (void)ctx;
    g_tone = tone;
}
static const kfmt_ops_t RING_OPS = {
    .emit_char = RingEmit,
    .set_tone  = RingTone,
};

/* --- Buffer ops: drives a bounded char buffer for ksnprintf/kvsnprintf. --- */
typedef struct { char *buf; size_t pos; size_t size; } kfmt_buf_ctx_t;

static int buf_emit(void *ctx, char c)
{
    kfmt_buf_ctx_t *b = (kfmt_buf_ctx_t *)ctx;
    if (b->pos + 1 < b->size)
    {
        b->buf[b->pos++] = c;
        return 1;
    }
    return 0;  /* buffer full — parser will stop */
}
static const kfmt_ops_t BUF_OPS = {
    .emit_char = buf_emit,
    .set_tone  = NULL,
};

/* Said into the ring under the console lock: tone PLAIN to begin with, the
 * utterance ended (LogRingSaid), and nothing drawn. */
static int kvprintf_said_locked(const char *format, va_list args)
{
    g_tone = LOG_TONE_PLAIN;
    int n = kvformat(&RING_OPS, NULL, format, args);
    LogRingSaid();
    return n;
}

int kvprintf(const char *format, va_list args)
{
    spin_lock(&g_kprintf_lock);
    /* Once a panic is claimed the console is its account's alone. A core that
     * comes to it after the claim stands here, before it puts a byte in the
     * ring: the NMI that stops it may still be on its way. The lock it stands
     * holding is taken by the owner as it stands (klib_lock.c). */
    PanicStandIfClaimedElsewhere();
    int n = kvprintf_said_locked(format, args);
    EmergencyScreenFollow();
    spin_unlock(&g_kprintf_lock);
    /* With the locks down: BatonPass can print, and a print under the
     * console lock would be this core spinning on its own lock. */
    LogRingBellKnock();
    return n;
}

/* The ring alone, for the line that ends a panic's account a fault or a panic
 * broke off (klib_panic.c): it goes out to the wire before the glass is
 * touched, so a fault on the glass's road costs the wire nothing. */
int KvprintfRing(const char *format, va_list args)
{
    spin_lock(&g_kprintf_lock);
    PanicStandIfClaimedElsewhere();
    int n = kvprintf_said_locked(format, args);
    spin_unlock(&g_kprintf_lock);
    return n;
}

int kprintf(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    int n = kvprintf(format, args);
    va_end(args);
    return n;
}

int kvsnprintf(char *buf, size_t size, const char *fmt, va_list args)
{
    if (!buf || size == 0) return 0;
    kfmt_buf_ctx_t ctx = { buf, 0, size };
    (void)kvformat(&BUF_OPS, &ctx, fmt, args);
    buf[ctx.pos] = '\0';
    return (int)ctx.pos;
}

int ksnprintf(char *buf, size_t size, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    int n = kvsnprintf(buf, size, fmt, args);
    va_end(args);
    return n;
}

__attribute__((noreturn)) void panic(const char *message, ...)
{
    /* This core stops taking interrupts first: nothing else is to run on it
     * while it says what happened, and an interrupt handler's kprintf would
     * land in the middle of the banner. */
    asm volatile("cli");

    /* The one claim both accounts take (klib_panic.c), before the banner is
     * composed: a core still running keeps writing into the same console and
     * the same ring while the account of the crash is being written into
     * them. A core that lost it to another stands there without a word; the
     * winner has every other online core stopped by an NMI and the glass
     * taken for its account, whoever held the screen (EmergencyScreenPanicTake);
     * and the core that holds it already, this panic inside its own account,
     * ends that account with this message as the line that says by what.
     * No lock is broken up front: every lock the banner takes on its way
     * — the console's, the ring's in LogRingPut, the line's in WireKick — is
     * taken by this core as it stands if another holds it (klib_lock.c), so a
     * core that died holding one, or one this core holds itself, costs the
     * banner nothing. */
    va_list args;
    va_start(args, message);
    bool claimed_here = false;
    PanicClaimForMessage(&claimed_here, message, args);

    /* The message straight onto the console, no copy of it on this stack: a
     * message whose own arguments are the next bug faults inside the
     * account's line, where the claim ends it with the line that says by
     * what (klib_panic.c), the words before the fault said. */
    kprintf("\n%[E]KERNEL PANIC:%[D] ");
    kvprintf(message, args);

    kprintf("\n\nDebug info:");
    kprintf("\n- Stack pointer: %p", __builtin_frame_address(0));
    kprintf("\n- Instruction pointer: %p\n", __builtin_return_address(0));

    va_end(args);

    /* Said whole: marked so, out to the memory the next boot reads and onto
     * the wire, and standing (klib_panic.c). */
    PanicEndAccount();
}

