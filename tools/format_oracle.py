#!/usr/bin/env python3
"""format oracle — what C's %f, %e and %g write for an x87 long double.

    format_oracle.py extended COUNT    print COUNT vectors for format_stand

Each line is "conversion precision alternate mantissa exponent negative text":
the value is mantissa x 2^exponent, taken exactly as a fraction, and the text
is written from that fraction by C's rules — rounded to nearest, ties to
even, %g choosing its form by %e's exponent and dropping trailing zeros
unless '#'. The host has no 80-bit type for printf to be held to, so the
witness is exact arithmetic instead.
"""

import random
import sys
from fractions import Fraction

sys.set_int_max_str_digits(0)       # a long double's integer part runs to 4933 digits


def nearest(value):
    """The integer nearest a nonnegative fraction, a tie going to even."""
    whole, rest = divmod(value.numerator, value.denominator)
    if 2 * rest > value.denominator or (2 * rest == value.denominator and whole % 2 == 1):
        whole += 1
    return whole


def fixed(value, precision, alternate):
    digits = str(nearest(value * 10 ** precision)).rjust(precision + 1, "0")
    whole = digits[:len(digits) - precision]
    fraction = digits[len(digits) - precision:]
    return whole + ("." if precision or alternate else "") + fraction


def decade(value):
    """floor(log10(value)) for a value above zero."""
    guess = len(str(value.numerator)) - len(str(value.denominator))
    while Fraction(10) ** guess > value:
        guess -= 1
    while Fraction(10) ** (guess + 1) <= value:
        guess += 1
    return guess


def scientific_parts(value, precision):
    if value == 0:
        return 0, 0
    exponent = decade(value)
    digits = nearest(value * Fraction(10) ** (precision - exponent))
    if digits == 10 ** (precision + 1):
        exponent += 1
        digits //= 10
    return exponent, digits


def scientific(value, precision, alternate):
    exponent, digits = scientific_parts(value, precision)
    text = str(digits).rjust(precision + 1, "0")
    mantissa = text[0] + ("." if precision or alternate else "") + text[1:]
    return "%se%s%02d" % (mantissa, "-" if exponent < 0 else "+", abs(exponent))


def general(value, precision, alternate):
    precision = max(precision, 1)
    exponent, _ = scientific_parts(value, precision - 1)
    if precision > exponent >= -4:
        text = fixed(value, precision - 1 - exponent, alternate)
    else:
        text = scientific(value, precision - 1, alternate)
    if alternate:
        return text
    mantissa, mark, tail = text.partition("e")
    if "." in mantissa:
        mantissa = mantissa.rstrip("0").rstrip(".")
    return mantissa + mark + tail


FORMS = {"f": fixed, "e": scientific, "g": general}


def vectors(count, seed):
    rng = random.Random(seed)
    edges = [((1 << 64) - 1, 16320), (1 << 63, -16382 - 63), (1, -16445), ((1 << 63) | 1, 0),
             (0xA000000000000000, -60), (0xFFFFFFFFFFFFFFFF, -64), (0xC000000000000000, -63),
             (0x8000000000000000, -63), (0, 0)]
    for index in range(count):
        if index < len(edges):
            mantissa, exponent = edges[index]
        elif rng.random() < 0.1:                       # a denormal
            mantissa, exponent = rng.getrandbits(rng.randint(1, 63)), -16445
        else:
            mantissa = rng.getrandbits(64) | (1 << 63)
            exponent = rng.choice([rng.randint(-16445, 16320), rng.randint(-200, 200),
                                   rng.randint(-80, 20)])
        conversion = rng.choice("feg")
        precision = rng.choice([rng.randint(0, 25), rng.randint(0, 25), rng.randint(26, 120)])
        if conversion == "f" and abs(exponent) > 400:
            precision = rng.randint(0, 6)
        if index % 97 == 0:
            precision = rng.randint(1000, 17000) if conversion != "f" or exponent > 0 else 16500
        alternate = int(rng.random() < 0.2)
        negative = int(rng.random() < 0.5)
        value = Fraction(mantissa) * Fraction(2) ** exponent
        text = FORMS[conversion](value, precision, alternate)
        yield "%s %d %d %x %d %d %s%s" % (conversion, precision, alternate, mantissa, exponent,
                                          negative, "-" if negative else "", text)


def main(argv):
    if len(argv) == 3 and argv[1] == "extended":
        for line in vectors(int(argv[2]), 80):
            print(line)
        return 0
    print(__doc__.strip().splitlines()[2], file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
