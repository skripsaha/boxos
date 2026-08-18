/*
 * cfenv.cpp — [cfenv] against the actual hardware.
 *
 * Two units, always both. SSE (MXCSR) carries float and double; x87 carries
 * boxcxx's genuine 80-bit long double (Ф27). A <cfenv> that read only MXCSR
 * would report "no exception" for every long double operation that raised one,
 * which is the kind of half-answer this library does not ship.
 *
 * The instruction set here is deliberately the old, unprivileged one —
 * stmxcsr/ldmxcsr, fnstcw/fldcw, fnstenv/fldenv — because it exists on every
 * x86-64 part, including the oldest machine BoxOS is meant to boot on. There is
 * no xsave path and no CPUID check: nothing below is optional in the
 * architecture.
 */

#include <cfenv>

namespace {

unsigned int ReadMxcsr()
{
    unsigned int v;
    __asm__ volatile("stmxcsr %0" : "=m"(v));
    return v;
}

void WriteMxcsr(unsigned int v)
{
    __asm__ volatile("ldmxcsr %0" : : "m"(v));
}

unsigned short ReadX87ControlWord()
{
    unsigned short cw;
    __asm__ volatile("fnstcw %0" : "=m"(cw));
    return cw;
}

void WriteX87ControlWord(unsigned short cw)
{
    __asm__ volatile("fldcw %0" : : "m"(cw));
}

unsigned short ReadX87StatusWord()
{
    unsigned short sw;
    __asm__ volatile("fnstsw %0" : "=am"(sw));
    return sw;
}

/* The x87 status word has no partial-clear instruction: FNCLEX clears every
 * flag at once. Clearing a SUBSET means writing the word back, and the only
 * way to write it is through the 28-byte environment image — read it, edit the
 * status field, load it again. FNSTENV masks all exceptions as a side effect,
 * which FLDENV then undoes by restoring the control word we just read. */
void ClearX87Flags(unsigned int excepts)
{
    unsigned char env[28];
    __asm__ volatile("fnstenv %0" : "=m"(env));
    unsigned short sw;
    __builtin_memcpy(&sw, env + 4, sizeof sw);
    sw = static_cast<unsigned short>(sw & ~(excepts & FE_ALL_EXCEPT));
    __builtin_memcpy(env + 4, &sw, sizeof sw);
    __asm__ volatile("fldenv %0" : : "m"(env));
}

void SetX87Flags(unsigned int excepts)
{
    unsigned char env[28];
    __asm__ volatile("fnstenv %0" : "=m"(env));
    unsigned short sw;
    __builtin_memcpy(&sw, env + 4, sizeof sw);
    sw = static_cast<unsigned short>(sw | (excepts & FE_ALL_EXCEPT));
    __builtin_memcpy(env + 4, &sw, sizeof sw);
    __asm__ volatile("fldenv %0" : : "m"(env));
}

constexpr unsigned int kRoundMask = 0x0C00u;   // both units, same two bits
constexpr int          kMxcsrRoundShift = 3;   // x87 bits 10-11 → MXCSR 13-14

} // namespace

// The default environment, as the hardware defines it: everything masked,
// round to nearest, x87 precision 64 bits — which is the setting boxcxx's
// 80-bit long double depends on, so it is not merely inherited, it is required.
extern "C" const std::fenv_t __boxcxx_fe_dfl_env = {0x037F, 0x0000, 0x1F80};

