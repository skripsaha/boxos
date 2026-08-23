// tzdb.cpp — the one translation unit that carries the IANA table.
//
// <__bits/chrono_zone> declares four functions and nothing else reaches the
// data; they are defined here, and <__bits/tzdb_table> is included here and
// nowhere else in the library. That is not tidiness — it is what makes the
// 116 KB cost conditional. The linker pulls an archive member only when
// something references one of its symbols, so a program that includes <chrono>
// for durations, calendars or formatting never links this object and never
// carries a byte of zone data. A program that asks a zone a question links all
// of it, once.
//
// The reader itself is <__bits/tzdb_read>: no std types, seconds as long long,
// so the same source compiles on a development host and is measured there
// against libstdc++ and against tzcode's own zdump over 919 417 instants.
// What this file adds is the standard's shape — strings, vectors, exceptions —
// and the two things only BoxOS can answer: which zone this machine is in, and
// whether a newer table has been laid in TagFS.
//
// The text of the two [time.zone.exception] classes is NOT here, and the split
// is load-bearing rather than cosmetic. Those messages render dates, so they
// reference the <format> engine — and a linker pulls whole archive members, so
// keeping them beside the table made every program that merely calls
// locate_zone() drag in the formatter and its Unicode tables. They live in
// tzdb_text.cpp, which is pulled only by a program that actually converts a
// local time and can therefore actually throw.
//
// ── The door ────────────────────────────────────────────────────────────────
// A table baked into an image is a table that cannot be updated, and a zone
// database that cannot be updated is wrong the first time a country moves its
// clocks. So `clock:tzdb` in TagFS is a door: lay a table behind it and
// reload_tzdb() loads it. What arrives through that door is the only untrusted
// input this library has, and tzdata::Validate is the gate — measured by
// corrupting the real table 300 000 ways on a host under ASan, where every
// truncation and every unsorted index was refused and everything admitted was
// read without once stepping outside the blob.
//
// Both databases then stay alive at once. That is not generosity: a program
// holding the `const time_zone*` it got before the reload must keep getting
// right answers from it, which [time.zone.db.remote]/3 requires and which is
// why a time_zone remembers the database it came out of rather than an index
// into "the" table. There is no longer a "the".

#include <__bits/tzdb_read>

#include <atomic>
#include <chrono>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <box/cxx/tagfs.h>

namespace tz = std::__boxcxx::tzdata;

namespace std {
namespace chrono {

namespace {

// ── one loaded database ─────────────────────────────────────────────────
// A Node never moves and is never copied, so its address is a stable handle:
// it is exactly what a time_zone stores as its opaque database pointer. The
// blob is owned here because the Table inside points into it, and the two have
// to die together or not at all.
struct Node {
    tzdb                  db;
    tz::Table             table{};
    vector<unsigned char> bytes;      // empty for the table baked into the image
    Node                 *next = nullptr;
};

Node *build(vector<unsigned char> &&bytes)
{
    Node *n = new Node;
    n->bytes = std::move(bytes);
    n->table = n->bytes.empty() ? tz::kBaked : tz::Bind(n->bytes.data());

    n->db.version = tz::Version(n->table);

    const int count = tz::NameCount(n->table);
    size_t zones = 0, links = 0;
    for (int i = 0; i < count; ++i)
        (tz::LinkTarget(n->table, i) == tz::kNoLink ? zones : links) += 1;
    n->db.zones.reserve(zones);        // addresses must not move: locate_zone
    n->db.links.reserve(links);        // hands out pointers into these

    // The name index is sorted, so both vectors come out sorted by name and
    // locate_zone can binary-search them without touching the table at all.
    for (int i = 0; i < count; ++i) {
        const int target = tz::LinkTarget(n->table, i);
        if (target == tz::kNoLink) n->db.zones.emplace_back(n, i);
        else                       n->db.links.emplace_back(n, i, target);
    }

    const int leaps = tz::LeapCount(n->table);
    n->db.leap_seconds.reserve(static_cast<size_t>(leaps));
    for (int i = 0; i < leaps; ++i) {
        long long when = 0;
        int value = 0;
        tz::LeapAt(n->table, i, when, value);
        n->db.leap_seconds.emplace_back(sys_seconds(seconds(when)),
                                        seconds(value));
    }
    return n;
}

// The list head. Loading publishes with a release store and every reader takes
// it with an acquire load, so a Node is fully built before any other core can
// reach it. Only reload_tzdb and erase_after ever write, and they hold the
// mutex; readers never take it, because asking what time it is must not queue
// behind anything.
atomic<Node *> &head_ref()
{
    static atomic<Node *> h{build({})};
    return h;
}

Node *front_node() noexcept { return head_ref().load(memory_order_acquire); }

mutex &list_mutex()
{
    static mutex m;
    return m;
}

const Node *as_node(const void *db) noexcept
{
    return static_cast<const Node *>(db);
}

// Binary search by name over a vector the builder left sorted. Used for zones
// and for links, which is why it is a template over the element rather than
// two copies of six lines.
template <class Vec>
const typename Vec::value_type *find_by_name(const Vec &v, string_view name)
{
    size_t lo = 0, hi = v.size();
    while (lo < hi) {
        const size_t mid = lo + (hi - lo) / 2;
        const string_view here = v[mid].name();
        if (here == name) return &v[mid];
        if (here < name) lo = mid + 1;
        else             hi = mid;
    }
    return nullptr;
}

// ── the machine's own zone, and the door ────────────────────────────────
// `clock:*` rather than `system:*`: the space beside time, not the space
// beside switching the machine off. The two tags in it are the setting and
// the table.
constexpr const char *kZoneTag      = "clock:zone";
constexpr const char *kTzdbTag      = "clock:tzdb";
constexpr const char *kZoneFallback = "Etc/UTC";

string trimmed(const char *p, size_t n)
{
    string_view text(p, n);
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r'
                             || text.back() == ' ' || text.back() == '\t'))
        text.remove_suffix(1);
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t'))
        text.remove_prefix(1);
    return string(text);
}

// Read on every call rather than cached, and that is a decision. The Touch
// multicast the `timezone` command sends on `clock:zone` exists so a program
// that wants to be TOLD the machine moved can subscribe; a program that merely
// asks gets the current answer because this looked, not because something
// remembered to invalidate a cache. The zone name is a few dozen bytes and
// current_zone() hands back a pointer the caller keeps.
string configured_zone()
{
    auto files = box::tagfs::query(kZoneTag);
    if (files.empty()) return kZoneFallback;

    char buf[64] = {};
    auto got = files.front().read_at(0, buf, sizeof buf - 1);
    if (!got || *got == 0) return kZoneFallback;

    string name = trimmed(buf, *got);
    return name.empty() ? string(kZoneFallback) : name;
}

// Whatever is behind the door, if it is a table. An empty vector means either
// nothing was laid there or what was laid there did not survive the gate —
// which are the same thing to every caller, because in both cases the newest
// database in reach is the one already loaded.
vector<unsigned char> door_blob()
{
    auto files = box::tagfs::query(kTzdbTag);
    if (files.empty()) return {};

    const uint64_t n = files.front().size();
    if (n < 44) return {};             // smaller than the header: not a table

    vector<unsigned char> blob(static_cast<size_t>(n));
    auto got = files.front().read_at(0, blob.data(), blob.size());
    if (!got || *got != blob.size()) return {};
    if (!tz::Validate(blob.data(), blob.size())) return {};
    return blob;
}

} // namespace

