/* characters.cpp — what a character is to a line of text. */

#include "characters.h"

namespace typecase {

namespace {

bool Within(char32_t Character, char32_t First, char32_t Last) noexcept
{
    return Character >= First && Character <= Last;
}

bool IsMark(char32_t C) noexcept
{
    return Within(C, 0x0300, 0x036F) || Within(C, 0x0483, 0x0489) || Within(C, 0x1AB0, 0x1AFF) ||
           Within(C, 0x1DC0, 0x1DFF) || Within(C, 0x20D0, 0x20FF) || Within(C, 0xFE00, 0xFE0F) ||
           Within(C, 0xFE20, 0xFE2F) || Within(C, 0x1F3FB, 0x1F3FF) || Within(C, 0xE0100, 0xE01EF);
}

bool IsSmallKana(char32_t C) noexcept
{
    switch (C) {
    case 0x3041: case 0x3043: case 0x3045: case 0x3047: case 0x3049: case 0x3063: case 0x3083: case 0x3085:
    case 0x3087: case 0x308E: case 0x3095: case 0x3096: case 0x30A1: case 0x30A3: case 0x30A5: case 0x30A7:
    case 0x30A9: case 0x30C3: case 0x30E3: case 0x30E5: case 0x30E7: case 0x30EE: case 0x30F5: case 0x30F6:
    case 0x30FC: case 0x3005: case 0x309D: case 0x309E: case 0x30FD: case 0x30FE: case 0x30FB:
        return true;
    default:
        return false;
    }
}

bool IsIdeograph(char32_t C) noexcept
{
    return Within(C, 0x1100, 0x115F) || Within(C, 0x2E80, 0x2FFF) || Within(C, 0x3040, 0x30FF) ||
           Within(C, 0x3100, 0x312F) || Within(C, 0x3130, 0x318F) || Within(C, 0x31A0, 0x31FF) ||
           Within(C, 0x3200, 0x33FF) || Within(C, 0x3400, 0x4DBF) || Within(C, 0x4E00, 0x9FFF) ||
           Within(C, 0xA000, 0xA4CF) || Within(C, 0xAC00, 0xD7AF) || Within(C, 0xF900, 0xFAFF) ||
           Within(C, 0xFF01, 0xFF60) || Within(C, 0x1F000, 0x1FAFF) || Within(C, 0x20000, 0x3FFFD);
}

}  // namespace

Kind KindOf(char32_t C) noexcept
{
    switch (C) {
    case U'\t': return Kind::Tab;
    case 0x000A: case 0x000B: case 0x000C: case 0x000D: case 0x0085: case 0x2028: case 0x2029:
        return Kind::LineBreak;
    case 0x0020: case 0x1680: case 0x2000: case 0x2001: case 0x2002: case 0x2003: case 0x2004: case 0x2005:
    case 0x2006: case 0x2008: case 0x2009: case 0x200A: case 0x205F: case 0x3000:
        return Kind::Space;
    case 0x00A0: case 0x2007: case 0x202F: case 0x2060: case 0xFEFF:
        return Kind::Glue;
    case 0x200B: return Kind::ZeroSpace;
    case 0x00AD: return Kind::SoftHyphen;
    case U'-': return Kind::Hyphen;
    case 0x2010: case 0x2012: case 0x2013: case 0x2014: return Kind::Dash;
    case U'(': case U'[': case U'{': case 0x3008: case 0x300A: case 0x300C: case 0x300E: case 0x3010:
    case 0x3014: case 0x3016: case 0x3018: case 0x301A: case 0xFF08: case 0xFF3B: case 0xFF5B:
        return Kind::Opening;
    case U')': case U']': case U'}': case U',': case U'.': case U':': case U';': case U'!': case U'?':
    case 0x3001: case 0x3002: case 0x3009: case 0x300B: case 0x300D: case 0x300F: case 0x3011: case 0x3015:
    case 0x3017: case 0x3019: case 0x301B: case 0xFF01: case 0xFF09: case 0xFF0C: case 0xFF0E: case 0xFF1A:
    case 0xFF1B: case 0xFF1F: case 0xFF3D: case 0xFF5D:
        return Kind::Closing;
    default:
        break;
    }
    if (C < 0x20 || Within(C, 0x7F, 0x9F) || C == 0x034F || C == 0x061C || Within(C, 0x200C, 0x200F) ||
        Within(C, 0x202A, 0x202E) || Within(C, 0x2061, 0x206F) || Within(C, 0xFFF9, 0xFFFB) ||
        Within(C, 0xE0000, 0xE007F) || Within(C, 0x180B, 0x180F))
        return Kind::Invisible;
    if (IsMark(C)) return Kind::Mark;
    if (IsSmallKana(C)) return Kind::Closing;
    if (IsIdeograph(C)) return Kind::Ideograph;
    return Kind::Letter;
}

bool Spreads(char32_t C) noexcept { return C == 0x0020 || C == 0x00A0 || C == 0x3000; }

bool Blank(char32_t C) noexcept
{
    switch (KindOf(C)) {
    case Kind::Space: case Kind::Tab: case Kind::LineBreak: case Kind::Invisible: case Kind::ZeroSpace:
    case Kind::SoftHyphen: case Kind::Glue:
        return true;
    default:
        return false;
    }
}

bool Clings(char32_t C) noexcept { return KindOf(C) == Kind::Mark; }

bool BreaksBetween(char32_t Previous, char32_t Before, char32_t After) noexcept
{
    const Kind A = KindOf(Before), B = KindOf(After);
    if (B == Kind::Mark || Before == Joiner || B == Kind::Invisible) return false;
    if (A == Kind::Glue || B == Kind::Glue) return false;
    if (A == Kind::Space || A == Kind::Tab) return B != Kind::Space && B != Kind::Tab;
    if (B == Kind::Space || B == Kind::Tab) return false;
    if (A == Kind::ZeroSpace || A == Kind::SoftHyphen) return true;
    if (B == Kind::Closing || A == Kind::Opening) return false;
    if (A == Kind::Hyphen) {
        const Kind P = Previous ? KindOf(Previous) : Kind::Space;
        const bool Digit = After >= U'0' && After <= U'9';
        return (P == Kind::Letter || P == Kind::Ideograph) && !Digit;
    }
    if (A == Kind::Dash) return true;
    if (After == 0x2014) return true;
    return A == Kind::Ideograph || B == Kind::Ideograph;
}

}  // namespace typecase
