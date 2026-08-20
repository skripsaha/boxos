// boxcxx — the locale table, out of line.
//
// Three things live here and nowhere else, each for a reason the header could
// not satisfy.
//
//   1. THE FACETS THE IMAGE CARRIES. They are in one translation unit rather
//      than inline in a header so their destructor registration — see below —
//      happens once for the program instead of once per translation unit.
//
//   2. THE CLASSIC TABLE. Constant-initialized, so a facet lookup on
//      locale::classic() is two loads with nothing built at startup and no
//      guard variable. It cannot be assembled in the header because it names
//      every facet the image has, and those are here.
//
//   3. ALLOCATION AND TEARDOWN. Building a locale with a facet installed
//      allocates, and ending one runs facet destructors. Neither belongs in a
//      header that <ios> pulls into every translation unit that prints.
//
// ── the union cell, and what it is for ──────────────────────────────────
// A facet's destructor is virtual ([locale.facet] needs it: a locale deletes
// facets it owns through the base). A constant-initialized object with a
// virtual destructor gets a __cxa_atexit registration, and running that
// destructor during static teardown REWRITES THE OBJECT'S VPTR to a base that
// has no do_in — so a facet would stop answering while the program was still
// using it, which is worse than never freeing it at all.
//
// A union does not destroy its active member. So each image facet lives inside
// one whose own destructor is empty: the registration still happens, the
// destructor still runs, and it does nothing to the facet inside. Measured,
// not assumed — see the table at the top of <__bits/locale_core>.
#include <atomic>
#include <clocale>
#include <cstddef>
#include <locale>
#include <new>
#include <stdexcept>
#include <string>
#include <typeinfo>

namespace {

// The cell described above. `obj` is never destroyed, on purpose.
template <class F>
union FacetCell {
    std::__FacetHome<F> obj;
    constexpr FacetCell() noexcept : obj() {}
    ~FacetCell() {}
};

using CvtChar   = std::codecvt<char, char, std::mbstate_t>;
using CvtWchar  = std::codecvt<wchar_t, char, std::mbstate_t>;
using CvtChar16 = std::codecvt<char16_t, char8_t, std::mbstate_t>;
using CvtChar32 = std::codecvt<char32_t, char8_t, std::mbstate_t>;

constinit FacetCell<CvtChar>   gCvtChar{};
constinit FacetCell<CvtWchar>  gCvtWchar{};
constinit FacetCell<CvtChar16> gCvtChar16{};
constinit FacetCell<CvtChar32> gCvtChar32{};

constinit FacetCell<std::ctype<char>>    gCtypeChar{};
constinit FacetCell<std::ctype<wchar_t>> gCtypeWchar{};

using NumPutChar  = std::num_put<char>;
using NumPutWchar = std::num_put<wchar_t>;

constinit FacetCell<std::numpunct<char>>    gNumpunctChar{};
constinit FacetCell<std::numpunct<wchar_t>> gNumpunctWchar{};
constinit FacetCell<NumPutChar>             gNumPutChar{};
constinit FacetCell<NumPutWchar>            gNumPutWchar{};

// Table 104 of [locale.category] puts all four codecvts in the ctype
// category, which is what the category-combining constructors select on.
constinit const std::__loc::Slot kClassicSlots[std::__cvt::kBuiltinFacets] = {
    {&gCvtChar.obj,   std::locale::ctype},
    {&gCvtWchar.obj,  std::locale::ctype},
    {&gCvtChar16.obj, std::locale::ctype},
    {&gCvtChar32.obj, std::locale::ctype},
    {&gCtypeChar.obj,  std::locale::ctype},
    {&gCtypeWchar.obj, std::locale::ctype},
    {&gNumpunctChar.obj,  std::locale::numeric},
    {&gNumpunctWchar.obj, std::locale::numeric},
    {&gNumPutChar.obj,    std::locale::numeric},
    {&gNumPutWchar.obj,   std::locale::numeric},
};

constinit const std::locale::__Table kClassicTable{kClassicSlots,
                                                   std::__cvt::kBuiltinFacets, "C"};

// The name every unnamed locale reports. [locale.members]/2: a locale that had
// a facet installed into it has no name, and "*" is the spelling both
// reference implementations use for that.
constexpr const char *kUnnamed = "*";

// ── the global locale ───────────────────────────────────────────────────
// A pointer and a spin lock rather than a locale object, for two reasons: a
// locale object with a nontrivial destructor would register one more atexit,
// and the read side has to take a reference under the same lock that the write
// side swaps under. Without that, a reader could publish a table between
// another thread reading the pointer and counting it.
constinit std::atomic_flag             gLock{};
constinit const std::locale::__Table  *gGlobal = &kClassicTable;

struct Guard {
    Guard() noexcept
    {
        while (gLock.test_and_set(std::memory_order_acquire)) {}
    }
    ~Guard() { gLock.clear(std::memory_order_release); }
};

// Facets the image does not carry are numbered from here on, one per
// locale::id, the first time anybody asks.
constinit std::atomic<std::size_t> gNextIndex{std::__cvt::kBuiltinFacets};

} // namespace