namespace __detail {

// ── the functions <__bits/chrono_zone> is allowed to call ───────────────

string_view zone_name(const void *db, int idx) noexcept
{
    const Node *n = as_node(db);
    if (!n || idx < 0 || idx >= tz::NameCount(n->table)) return {};
    return tz::NameAt(n->table, idx);
}

static sys_info from_reader(const tz::Info &in)
{
    sys_info out;
    out.begin = in.begin == tz::kBeginning ? sys_seconds::min()
                                           : sys_seconds(seconds(in.begin));
    out.end   = in.end == tz::kForever ? sys_seconds::max()
                                       : sys_seconds(seconds(in.end));
    out.offset = seconds(in.offset);
    // [time.zone.info] types `save` as minutes, and the generator refuses to
    // write a table where any save is not a whole number of them.
    out.save   = duration_cast<minutes>(seconds(in.save));
    out.abbrev = in.abbrev;
    return out;
}

sys_info zone_info_sys(const void *db, int idx, sys_seconds t)
{
    const Node *n = as_node(db);
    tz::Info info{};
    if (!n || idx < 0
        || !tz::InfoAtSys(n->table, tz::BodyOf(n->table, idx),
                          t.time_since_epoch().count(), info))
        throw runtime_error("chrono: no zone information");
    return from_reader(info);
}

local_info zone_info_local(const void *db, int idx, local_seconds t)
{
    const Node *n = as_node(db);
    local_info out{};
    if (!n || idx < 0) throw runtime_error("chrono: no zone information");

    tz::Info a{}, b{};
    const tz::LocalResult r =
        tz::InfoAtLocal(n->table, tz::BodyOf(n->table, idx),
                        t.time_since_epoch().count(), a, b);
    switch (r) {
    case tz::LocalResult::unique:
        out.result = local_info::unique;
        out.first  = from_reader(a);
        break;
    case tz::LocalResult::nonexistent:
        out.result = local_info::nonexistent;
        out.first  = from_reader(a);
        out.second = from_reader(b);
        break;
    case tz::LocalResult::ambiguous:
        out.result = local_info::ambiguous;
        out.first  = from_reader(a);
        out.second = from_reader(b);
        break;
    }
    return out;
}

const tzdb *db_next(const tzdb *cur) noexcept
{
    for (Node *n = front_node(); n; n = n->next)
        if (&n->db == cur) return n->next ? &n->next->db : nullptr;
    return nullptr;
}

// ── the leap-second arithmetic the clocks stand on ──────────────────────
// Declared in <__bits/chrono_clock>, computed in <__bits/tzdb_read>, measured
// on the host against libstdc++ at every second around every insertion.
//
// These live beside the zone data rather than in an object of their own, and
// that is a decision rather than an oversight: a program that asks how many
// leap seconds have passed is asking the time zone database a question, and
// the database is one table. Splitting them would save 116 KB for a program
// that wants utc_clock and no zones, at the price of a second format and a
// second thing to verify — the exception text was worth splitting out because
// that split cost nothing, and this one would not be free.
//
// They read the FRONT database rather than any particular one, because a UTC
// reading has no zone to belong to. A table loaded through the door that adds
// an insertion moves utc_clock for everybody, which is the point of loading it.

long long leap_elapsed_at_sys(long long t) noexcept
{
    return tz::LeapElapsedAtSys(front_node()->table, t);
}

void leap_at_utc(long long u, bool &is_leap, long long &elapsed) noexcept
{
    tz::LeapAtUtc(front_node()->table, u, is_leap, elapsed);
}

} // namespace __detail

