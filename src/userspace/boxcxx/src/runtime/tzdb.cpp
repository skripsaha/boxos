
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

struct Node {
    tzdb                  db;
    tz::Table             table{};
    vector<unsigned char> bytes;
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
    n->db.zones.reserve(zones);
    n->db.links.reserve(links);

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

vector<unsigned char> door_blob()
{
    auto files = box::tagfs::query(kTzdbTag);
    if (files.empty()) return {};

    const uint64_t n = files.front().size();
    if (n < 44) return {};

    vector<unsigned char> blob(static_cast<size_t>(n));
    auto got = files.front().read_at(0, blob.data(), blob.size());
    if (!got || *got != blob.size()) return {};
    if (!tz::Validate(blob.data(), blob.size())) return {};
    return blob;
}

}

namespace __detail {


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


long long leap_elapsed_at_sys(long long t) noexcept
{
    return tz::LeapElapsedAtSys(front_node()->table, t);
}

void leap_at_utc(long long u, bool &is_leap, long long &elapsed) noexcept
{
    tz::LeapAtUtc(front_node()->table, u, is_leap, elapsed);
}

}


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

string remote_version()
{
    const vector<unsigned char> blob = door_blob();
    if (!blob.empty()) return tz::Version(tz::Bind(blob.data()));
    return front_node()->db.version;
}

const tzdb &reload_tzdb()
{
    vector<unsigned char> blob = door_blob();
    if (blob.empty()) return get_tzdb();

    (void)get_tzdb_list();
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

}
}