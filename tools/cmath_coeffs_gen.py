#!/usr/bin/env python3
# ============================================================================
# cmath_coeffs_gen.py — offline generator for the 80-bit long double special
# functions in boxcxx <cmath> (Ф27d2: erf/erfc/tgamma/lgamma).
#
# NOT a build dependency. The kernel build compiles the baked constexpr tables
# this script prints; run it only to (re)generate those tables.
#
#   python3 -m venv venv && ./venv/bin/pip install mpmath
#   ./venv/bin/python tools/cmath_coeffs_gen.py
#
# Every long double literal is emitted at 25 significant digits (80-bit needs
# >=21 to round-trip; 25 over-specifies so the compiler rounds to the unique
# intended value). For each rounded-transcendental literal the script also emits
# a compile-time bit-pattern canary (masked to the low 80 bits) computed from the
# SAME round-to-nearest-even it used for the literal — so literal and canary can
# never silently disagree. The script round-trip-verifies every literal.
# ============================================================================
import sys
from fractions import Fraction

try:
    import mpmath
    from mpmath import mp, mpf
except ImportError:
    sys.exit("mpmath missing: python3 -m venv venv && ./venv/bin/pip install mpmath")

mp.dps = 120  # ~400 bits working precision — far above 80-bit target

LD_MANT_BITS = 64       # explicit-integer-bit mantissa width
LD_BIAS      = 16383
SIG_DIGITS   = 25

# ── round-to-nearest-even encode of a real value to the 80-bit field layout ──
def ld_decompose(v):
    v = mpf(v)
    if v == 0:
        return (1 if mpmath.sign(v) < 0 else 0, 0, 0)
    sign = 1 if v < 0 else 0
    a = abs(v)
    e = int(mpmath.floor(mpmath.log(a, 2)))
    scale = a * mpf(2) ** (LD_MANT_BITS - 1 - e)      # aim for [2^63, 2^64)
    while scale >= mpf(2) ** LD_MANT_BITS:
        e += 1; scale = a * mpf(2) ** (LD_MANT_BITS - 1 - e)
    while scale < mpf(2) ** (LD_MANT_BITS - 1):
        e -= 1; scale = a * mpf(2) ** (LD_MANT_BITS - 1 - e)
    fl = int(mpmath.floor(scale))
    frac = scale - fl
    if   frac < mpf(1) / 2: mant = fl
    elif frac > mpf(1) / 2: mant = fl + 1
    else:                   mant = fl if fl % 2 == 0 else fl + 1   # ties → even
    if mant >= 2 ** LD_MANT_BITS:                      # rounding carried out
        mant //= 2; e += 1
    biased = e + LD_BIAS
    assert 0 < biased < 0x7FFF, f"exponent {e} out of 80-bit range for {v}"
    assert 2 ** (LD_MANT_BITS - 1) <= mant < 2 ** LD_MANT_BITS
    return (sign, biased, mant)

def ld_value(v):                                       # exact value the 80-bit field holds
    sign, biased, mant = ld_decompose(v)
    if mant == 0:
        return mpf(0)
    val = mpf(mant) * mpf(2) ** (biased - LD_BIAS - (LD_MANT_BITS - 1))
    return -val if sign else val

def ld_literal(v):
    for digits in (SIG_DIGITS, 27, 30, 34):
        s = mpmath.nstr(mpf(v), digits, strip_zeros=False)
        if 'e' not in s and 'E' not in s and '.' not in s:
            s += '.0'
        if ld_decompose(mpf(s)) == ld_decompose(v):    # literal rounds to intended value
            return s + 'L'
    raise RuntimeError(f"cannot round-trip {v}")

def canary(name, v):
    sign, biased, mant = ld_decompose(v)
    hi16 = (sign << 15) | biased
    return (f"static_assert((__builtin_bit_cast(unsigned __int128, {name}) "
            f"& ((((unsigned __int128)1) << 80) - 1)) == "
            f"((((unsigned __int128)0x{hi16:04X}u) << 64) | (unsigned __int128)0x{mant:016X}ull), "
            f'"{name} lost 80-bit precision");')