// ── the database ────────────────────────────────────────────────────────

const time_zone *tzdb::locate_zone(string_view tz_name) const
{
    if (const time_zone *z = find_by_name(zones, tz_name)) return z;
    if (const time_zone_link *l = find_by_name(links, tz_name))
        if (const time_zone *z = find_by_name(zones, l->target())) return z;
    throw runtime_error("chrono: no time zone named " + string(tz_name));
}

const time_zone *tzdb::current_zone() const
{
    const string name = configured_zone();
    // A machine that names a zone the table does not have is a machine whose
    // setting is wrong; UTC is the honest answer, not a throw out of a
    // function every clock display calls.
    if (const time_zone *z = find_by_name(zones, name)) return z;
    if (const time_zone_link *l = find_by_name(links, name))
        if (const time_zone *z = find_by_name(zones, l->target())) return z;
    return locate_zone(kZoneFallback);
}

tzdb_list &get_tzdb_list()
{
    static tzdb_list list{tzdb_list::__private_tag{}};
    (void)front_node();
    return list;
}

const tzdb &get_tzdb() { return get_tzdb_list().front(); }

const tzdb &tzdb_list::front() const noexcept { return front_node()->db; }

tzdb_list::const_iterator tzdb_list::begin() const noexcept
{
    return const_iterator(&front_node()->db);
}

tzdb_list::const_iterator tzdb_list::end() const noexcept
{
    return const_iterator(nullptr);
}

tzdb_list::const_iterator tzdb_list::cbegin() const noexcept { return begin(); }
tzdb_list::const_iterator tzdb_list::cend() const noexcept { return end(); }

// [time.zone.db.list]/6: erases the entry AFTER p and returns an iterator to
// the one after that. The erased Node is destroyed, which is what the standard
// means when it says references to it are invalidated — a caller that erases a
// database it still holds zones from asked for exactly that.
tzdb_list::const_iterator tzdb_list::erase_after(const_iterator p)
{
    lock_guard<mutex> guard(list_mutex());

    const tzdb *at = p == end() ? nullptr : &*p;
    for (Node *n = front_node(); n; n = n->next) {
        if (&n->db != at) continue;
        Node *victim = n->next;
        if (!victim) break;
        n->next = victim->next;
        Node *after = n->next;
        delete victim;
        return after ? const_iterator(&after->db) : end();
    }
    return end();
}

// [time.zone.db.remote]. BoxOS has no network; what it has is the TagFS door
// the table was designed around. When nothing is behind it, the newest
// database in reach IS the one in the image, and saying so is the truth rather
// than a placeholder.
string remote_version()
{
    const vector<unsigned char> blob = door_blob();
    if (!blob.empty()) return tz::Version(tz::Bind(blob.data()));
    return front_node()->db.version;
}

// [time.zone.db.remote]/2 says to load when the remote database is NEWER. The
// owner's decision for BoxOS is: load when it is DIFFERENT. The standard says
// newer because behind its door sits a service that only ever moves forward;
// behind this one sits a person, who put that file there on purpose, and a
// deliberate step back to last month's table to undo a bad release is a thing
// a person does. Recorded as a decision in CONFORMANCE §4, not as a gap.
const tzdb &reload_tzdb()
{
    vector<unsigned char> blob = door_blob();
    if (blob.empty()) return get_tzdb();

    (void)get_tzdb_list();          // the baked entry exists before we push
    lock_guard<mutex> guard(list_mutex());

    Node *front = front_node();
    if (tz::Version(tz::Bind(blob.data())) == front->db.version)
        return front->db;

    Node *fresh = build(std::move(blob));
    fresh->next = front;
    head_ref().store(fresh, memory_order_release);
    return fresh->db;
}

const time_zone *locate_zone(string_view tz_name)
{
    return get_tzdb().locate_zone(tz_name);
}

const time_zone *current_zone() { return get_tzdb().current_zone(); }

} // namespace chrono
} // namespace std
