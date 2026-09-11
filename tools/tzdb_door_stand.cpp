
#include <__bits/tzdb_read>
#include "tzdb_mini.h"

#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

namespace tz = std::__boxcxx::tzdata;

static int g_bad = 0;

static void Want(bool ok, const char *what)
{
    if (!ok) { std::printf("  FAIL %s\n", what); ++g_bad; }
}

static std::vector<unsigned char> Fresh()
{
    return std::vector<unsigned char>(tz::kTable, tz::kTable + tz::kTableBytes);
}

static unsigned long long Exercise(const unsigned char *base)
{
    const tz::Table t = tz::Bind(base);
    unsigned long long acc = 0;

    acc += static_cast<unsigned long long>(std::strlen(tz::Version(t)));

    const int n = tz::NameCount(t);
    for (int i = 0; i < n; ++i) {
        const char *nm = tz::NameAt(t, i);
        acc += static_cast<unsigned long long>(std::strlen(nm));
        acc += static_cast<unsigned long long>(
            tz::Find(t, nm, static_cast<unsigned>(std::strlen(nm))) + 1);

        if (tz::LinkTarget(t, i) != tz::kNoLink) continue;

        const int body = tz::BodyOf(t, i);
        static const long long kWhen[] = {
            -4000000000LL, -1000000000LL, 0LL, 1000000000LL, 1784073600LL,
            2000000000LL, 4000000000LL, 8000000000LL,
        };
        for (long long w : kWhen) {
            tz::Info info{};
            if (tz::InfoAtSys(t, body, w, info)) acc += info.abbrev[0] + info.offset;
            tz::Info a{}, b{};
            (void)tz::InfoAtLocal(t, body, w, a, b);
            acc += static_cast<unsigned long long>(a.offset)
                 + static_cast<unsigned long long>(b.offset);
        }
    }

    for (int i = 0, m = tz::LeapCount(t); i < m; ++i) {
        long long when = 0; int value = 0;
        tz::LeapAt(t, i, when, value);
        acc += static_cast<unsigned long long>(when)
             + static_cast<unsigned long long>(value);
    }
    bool leap = false; long long elapsed = 0;
    tz::LeapAtUtc(t, 1483228826LL, leap, elapsed);
    acc += static_cast<unsigned long long>(elapsed) + (leap ? 1u : 0u);
    acc += static_cast<unsigned long long>(tz::LeapElapsedAtSys(t, 1700000000LL));
    return acc;
}


static void WantInfo(const tz::Table &t, const char *zone, long long at,
                     int offset, int save, const char *abbrev)
{
    const int i = tz::Find(t, zone, static_cast<unsigned>(std::strlen(zone)));
    if (i < 0) { std::printf("  FAIL %s: not found\n", zone); ++g_bad; return; }
    int body = tz::BodyOf(t, i);
    if (tz::LinkTarget(t, i) != tz::kNoLink)
        body = tz::BodyOf(t, tz::LinkTarget(t, i));

    tz::Info info{};
    if (!tz::InfoAtSys(t, body, at, info)) {
        std::printf("  FAIL %s at %lld: no answer\n", zone, at);
        ++g_bad;
        return;
    }
    if (info.offset != offset || info.save != save
        || std::strcmp(info.abbrev, abbrev) != 0) {
        std::printf("  FAIL %s at %lld: got %d/%d/%s want %d/%d/%s\n", zone, at,
                    info.offset, info.save, info.abbrev, offset, save, abbrev);
        ++g_bad;
    }
}

static void MiniTable()
{
    std::printf("-- the table behind the door (%u bytes) --\n", kMiniTzdbBytes);

    Want(tz::Validate(kMiniTzdb, kMiniTzdbBytes), "the gate accepts it");
    if (g_bad) return;

    const tz::Table t = tz::Bind(kMiniTzdb);
    Want(std::strcmp(tz::Version(t), "boxtest1") == 0, "version");
    Want(tz::NameCount(t) == 4, "four names");
    Want(tz::LeapCount(t) == 28, "the baked 27 plus one");

    for (int i = 1; i < tz::NameCount(t); ++i)
        Want(std::strcmp(tz::NameAt(t, i - 1), tz::NameAt(t, i)) < 0, "sorted");

    const int alias = tz::Find(t, "Box/Alias", 9);
    Want(alias >= 0, "Box/Alias is present");
    const int target = alias >= 0 ? tz::LinkTarget(t, alias) : -1;
    Want(target >= 0 && std::strcmp(tz::NameAt(t, target), "Box/Test") == 0,
         "Box/Alias -> Box/Test");
    Want(target >= 0 && tz::LinkTarget(t, target) == tz::kNoLink,
         "and the target is a Zone, not another Link");

    Want(tz::Find(t, "America/New_York", 16) < 0,
         "the baked table's zones are NOT in here");

    WantInfo(t, "Box/Shift", 0, 3600, 0, "TSTA");
    WantInfo(t, "Box/Shift", 999999999, 3600, 0, "TSTA");
    WantInfo(t, "Box/Shift", 1000000000, 7200, 0, "TSTB");
    WantInfo(t, "Box/Shift", 1500000000, 7200, 0, "TSTB");
    WantInfo(t, "Box/Test", -3000000000LL, 19800, 0, "+0530");
    WantInfo(t, "Box/Test", 4000000000LL, 19800, 0, "+0530");
    WantInfo(t, "Box/Alias", 0, 19800, 0, "+0530");
    WantInfo(t, "Etc/UTC", 0, 0, 0, "UTC");

    long long when = 0; int value = 0;
    tz::LeapAt(t, 0, when, value);
    Want(when == 78796800 && value == 1, "the first insertion is 1972-07-01");
    tz::LeapAt(t, 26, when, value);
    Want(when == 1483228800 && value == 1, "the 27th is 2017-01-01");
    tz::LeapAt(t, 27, when, value);
    Want(when == 4102444800LL && value == 1, "and the test's own is in 2100");
    Want(tz::LeapElapsedAtSys(t, 1700000000LL) == 27, "27 elapsed by 2023");
    Want(tz::LeapElapsedAtSys(t, 4200000000LL) == 28, "28 elapsed by 2103");

    {
        tz::Info a{}, b{};
        const int i = tz::Find(t, "Box/Shift", 9);
        const tz::LocalResult r =
            tz::InfoAtLocal(t, tz::BodyOf(t, i), 1000000000LL + 3600 + 1800, a, b);
        Want(r == tz::LocalResult::nonexistent, "the skipped half hour is a gap");
    }
}


