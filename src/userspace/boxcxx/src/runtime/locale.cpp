#include <atomic>
#include <clocale>
#include <cstddef>
#include <locale>
#include <new>
#include <stdexcept>
#include <string>
#include <typeinfo>

namespace {

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

using NumGetChar  = std::num_get<char>;
using NumGetWchar = std::num_get<wchar_t>;

constinit FacetCell<NumGetChar>  gNumGetChar{};
constinit FacetCell<NumGetWchar> gNumGetWchar{};

using MpCharF  = std::moneypunct<char, false>;
using MpCharI  = std::moneypunct<char, true>;
using MpWcharF = std::moneypunct<wchar_t, false>;
using MpWcharI = std::moneypunct<wchar_t, true>;
using MoneyGetChar  = std::money_get<char>;
using MoneyGetWchar = std::money_get<wchar_t>;
using MoneyPutChar  = std::money_put<char>;
using MoneyPutWchar = std::money_put<wchar_t>;

constinit FacetCell<MpCharF>  gMpCharF{};
constinit FacetCell<MpCharI>  gMpCharI{};
constinit FacetCell<MpWcharF> gMpWcharF{};
constinit FacetCell<MpWcharI> gMpWcharI{};
constinit FacetCell<MoneyGetChar>  gMoneyGetChar{};
constinit FacetCell<MoneyGetWchar> gMoneyGetWchar{};
constinit FacetCell<MoneyPutChar>  gMoneyPutChar{};
constinit FacetCell<MoneyPutWchar> gMoneyPutWchar{};

using TimeGetChar  = std::time_get<char>;
using TimeGetWchar = std::time_get<wchar_t>;
using TimePutChar  = std::time_put<char>;
using TimePutWchar = std::time_put<wchar_t>;

constinit FacetCell<TimeGetChar>  gTimeGetChar{};
constinit FacetCell<TimeGetWchar> gTimeGetWchar{};
constinit FacetCell<TimePutChar>  gTimePutChar{};
constinit FacetCell<TimePutWchar> gTimePutWchar{};

constinit FacetCell<std::collate<char>>     gCollateChar{};
constinit FacetCell<std::collate<wchar_t>>  gCollateWchar{};
constinit FacetCell<std::messages<char>>    gMessagesChar{};
constinit FacetCell<std::messages<wchar_t>> gMessagesWchar{};

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
    {&gNumGetChar.obj,    std::locale::numeric},
    {&gNumGetWchar.obj,   std::locale::numeric},
    {&gCollateChar.obj,   std::locale::collate},
    {&gCollateWchar.obj,  std::locale::collate},
    {&gMessagesChar.obj,  std::locale::messages},
    {&gMessagesWchar.obj, std::locale::messages},
    {&gMpCharF.obj,  std::locale::monetary},
    {&gMpCharI.obj,  std::locale::monetary},
    {&gMpWcharF.obj, std::locale::monetary},
    {&gMpWcharI.obj, std::locale::monetary},
    {&gMoneyGetChar.obj,  std::locale::monetary},
    {&gMoneyGetWchar.obj, std::locale::monetary},
    {&gMoneyPutChar.obj,  std::locale::monetary},
    {&gMoneyPutWchar.obj, std::locale::monetary},
    {&gTimeGetChar.obj,  std::locale::time},
    {&gTimeGetWchar.obj, std::locale::time},
    {&gTimePutChar.obj,  std::locale::time},
    {&gTimePutWchar.obj, std::locale::time},
};

constinit const std::locale::__Table kClassicTable{kClassicSlots,
                                                   std::__cvt::kBuiltinFacets, "C"};

constexpr const char *kUnnamed = "*";

constinit std::atomic_flag             gLock{};
constinit const std::locale::__Table  *gGlobal = &kClassicTable;

struct Guard {
    Guard() noexcept
    {
        while (gLock.test_and_set(std::memory_order_acquire)) {}
    }
    ~Guard() { gLock.clear(std::memory_order_release); }
};

constinit std::atomic<std::size_t> gNextIndex{std::__cvt::kBuiltinFacets};

}

namespace std {

namespace __loc {

size_t NextFacetIndex() noexcept
{
    return gNextIndex.fetch_add(1, memory_order_relaxed);
}

void ThrowBadCast() { throw bad_cast(); }

void ThrowUnknownLocale(const char *name)
{
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

}


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
        const bool take = other.face != nullptr && (other.cat & cat) != 0;
        slots[i]        = take ? other : mine;
    }

    for (size_t i = 0; i < count; ++i)
        if (slots[i].face != nullptr) slots[i].face->__add_ref();

    const bool named = base->__name[0] != '*' && from->__name[0] != '*';
    return new __Table(slots, count, named ? "C" : kUnnamed, false);
}


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
    if (__table == other.__table) return true;
    const char *a = __table->__name_of();
    const char *b = other.__table->__name_of();
    if (a[0] == '*' || b[0] == '*') return false;
    while (*a == *b && *a != '\0') { ++a; ++b; }
    return *a == *b;
}

locale locale::global(const locale &loc)
{
    loc.__table->__acquire();

    const __Table *old;
    {
        Guard g;
        old     = gGlobal;
        gGlobal = loc.__table;
    }

    const char *name = loc.__table->__name_of();
    if (name[0] != '*') setlocale(LC_ALL, name);

    return locale(old);
}

constinit const locale locale::__classic{&kClassicTable};

const locale &locale::classic() noexcept { return __classic; }

}