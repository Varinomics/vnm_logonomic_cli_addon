#include "decimal_parse.h"

#if defined(__APPLE__)
#include <xlocale.h>

#include <cerrno>
#include <cfenv>
#include <cmath>
#include <cstdlib>
#include <string>
#endif

namespace logonomic::cli_addon::detail {

#if defined(__APPLE__)
namespace {

class Decimal_locale
{
public:
    Decimal_locale()
    :
        m_locale(newlocale(LC_NUMERIC_MASK, "C", nullptr))
    {}

    ~Decimal_locale()
    {
        if (m_locale) {
            freelocale(m_locale);
        }
    }

    locale_t get() const { return m_locale; }

private:
    locale_t m_locale;
};

} // namespace
#endif

std::from_chars_result parse_decimal(std::string_view text, double& value)
{
#if defined(__APPLE__)
    // Apple libc++ floating from_chars requires macOS 26. Keep the deployment
    // floor while preserving C-locale, nearest-rounding conversion semantics.
    static const Decimal_locale locale;
    if (!locale.get()) {
        return {text.data(), std::errc::not_enough_memory};
    }
    const std::string terminated(text);
    std::fenv_t environment;
    if (std::feholdexcept(&environment) != 0) {
        return {text.data(), std::errc::not_supported};
    }
    if (std::fesetround(FE_TONEAREST) != 0) {
        (void)std::fesetenv(&environment);
        return {text.data(), std::errc::not_supported};
    }
    char* end = nullptr;
    const int saved_errno = errno;
    errno = 0;
    const double parsed = strtod_l(terminated.c_str(), &end, locale.get());
    const int conversion_errno = errno;
    errno = saved_errno;
    (void)std::fesetenv(&environment);
    const char* parsed_end = text.data() + (end - terminated.c_str());
    if (end == terminated.c_str()) {
        return {parsed_end, std::errc::invalid_argument};
    }
    // strtod_l also sets ERANGE for representable subnormals. from_chars
    // accepts those; only overflow and underflow rounded to zero are rejected.
    if (!std::isfinite(parsed) || (conversion_errno == ERANGE && parsed == 0.0)) {
        return {parsed_end, std::errc::result_out_of_range};
    }
    value = parsed;
    return {parsed_end, {}};
#else
    return std::from_chars(text.data(), text.data() + text.size(), value);
#endif
}

} // namespace logonomic::cli_addon::detail