def fmt_table(name, values, per_row=3):
    out = [f"inline constexpr long double {name}[] = {{"]
    row = []
    for v in values:
        row.append(ld_literal(v))
        if len(row) == per_row:
            out.append("  " + ", ".join(row) + ",")
            row = []
    if row:
        out.append("  " + ", ".join(row) + ",")
    out.append("};")
    out.append(f"inline constexpr int {name}_N = {len(values)};")
    return "\n".join(out)

# ── Chebyshev fit of f on [a,b]: c[0]=a0/2, c[k]=a_k  (matches the Clenshaw loop) ──
def cheb_coeffs(f, a, b, M):
    xs, fs = [], []
    for j in range(M):
        t = mpmath.cos(mpmath.pi * (2 * j + 1) / (2 * M))
        x = ((b - a) * t + (a + b)) / 2
        xs.append(t); fs.append(f(x))
    a_k = []
    for k in range(M):
        s = mpmath.mpf(0)
        for j in range(M):
            s += fs[j] * mpmath.cos(mpmath.pi * k * (2 * j + 1) / (2 * M))
        a_k.append(2 * s / M)
    c = list(a_k); c[0] = a_k[0] / 2
    return c

def clenshaw(c, t):
    b0 = b1 = mpmath.mpf(0)
    for k in range(len(c) - 1, 0, -1):
        b0, b1 = 2 * t * b0 - b1 + c[k], b0
    return t * b0 - b1 + c[0]

def fit_scaled_erfc(a, b, tol):
    f = lambda x: x * mpmath.e ** (x * x) * mpmath.erfc(x)   # w(x) = x e^{x^2} erfc(x)
    M = 256
    c_full = cheb_coeffs(f, a, b, M)
    N = len(c_full)
    for n in range(4, M):
        if sum(abs(c_full[k]) for k in range(n, M)) < tol:
            N = n; break
    c = c_full[:N]
    worst = mpmath.mpf(0)
    for i in range(4001):
        x = a + (b - a) * i / 4000
        t = (2 * x - (a + b)) / (b - a)
        err = abs(clenshaw(c, t) - f(x))
        if err > worst: worst = err
    return c, N, worst

def bits_below(err):
    return float(-mpmath.log(err, 2)) if err > 0 else 999.0

# ============================================================================
print("// ==== paste into src/userspace/boxcxx/include/std/__bits/cmath_coeffs ====")
print("// Generated by tools/cmath_coeffs_gen.py (mpmath). Do not hand-edit values.\n")

tol_fit = mpf(2) ** -66

# ── ERF_C_L: erf(x)/x Maclaurin, c_n = (2/sqrtpi)(-1)^n / (n!(2n+1)) ──
erf_c = []
c0 = 2 / mpmath.sqrt(mpmath.pi)
n = 0
while True:
    cn = c0 * ((-1) ** n) / (mpmath.factorial(n) * (2 * n + 1))
    erf_c.append(cn)
    # truncation on |x|<0.5 → s=x^2<0.25: first omitted term magnitude
    nxt = abs(c0 / (mpmath.factorial(n + 1) * (2 * n + 3))) * mpf("0.25") ** (n + 1)
    if nxt < mpf(2) ** -67 and n + 1 >= 15:
        break
    n += 1
erf_omit = abs(c0 / (mpmath.factorial(len(erf_c)) * (2 * len(erf_c) + 1))) * mpf("0.25") ** len(erf_c)
print(fmt_table("ERF_C_L", erf_c))
print(f"// ERF_C_L: {len(erf_c)} terms; first omitted at s=0.25 ~ 2^-{bits_below(erf_omit):.1f}")
print()

# ── ERFC_W1_L on [0.5, 6], ERFC_W2_L on [6, 12] ──
w1, n1, e1 = fit_scaled_erfc(mpf("0.5"), mpf(6), tol_fit)
w2, n2, e2 = fit_scaled_erfc(mpf(6), mpf(12), tol_fit)
print(fmt_table("ERFC_W1_L", w1))
print(f"// ERFC_W1_L on [0.5,6]: {n1} terms, achieved max|err| ~ 2^-{bits_below(e1):.1f}")
print(fmt_table("ERFC_W2_L", w2))
print(f"// ERFC_W2_L on [6,12]: {n2} terms, achieved max|err| ~ 2^-{bits_below(e2):.1f}")
print()

