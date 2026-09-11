
#include <climits>
#include <clocale>
#include <cstddef>

namespace {

constexpr char kDot[]   = ".";
constexpr char kEmpty[] = "";

char *const kDotP   = const_cast<char *>(kDot);
char *const kEmptyP = const_cast<char *>(kEmpty);

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

char g_name[] = "C";

bool KnownCategory(int category) noexcept
{
    return category == LC_ALL || category == LC_COLLATE || category == LC_CTYPE ||
           category == LC_MONETARY || category == LC_NUMERIC || category == LC_TIME;
}

bool KnownName(const char *locale) noexcept
{
    if (locale[0] == '\0') return true;
    return locale[0] == 'C' && locale[1] == '\0';
}

}

namespace std {

char *setlocale(int category, const char *locale) noexcept
{
    if (!KnownCategory(category)) return nullptr;

    if (!locale) return g_name;

    if (!KnownName(locale)) return nullptr;

    return g_name;
}

lconv *localeconv() noexcept { return &g_lconv; }

}