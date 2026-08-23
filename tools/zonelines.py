"""Zone records from the tzdata source, for the one thing TZif does not carry.

`save` is not in a TZif file. Deriving it by looking at neighbouring intervals
is what CPython does and it is wrong wherever a zone moved its standard offset
and its DST amount in the same reform: Inuvik went straight from PST to MDT in
1979, so the adjacent-interval answer is two hours where the Rule says one, and
America/Coyhaique -- a zone tzdata only added in 2025 -- comes out with a DST
amount of 2565 seconds, which is not a thing.

The definition that is actually true is `save = offset - STDOFF of the Zone
record in force`, and the Zone records are right here in the source. Their
boundaries are UNTIL fields in local time, so placing them exactly would need
the DST amount we are trying to find; the way out is that a boundary is never
more than a day or so away from where the standard offset alone puts it, and
that EVERY standard-time transition must land on a record whose STDOFF equals
its offset. That constraint pins the assignment wherever the approximation
could have slipped, and the generator refuses to write when it cannot be met.
"""
import re

MONTHS = {m: i + 1 for i, m in enumerate(
    ["Jan", "Feb", "Mar", "Apr", "May", "Jun",
     "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"])}
DAYS = {d: i for i, d in enumerate(
    ["Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"])}


def days_from_civil(y, m, d):
    y -= m <= 2
    era = (y if y >= 0 else y - 399) // 400
    yoe = y - era * 400
    mp = m - 3 if m > 2 else m + 9
    doy = (153 * mp + 2) // 5 + d - 1
    doe = yoe * 365 + yoe // 4 - yoe // 100 + doy
    return era * 146097 + doe - 719468


def weekday_from_days(z):
    return (z + 4) % 7 if z >= -4 else (z + 5) % 7 + 6


def parse_hms(s):
    """[-]h[:mm[:ss]] -> seconds. A bare '-' is zero."""
    if s in ("-", ""):
        return 0
    neg = s.startswith("-")
    s = s.lstrip("+-")
    parts = s.split(":")
    v = int(parts[0]) * 3600
    if len(parts) > 1:
        v += int(parts[1]) * 60
    if len(parts) > 2:
        v += int(float(parts[2]))
    return -v if neg else v


def parse_until(fields):
    """(year, month, day_expr, time_seconds, suffix) from an UNTIL field list."""
    if not fields:
        return None
    y = int(fields[0])
    mo = MONTHS[fields[1]] if len(fields) > 1 else 1
    day = fields[2] if len(fields) > 2 else "1"
    tm, suf = 0, "w"
    if len(fields) > 3:
        t = fields[3]
        if t and t[-1] in "wsugz":
            suf, t = t[-1], t[:-1]
        tm = parse_hms(t)
    return (y, mo, day, tm, suf)


def resolve_day(y, mo, expr):
    """A tzdata day expression -> day number since the epoch."""
    if expr.isdigit():
        return days_from_civil(y, mo, int(expr))
    m = re.fullmatch(r"last(\w{3})", expr)
    if m:
        wd = DAYS[m.group(1)]
        nm_y, nm_m = (y, mo + 1) if mo < 12 else (y + 1, 1)
        last = days_from_civil(nm_y, nm_m, 1) - 1
        return last - ((weekday_from_days(last) - wd) % 7)
    m = re.fullmatch(r"(\w{3})([<>])=(\d+)", expr)
    if m:
        wd, op, num = DAYS[m.group(1)], m.group(2), int(m.group(3))
        base = days_from_civil(y, mo, num)
        if op == ">":
            return base + ((wd - weekday_from_days(base)) % 7)
        return base - ((weekday_from_days(base) - wd) % 7)
    raise SystemExit(f"unparsed day expression {expr!r}")


class Record:
    __slots__ = ("stdoff", "rules", "fmt", "until")

    def __init__(self, stdoff, rules, fmt, until):
        self.stdoff, self.rules, self.fmt, self.until = stdoff, rules, fmt, until


def load_zones(sources):
    """name -> [Record], in order."""
    zones, cur = {}, None
    for src in sources:
        for raw in open(src, errors="replace"):
            line = raw.split("#", 1)[0].rstrip()
            if not line.strip():
                # A comment does NOT end a Zone block, and Europe/Dublin is why
                # this is spelled out: tzdata splits it with "# Vanguard
                # section" in the middle, and treating that as a terminator
                # dropped its last four records — including the one where
                # Ireland's standard offset becomes 1:00 and its winter becomes
                # negative DST. A block ends at a record without an UNTIL, or
                # at the next Zone/Rule/Link, and at nothing else.
                continue
            f = line.split()
            if f[0] == "Zone":
                cur = zones.setdefault(f[1], [])
                cur.append(Record(parse_hms(f[2]), f[3], f[4], parse_until(f[5:])))
                if cur[-1].until is None:
                    cur = None
            elif f[0] in ("Rule", "Link"):
                cur = None
            elif cur is not None and raw[:1] in ("\t", " "):
                cur.append(Record(parse_hms(f[0]), f[1], f[2], parse_until(f[3:])))
                if cur[-1].until is None:
                    cur = None
    return zones