# ── STIRL_L: B_{2k}/((2k)(2k-1)), k=1..11, exact rationals ──
stirl_lines = ["inline constexpr long double STIRL_L[] = {"]
for k in range(1, 12):
    bn, bd = mpmath.bernfrac(2 * k)
    coeff = Fraction(int(bn), int(bd)) / ((2 * k) * (2 * k - 1))
    num, den = coeff.numerator, coeff.denominator
    assert abs(num) < 2 ** 20 and den < 2 ** 20, f"STIRL_L[{k}] not < 2^20: {num}/{den}"
    stirl_lines.append(f"  {num}.0L/{den}.0L,")
stirl_lines.append("};")
stirl_lines.append("inline constexpr int STIRL_L_N = 11;")
print("\n".join(stirl_lines))
print("// STIRL_L: exact rationals (num,den < 2^20 → compiler division is exact 80-bit)")
print()

# ── constants ──
pi          = mpmath.pi
inv_sqrtpi  = 1 / mpmath.sqrt(mpmath.pi)
halflog2pi  = mpmath.log(2 * mpmath.pi) / 2
hlp_hi      = ld_value(halflog2pi)
hlp_lo      = halflog2pi - hlp_hi
ln2         = mpmath.log(2)
# KLN2HI: mask low 40 mantissa bits so k*KLN2HI is exact for |k| <= 2^14 (exp path).
_, _, ln2_mant = ld_decompose(ln2)
ln2_hi_mant = ln2_mant & ~((1 << 40) - 1)
kln2hi      = mpf(ln2_hi_mant) * mpf(2) ** (-1 - 63)   # ln2 = mant·2^(exp−63), e(ln2)=−1
kln2lo      = ln2 - kln2hi
# KLN2DD: proper NORMALIZED ln2 double-double for log_l_dd (small k, no exactness
# constraint; a 24-bit hi like KLN2HI would leave a ~2^-22 un-normalized low part).
kln2dd_hi   = ld_value(ln2)                            # full 64-bit-mantissa ln2
kln2dd_lo   = ln2 - kln2dd_hi                          # residual ~ 2^-65
third_hi    = ld_value(mpf(1) / 3)
third_lo    = mpf(1) / 3 - third_hi

print(f"inline constexpr long double kPiL = {ld_literal(pi)};")
print(f"inline constexpr long double INV_SQRTPI_L = {ld_literal(inv_sqrtpi)};")
print(f"inline constexpr long double KHALFLOG2PI_HI_L = {ld_literal(hlp_hi)};")
print(f"inline constexpr long double KHALFLOG2PI_LO_L = {ld_literal(hlp_lo)};")
print(f"inline constexpr long double KLN2HI_L = 0x{ln2_hi_mant:016X}p-64L;   // 24 sig bits, 40 trailing zeros")
print(f"inline constexpr long double KLN2LO_L = {ld_literal(kln2lo)};")
print(f"inline constexpr long double KLN2DD_HI_L = {ld_literal(kln2dd_hi)};   // full-precision ln2 (log_l_dd)")
print(f"inline constexpr long double KLN2DD_LO_L = {ld_literal(kln2dd_lo)};")
print(f"inline constexpr long double C3_HI_L = {ld_literal(third_hi)};")
print(f"inline constexpr long double C3_LO_L = {ld_literal(third_lo)};")
print()

# ── compile-time precision canaries (R1 layer 2) ──
print("// R1 layer 2: bit-pattern canaries — the only build-time catch for an")
print("// under-digited literal (a plain x!=(double)x does NOT catch 56-bit loss).")
print(canary("ERF_C_L[0]", erf_c[0]))
print(canary("ERFC_W1_L[0]", w1[0]))
print(canary("ERFC_W2_L[0]", w2[0]))
print(canary("kPiL", pi))
print(canary("INV_SQRTPI_L", inv_sqrtpi))
print(canary("KHALFLOG2PI_HI_L", hlp_hi))
print(canary("KHALFLOG2PI_LO_L", hlp_lo))
print(canary("KLN2LO_L", kln2lo))
print(canary("KLN2DD_HI_L", kln2dd_hi))
print(canary("KLN2DD_LO_L", kln2dd_lo))
print(canary("C3_HI_L", third_hi))
print(canary("C3_LO_L", third_lo))