namespace std {

namespace __loc {

size_t NextFacetIndex() noexcept
{
    return gNextIndex.fetch_add(1, memory_order_relaxed);
}

void ThrowBadCast() { throw bad_cast(); }

void ThrowUnknownLocale(const char *name)
{
    // The name is quoted into the message because the whole point of refusing
    // it is to say which name was refused.
    string what = "locale: no locale named \"";
    what += (name != nullptr ? name : "(null)");
    what += "\" — this system has \"C\" and no other";
    throw runtime_error(what);
}

void ThrowNoSuchFacet()
{
    throw runtime_error("locale::combine: the source locale has no such facet");
}

bool IsClassicName(const char *name) noexcept
{
    if (name == nullptr) return false;
    // "" is the standard's spelling for "the native environment", and this
    // environment is "C".
    if (name[0] == '\0') return true;
    const char *c = "C";
    const char *p = "POSIX";
    const char *a = name;
    while (*a == *c && *a != '\0') { ++a; ++c; }
    if (*a == '\0' && *c == '\0') return true;
    a = name;
    while (*a == *p && *a != '\0') { ++a; ++p; }
    return *a == '\0' && *p == '\0';
}

} // namespace __loc

// ── locale::__Table ─────────────────────────────────────────────────────

const locale::__Table *locale::__Table::__classic() noexcept { return &kClassicTable; }

void locale::__Table::__destroy() const noexcept
{
    for (size_t i = 0; i < __count; ++i)
        if (__slots[i].face != nullptr) __slots[i].face->__remove_ref();
    delete[] __slots;
    delete this;
}

const locale::__Table *locale::__Table::__with_facet(const __Table *base, size_t index,
                                                     const facet *f)
{
    const size_t count = index + 1 > base->__count ? index + 1 : base->__count;

    __loc::Slot *slots = new __loc::Slot[count];
    for (size_t i = 0; i < count; ++i) {
        if (i < base->__count)
            slots[i] = base->__slots[i];
        else
            slots[i] = __loc::Slot{nullptr, locale::none};
    }

    // The category belongs to the SLOT, not to the object in it: a facet is in
    // the category its locale::id names, whoever wrote the class. So an
    // installed numpunct is still `numeric`, and only an id the image never
    // heard of lands in no category at all.
    const int cat  = index < base->__count ? base->__slots[index].cat : locale::none;
    slots[index]   = __loc::Slot{f, cat};

    f->__add_ref();
    for (size_t i = 0; i < count; ++i)
        if (i != index && slots[i].face != nullptr) slots[i].face->__add_ref();

    return new __Table(slots, count, kUnnamed, false);
}

const locale::__Table *locale::__Table::__combine(const __Table *base, const __Table *from,
                                                  category cat)
{
    const size_t count = from->__count > base->__count ? from->__count : base->__count;

    __loc::Slot *slots = new __loc::Slot[count];
    for (size_t i = 0; i < count; ++i) {
        const __loc::Slot mine  = i < base->__count ? base->__slots[i]
                                                    : __loc::Slot{nullptr, locale::none};
        const __loc::Slot other = i < from->__count ? from->__slots[i]
                                                    : __loc::Slot{nullptr, locale::none};
        // [locale.cons]: the facets of `from` that are in `cat`, and the facets
        // of `base` that are not. A slot `from` does not fill cannot win even
        // when its category matches — there is nothing there to take.
        const bool take = other.face != nullptr && (other.cat & cat) != 0;
        slots[i]        = take ? other : mine;
    }

    for (size_t i = 0; i < count; ++i)
        if (slots[i].face != nullptr) slots[i].face->__add_ref();

    // Naming: the result keeps a name only if both sources had one. Every
    // named locale here is "C", so that is the only name it can be.
    const bool named = base->__name[0] != '*' && from->__name[0] != '*';
    return new __Table(slots, count, named ? "C" : kUnnamed, false);
}

// ── locale [locale.cons] / [locale.members] / [locale.statics] ──────────

locale::locale() noexcept
{
    Guard g;
    __table = gGlobal;
    __table->__acquire();
}

locale::locale(const char *std_name)
{
    if (!__loc::IsClassicName(std_name)) __loc::ThrowUnknownLocale(std_name);
    __table = &kClassicTable;
    __table->__acquire();
}

locale::locale(const locale &other, const char *std_name, category cat)
{
    if (!__loc::IsClassicName(std_name)) __loc::ThrowUnknownLocale(std_name);
    __table = __Table::__combine(other.__table, &kClassicTable, cat);
}

locale::locale(const locale &other, const locale &one, category cat)
    : __table(__Table::__combine(other.__table, one.__table, cat))
{
}

string locale::name() const { return string(__table->__name_of()); }

bool locale::operator==(const locale &other) const
{
    // [locale.members]/1: the same object, or two locales with the same name.
    // An unnamed locale equals only itself, which is why the name is checked
    // for "*" rather than merely compared.
    if (__table == other.__table) return true;
    const char *a = __table->__name_of();
    const char *b = other.__table->__name_of();
    if (a[0] == '*' || b[0] == '*') return false;
    while (*a == *b && *a != '\0') { ++a; ++b; }
    return *a == *b;
}

locale locale::global(const locale &loc)
{
    loc.__table->__acquire();   // the global itself holds one reference

    const __Table *old;
    {
        Guard g;
        old     = gGlobal;
        gGlobal = loc.__table;
    }

    // [locale.statics]/2: a named locale also becomes the C locale. There is
    // one name and setlocale here never changes anything, so this is the
    // statement of intent rather than a state change — and it is made anyway,
    // because the day a second name exists this line is already correct.
    const char *name = loc.__table->__name_of();
    if (name[0] != '*') setlocale(LC_ALL, name);

    // The reference the global was holding is handed to the returned object
    // rather than dropped and retaken, so the table cannot die in between.
    return locale(old);
}

// Constant-initialized, like the table it names: asking for the classic locale
// is an address, not a call, and it answers during static teardown because
// nothing here was ever built. Its destructor is registered and does nothing —
// the table it names is immortal, so releasing it is a branch that returns.
constinit const locale locale::__classic{&kClassicTable};

const locale &locale::classic() noexcept { return __classic; }

} // namespace std
