#pragma once

#include "logonomic_cli_addon/ids.h"
#include "logonomic_cli_addon/json.h"

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace logonomic::cli_addon {

inline constexpr const char* k_provider_event_diagnostic_member = "logonomic_provider_event_diagnostic";

enum class Provider_event_problem
{
    MISSING,
    WRONG_TYPE,
    EMPTY,
    TOO_LARGE,
    CONTROL_CHARACTER,
    MISMATCH,
    INVALID_VALUE,
    INVALID_SHAPE,
    DUPLICATE,
    UNAVAILABLE,
};

/**
 * @brief Value-free diagnostic carried in an existing failure message. Native
 * event names retain the provider-event grammar. Field and rule names are
 * source-defined labels, never provider field values,
 * paths, prompt text or exception text. Sizes describe UTF-8 bytes or container
 * counts unless the source-defined rule names a different measurement unit.
 */
struct Provider_event_diagnostic
{
    std::string                     event_name;
    std::string                     field;
    Provider_event_problem          problem = Provider_event_problem::INVALID_VALUE;
    std::optional<Json_type>         actual_type;
    std::optional<std::uint64_t>     actual_size;
    std::optional<std::uint64_t>     limit;
    std::string                     rule;
};

/// Copies only the JSON type and byte/container size, never the supplied value.
Provider_event_diagnostic make_provider_event_diagnostic(
    std::string                     event_name,
    std::string                     field,
    Provider_event_problem          problem,
    const Json_value*               actual,
    std::optional<std::uint64_t>     limit = {},
    std::string                     rule = {});

/// Invalid source labels or sizes are programmer errors and throw invalid_argument.
std::string encode_provider_event_diagnostic(const Provider_event_diagnostic& diagnostic);

/// Accepts only the bounded, closed diagnostic envelope and its known vocabulary.
std::optional<Provider_event_diagnostic> decode_provider_event_diagnostic(std::string_view message);

/// Keeps optional-result classifiers on their existing explicit failure path.
std::nullopt_t reject_provider_event(Contract_failure* failure, Provider_event_diagnostic diagnostic);

/// A shared exception carrier for classifiers whose existing API throws.
class Provider_event_error : public std::runtime_error
{
public:
    explicit Provider_event_error(Provider_event_diagnostic diagnostic);
    const Provider_event_diagnostic& diagnostic() const noexcept;

private:
    Provider_event_diagnostic m_diagnostic;
};

} // namespace logonomic::cli_addon
