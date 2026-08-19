// boxcxx — <clocale> runtime
//
// One locale, "C", with a UTF-8 multibyte encoding. The lconv below is the set
// of values C fixes for that locale, and because it never changes it is a
// constant that every strand may read at once — see the header for why that is
// a property rather than a limitation.

#include <climits>
#include <clocale>
#include <cstddef>

namespace {

// C's "C" locale: decimal_point is ".", every other string is empty, and every
// char member is CHAR_MAX — which C defines to mean "not available in this
// locale". Zero would be a different claim: that there are none.
constexpr char kDot[]   = ".";
constexpr char kEmpty[] = "";

// The members are non-const char* because that is the type C gives them; the
// storage behind them is never written, and localeconv hands back a pointer to
// a single shared object exactly as C requires.
char *const kDotP   = const_cast<char *>(kDot);
char *const kEmptyP = const_cast<char *>(kEmpty);

// Designated rather than positional: [clocale.syn] fixes the member set but not
// their order, so a positional list is a promise about a layout no standard
// makes. Named initializers cannot silently put CHAR_MAX where a pointer goes.
::std::lconv g_lconv = {
    .decimal_point      = kDotP,
    .thousands_sep      = kEmptyP,
    .grouping           = kEmptyP,
    .mon_decimal_point  = kEmptyP,
    .mon_thousands_sep  = kEmptyP,
    .mon_grouping       = kEmptyP,
    .positive_sign      = kEmptyP,
    .negative_sign      = kEmptyP,
    .currency_symbol    = kEmptyP,
    .frac_digits        = CHAR_MAX,
    .p_cs_precedes      = CHAR_MAX,
    .n_cs_precedes      = CHAR_MAX,
    .p_sep_by_space     = CHAR_MAX,
    .n_sep_by_space     = CHAR_MAX,
    .p_sign_posn        = CHAR_MAX,
    .n_sign_posn        = CHAR_MAX,
    .int_curr_symbol    = kEmptyP,
    .int_frac_digits    = CHAR_MAX,
    .int_p_cs_precedes  = CHAR_MAX,
    .int_n_cs_precedes  = CHAR_MAX,
    .int_p_sep_by_space = CHAR_MAX,
    .int_n_sep_by_space = CHAR_MAX,
    .int_p_sign_posn    = CHAR_MAX,
    .int_n_sign_posn    = CHAR_MAX,
};

// setlocale returns char*, so the name it returns must live in writable
// storage even though nothing ever writes it.
char g_name[] = "C";

bool KnownCategory(int category) noexcept
{
    return category == LC_ALL || category == LC_COLLATE || category == LC_CTYPE ||
           category == LC_MONETARY || category == LC_NUMERIC || category == LC_TIME;
}

bool KnownName(const char *locale) noexcept
{
    // "" is C's spelling of "the implementation-defined native locale", and on
    // BoxOS that IS "C" — there is no environment to name a different one.
    if (locale[0] == '\0') return true;
    return locale[0] == 'C' && locale[1] == '\0';
}

} // namespace

namespace std {

char *setlocale(int category, const char *locale) noexcept
{
    if (!KnownCategory(category)) return nullptr;

    // A null name is a query, and the answer is the only locale there is.
    if (!locale) return g_name;

    // Anything else cannot be honoured, and C's way of saying so is a null
    // return — not a quiet fall back to "C", which would let a program believe
    // it had got what it asked for.
    if (!KnownName(locale)) return nullptr;

    return g_name;
}

lconv *localeconv() noexcept { return &g_lconv; }

} // namespace std
