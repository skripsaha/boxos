
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

constexpr unsigned int kRoundMask = 0x0C00u;
constexpr int          kMxcsrRoundShift = 3;

}

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
    const unsigned int raised =
        (ReadMxcsr() | ReadX87StatusWord()) & FE_ALL_EXCEPT;
    return static_cast<int>(raised & mask);
}

int feraiseexcept(int excepts) noexcept
{
    const unsigned int mask = static_cast<unsigned int>(excepts) & FE_ALL_EXCEPT;

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

    WriteMxcsr((ReadMxcsr() & ~mask) | want);
    ClearX87Flags(mask);
    if (want) SetX87Flags(want);
    return 0;
}

int fegetround() noexcept
{
    return static_cast<int>((ReadMxcsr() >> kMxcsrRoundShift) & kRoundMask);
}

int fesetround(int round) noexcept
{
    const unsigned int r = static_cast<unsigned int>(round);
    if (r != FE_TONEAREST && r != FE_DOWNWARD && r != FE_UPWARD &&
        r != FE_TOWARDZERO)
        return 1;

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
    WriteMxcsr(envp->Mxcsr);
    return 0;
}

int feholdexcept(fenv_t *envp) noexcept
{
    if (!envp) return 1;
    fegetenv(envp);
    feclearexcept(FE_ALL_EXCEPT);
    WriteX87ControlWord(
        static_cast<unsigned short>(ReadX87ControlWord() | FE_ALL_EXCEPT));
    WriteMxcsr(ReadMxcsr() | (FE_ALL_EXCEPT << 7));
    return 0;
}

int feupdateenv(const fenv_t *envp) noexcept
{
    if (!envp) return 1;
    const int raised = fetestexcept(FE_ALL_EXCEPT);
    fesetenv(envp);
    feraiseexcept(raised);
    return 0;
}

}