def bounds_of(records):
    """Approximate end instant of each record; the last one is open."""
    out = []
    for r in records:
        if r.until is None:
            out.append(None)
            continue
        y, mo, dayexpr, tm, suf = r.until
        local = resolve_day(y, mo, dayexpr) * 86400 + tm
        # `u` is already UTC; `w` and `s` are local, and the difference between
        # them is the DST amount we do not have. One standard offset is close
        # enough: the constraint below fixes whatever it misplaces.
        out.append(local if suf in "ugz" else local - r.stdoff)
    return out


def snap(bounds, times, near=30 * 3600):
    """Pull each approximate record boundary onto the transition it really is.

    A wall-clock UNTIL converted with the standard offset alone misses by the
    DST amount in force — Asia/Tbilisi's `2004 Jun 27` lands at 20:00 UTC where
    the transition is at 19:00, because Georgia was on +05 that day. The miss is
    exactly the number this module exists to compute, so it cannot be corrected
    arithmetically. It does not have to be: zic emits a transition at every
    record boundary, so the boundary IS one of the instants in the list, and the
    nearest one within a day and a half is it. Boundaries that snap onto an
    instant already claimed keep their approximation rather than collapsing two
    records onto one."""
    used, out = set(), []
    for b in bounds:
        if b is None:
            out.append(None)
            continue
        best, dist = None, near + 1
        for t in times:
            d = abs(t - b)
            if d < dist and t not in used:
                best, dist = t, d
        if best is None:
            out.append(b)
        else:
            used.add(best)
            out.append(best)
    return out


def saves_for(name, records, trans, types, snap_times=None):
    """save at each transition of `trans` [(time, type_index)].

    Raises when a standard-time transition cannot be matched to a record with
    its own offset — that is the signal the approximation slipped somewhere it
    matters, and it is a refusal rather than a guess."""
    stdoffs = [r.stdoff for r in records]
    bounds = snap(bounds_of(records), snap_times if snap_times is not None
                  else [t for (t, _i) in trans])
    finite = [(b, i) for i, b in enumerate(bounds) if b is not None]

    def record_at(t):
        j = 0
        for b, i in finite:
            if t >= b:
                j = i + 1
            else:
                break
        return min(j, len(records) - 1)

    # How far the UNTIL approximation can be off: a wall-clock boundary misses
    # by the DST amount in force, and `24:00` puts the named day's end a day
    # later. Thirty hours covers both with room to spare, and it is a WINDOW
    # rather than a free search on purpose — the first version corrected
    # whenever the offset did not match, which walked Asia/Tbilisi in 2004 out
    # of the `3:00 RussiaAsia` record it belongs in and into a later one where
    # +04 happens to be standard, turning a one-hour save into none.
    NEAR = 30 * 3600

    out = []
    for (tm, ti) in trans:
        utoff, isdst, _ab = types[ti]
        j = record_at(tm)
        if not isdst and stdoffs[j] != utoff:
            for cand in (j - 1, j + 1):
                if not (0 <= cand < len(stdoffs)) or stdoffs[cand] != utoff:
                    continue
                edge = bounds[min(j, cand)]
                if edge is not None and abs(tm - edge) <= NEAR:
                    j = cand
                    break
        save = utoff - stdoffs[j]

        # ‼ The invariant that makes this derivation checkable at all: a
        # transition is daylight saving exactly when its save is not zero. zic
        # sets the flag from the Rule, and the Rule's SAVE is what this computes
        # from the other side, so the two must agree at every transition or the
        # record assignment slipped. It is not decoration — it caught
        # America/Asuncion, where Paraguay's 2024 abolition of DST put a record
        # boundary at `2024 Oct 15` nine days after the last DST change, and a
        # boundary placed one record too early reported the summer that Paraguay
        # was still in as standard time.
        if bool(save) != bool(isdst):
            raise SystemExit(
                f"{name}: transition at {tm} is {'daylight' if isdst else 'standard'} "
                f"and record {j} (STDOFF {stdoffs[j]}) makes its save {save} — "
                f"the Zone record in force there is not the one this picked")
        out.append(save)
    return out