static void TheGate()
{
    std::printf("-- the gate, against the real table (%u bytes) --\n",
                tz::kTableBytes);

    Want(tz::Validate(tz::kTable, tz::kTableBytes),
         "the baked table passes its own gate");
    if (g_bad) return;
    {
        auto v = Fresh();
        std::printf("baked table: accepted, checksum %llu\n", Exercise(v.data()));
    }

    {
        auto v = Fresh();
        long long accepted = 0;
        for (unsigned long long len = 0; len < tz::kTableBytes; ++len)
            if (tz::Validate(v.data(), len)) {
                if (accepted < 4)
                    std::printf("  TRUNCATION ACCEPTED at %llu bytes\n", len);
                ++accepted;
            }
        std::printf("truncation: %u lengths tried, %lld accepted\n",
                    tz::kTableBytes, accepted);
        Want(accepted == 0, "every truncation is refused");
    }

    {
        auto v = Fresh();
        const tz::Table t = tz::Bind(v.data());
        unsigned char *e = v.data() + t.head.off_names + 100 * 8;
        unsigned char tmp[8];
        std::memcpy(tmp, e, 8);
        std::memcpy(e, e + 8, 8);
        std::memcpy(e + 8, tmp, 8);
        Want(!tz::Validate(v.data(), v.size()),
             "an unsorted name index is refused");
    }

    {
        auto v = Fresh();
        const tz::Table t = tz::Bind(v.data());
        int a_link = -1, another = -1;
        for (int i = 0, n = tz::NameCount(t); i < n; ++i)
            if (tz::LinkTarget(t, i) != tz::kNoLink) {
                if (a_link < 0) a_link = i; else { another = i; break; }
            }
        if (a_link >= 0 && another >= 0) {
            unsigned char *e = v.data() + t.head.off_names + a_link * 8;
            e[6] = static_cast<unsigned char>(another & 0xFF);
            e[7] = static_cast<unsigned char>((another >> 8) & 0xFF);
            Want(!tz::Validate(v.data(), v.size()),
                 "a link pointing at a link is refused");
        }
    }

    {
        std::mt19937_64 rng(20260823);
        long long accepted = 0, refused = 0;
        const int kRounds = 300000;
        auto v = Fresh();
        const tz::Table ref = tz::Bind(tz::kTable);
        const unsigned long long index_end = ref.head.off_bodytab
                                           + 4ull * ref.head.n_bodies;

        for (int round = 0; round < kRounds; ++round) {
            std::memcpy(v.data(), tz::kTable, tz::kTableBytes);
            const int burst = 1 + int(rng() % 4);
            for (int k = 0; k < burst; ++k) {
                const unsigned long long off = (rng() % 2)
                    ? rng() % index_end
                    : rng() % tz::kTableBytes;
                v[off] = static_cast<unsigned char>(rng() & 0xFF);
            }
            if (tz::Validate(v.data(), v.size())) {
                ++accepted;
                (void)Exercise(v.data());
            } else {
                ++refused;
            }
        }
        std::printf("fuzz: %d mutations - %lld refused, %lld accepted and "
                    "fully exercised with no read outside the blob\n",
                    kRounds, refused, accepted);
    }

    {
        static const unsigned kHostile[] = {
            0u, 1u, 2u, 7u, 43u, 44u, 0x7FFFu, 0x8000u, 0xFFFFu,
            119051u, 119052u, 119053u, 0x7FFFFFFFu, 0x80000000u,
            0xFFFFFFF0u, 0xFFFFFFFFu,
        };
        long long accepted = 0, tried = 0;
        auto v = Fresh();
        for (int field = 0; field < 9; ++field)
            for (unsigned hostile : kHostile) {
                std::memcpy(v.data(), tz::kTable, tz::kTableBytes);
                unsigned char *w = v.data() + 8 + field * 4;
                w[0] = static_cast<unsigned char>(hostile & 0xFF);
                w[1] = static_cast<unsigned char>((hostile >> 8) & 0xFF);
                w[2] = static_cast<unsigned char>((hostile >> 16) & 0xFF);
                w[3] = static_cast<unsigned char>((hostile >> 24) & 0xFF);
                ++tried;
                if (tz::Validate(v.data(), v.size())) {
                    ++accepted;
                    (void)Exercise(v.data());
                }
            }
        std::printf("hostile header words: %lld tried, %lld accepted and "
                    "exercised safely\n", tried, accepted);
    }
}

int main()
{
    MiniTable();
    TheGate();
    std::printf("tzdb_door_stand: %s\n", g_bad ? "FAIL" : "PASS");
    return g_bad ? 1 : 0;
}