namespace std {

int feclearexcept(int excepts) noexcept
{
    const unsigned int mask = static_cast<unsigned int>(excepts) & FE_ALL_EXCEPT;
    WriteMxcsr(ReadMxcsr() & ~mask);
    ClearX87Flags(mask);
    return 0;
}

int fetestexcept(int excepts) noexcept
{
    const unsigned int mask = static_cast<unsigned int>(excepts) & FE_ALL_EXCEPT;
    // The union of both units: an exception raised by a long double operation
    // is as real as one raised by a double.
    const unsigned int raised =
        (ReadMxcsr() | ReadX87StatusWord()) & FE_ALL_EXCEPT;
    return static_cast<int>(raised & mask);
}

int feraiseexcept(int excepts) noexcept
{
    const unsigned int mask = static_cast<unsigned int>(excepts) & FE_ALL_EXCEPT;

    // Performed, not asserted: an executed division IS the exception, and it
    // would trap if a trap were ever unmasked. The volatile is insurance
    // against a build that adds -ffast-math; GCC's default -ftrapping-math
    // already refuses to fold these away (measured at -O0 and -O2).
    if (mask & FE_INVALID) {
        volatile double zero = 0.0;
        volatile double r    = zero / zero;
        (void)r;
    }
    if (mask & FE_DIVBYZERO) {
        volatile double one  = 1.0;
        volatile double zero = 0.0;
        volatile double r    = one / zero;
        (void)r;
    }
    if (mask & FE_OVERFLOW) {
        volatile double big = 1.7976931348623157e308;
        volatile double r   = big * big;
        (void)r;
    }
    if (mask & FE_UNDERFLOW) {
        volatile double tiny = 2.2250738585072014e-308;
        volatile double r    = tiny * tiny;
        (void)r;
    }
    if (mask & FE_INEXACT) {
        volatile double one   = 1.0;
        volatile double three = 3.0;
        volatile double r     = one / three;
        (void)r;
    }
    // FE_DENORMAL is x86's own and no arithmetic raises it on demand without
    // a denormal operand, so this one IS written rather than performed.
    if (mask & FE_DENORMAL) {
        WriteMxcsr(ReadMxcsr() | FE_DENORMAL);
    }
    return 0;
}

int fegetexceptflag(fexcept_t *flagp, int excepts) noexcept
{
    if (!flagp) return 1;
    *flagp = static_cast<fexcept_t>(fetestexcept(excepts));
    return 0;
}

int fesetexceptflag(const fexcept_t *flagp, int excepts) noexcept
{
    if (!flagp) return 1;
    const unsigned int mask = static_cast<unsigned int>(excepts) & FE_ALL_EXCEPT;
    const unsigned int want = *flagp & mask;

    // [cfenv.syn]: this SETS the flag state without raising — no trap, even if
    // a future BoxOS unmasks one. That is the difference from feraiseexcept,
    // and the reason both exist.
    WriteMxcsr((ReadMxcsr() & ~mask) | want);
    ClearX87Flags(mask);
    if (want) SetX87Flags(want);
    return 0;
}

int fegetround() noexcept
{
    // Both units are kept in step by fesetround, so either answers — MXCSR is
    // read because that is where float and double actually round.
    return static_cast<int>((ReadMxcsr() >> kMxcsrRoundShift) & kRoundMask);
}

int fesetround(int round) noexcept
{
    const unsigned int r = static_cast<unsigned int>(round);
    if (r != FE_TONEAREST && r != FE_DOWNWARD && r != FE_UPWARD &&
        r != FE_TOWARDZERO)
        return 1;                        // nonzero: the mode was not installed

    // BOTH, or long double would round differently from double — a divergence
    // no program could explain.
    WriteX87ControlWord(
        static_cast<unsigned short>((ReadX87ControlWord() & ~kRoundMask) | r));
    WriteMxcsr((ReadMxcsr() & ~(kRoundMask << kMxcsrRoundShift)) |
               (r << kMxcsrRoundShift));
    return 0;
}

int fegetenv(fenv_t *envp) noexcept
{
    if (!envp) return 1;
    envp->X87ControlWord = ReadX87ControlWord();
    envp->X87StatusWord  = ReadX87StatusWord();
    envp->Mxcsr          = ReadMxcsr();
    return 0;
}

int fesetenv(const fenv_t *envp) noexcept
{
    if (!envp) return 1;
    WriteX87ControlWord(envp->X87ControlWord);
    ClearX87Flags(FE_ALL_EXCEPT);
    SetX87Flags(envp->X87StatusWord & FE_ALL_EXCEPT);
    WriteMxcsr(envp->Mxcsr);   // one word: SSE masks, rounding AND flags
    return 0;
}

int feholdexcept(fenv_t *envp) noexcept
{
    if (!envp) return 1;
    fegetenv(envp);
    feclearexcept(FE_ALL_EXCEPT);
    // "Install a non-stop mode": every exception masked, so nothing traps
    // while the caller computes. Both units, again.
    WriteX87ControlWord(
        static_cast<unsigned short>(ReadX87ControlWord() | FE_ALL_EXCEPT));
    WriteMxcsr(ReadMxcsr() | (FE_ALL_EXCEPT << 7));   // MXCSR masks: bits 7-12
    return 0;
}

int feupdateenv(const fenv_t *envp) noexcept
{
    if (!envp) return 1;
    // The flags raised while the caller held the environment must SURVIVE the
    // restore — that is the whole point of the function, and the reason it is
    // not just fesetenv.
    const int raised = fetestexcept(FE_ALL_EXCEPT);
    fesetenv(envp);
    feraiseexcept(raised);
    return 0;
}

} // namespace std