# ============================================================================
# Phase65 oracle grid — correctly-rounded 80-bit reference values, computed at
# the EXACT 80-bit value each x-literal holds (so the sweep measures only the
# function's error, not x-rounding). Paste into cxxtest.cpp Phase65.
# ============================================================================
def oracle_block(name, grid, f):
    lines = [f"    static const OraclePt {name}[] = {{"]
    for xd in grid:
        x = mpf(xd)
        xexact = ld_value(x)
        o = f(xexact)
        lines.append(f"        {{ {ld_literal(x)}, {ld_literal(o)} }},")
    lines.append("    };")
    return "\n".join(lines)

erf_grid   = ["0.1","0.25","0.4","0.499","0.5","0.75","1.0","1.5","2.0","3.0","4.0","5.0","6.0",
              "-0.5","-2.0","-4.0"]
erfc_grid  = ["0.5","0.75","1.0","1.5","2.0","3.0","4.0","5.0","6.0","7.0","8.0","10.0","12.0",
              "15.0","20.0","30.0","40.0","50.0","-0.3","-1.0","-3.0"]
tgam_grid  = ["0.6","0.75","0.9","1.0","1.1","1.5","2.5","3.0","4.0","4.4","5.5","7.0","7.3","10.0","10.9",
              "11.0","15.0","20.0","25.0","30.0","0.51",
              "-0.5","-1.5","-2.5","-3.5"]
lgam_grid  = ["0.51","0.6","0.75","1.5","2.5","3.0","4.0","4.4","5.0","7.3","8.0","10.9","11.0","15.0","20.0","50.0","100.0",
              "-0.5","-1.5","-2.5","-3.5"]

f_erf   = lambda x: mpmath.erf(x)
f_erfc  = lambda x: mpmath.erfc(x)
f_tgam  = lambda x: mpmath.gamma(x)
f_lgam  = lambda x: mpmath.log(abs(mpmath.gamma(x)))

print("\n// ==== paste into src/userspace/apps/cxxtest.cpp Phase65 ====")
print(oracle_block("kErf", erf_grid, f_erf))
print(oracle_block("kErfc", erfc_grid, f_erfc))
print(oracle_block("kTgam", tgam_grid, f_tgam))
print(oracle_block("kLgam", lgam_grid, f_lgam))

# named single-point oracles used by the decisive checks
print("\n// named point oracles")
for nm, v in [
    ("kErf05",  mpmath.erf(ld_value(mpf("0.5")))),
    ("kErf3",   mpmath.erf(ld_value(mpf(3)))),
    ("kErfc1",  mpmath.erfc(ld_value(mpf(1)))),
    ("kErfc8",  mpmath.erfc(ld_value(mpf(8)))),
    ("kErfc20", mpmath.erfc(ld_value(mpf(20)))),
    ("kErfc40", mpmath.erfc(ld_value(mpf(40)))),
    ("kSqrtPi", mpmath.sqrt(mpmath.pi)),          # tgamma(0.5)
    ("kTg55",   mpmath.gamma(ld_value(mpf("5.5")))),
    ("kHalfLnPi", mpmath.log(mpmath.pi) / 2),      # lgamma(0.5)
    ("kLg50",   mpmath.log(abs(mpmath.gamma(ld_value(mpf(50)))))),
    ("kTgNeg05", mpmath.gamma(ld_value(mpf("-0.5")))),   # -2 sqrt(pi)
    ("kLgNeg05", mpmath.log(abs(mpmath.gamma(ld_value(mpf("-0.5")))))),
]:
    print(f"    const long double {nm} = {ld_literal(v)};")

sys.stderr.write("OK: generated. Review the achieved-error comments above.\n")
