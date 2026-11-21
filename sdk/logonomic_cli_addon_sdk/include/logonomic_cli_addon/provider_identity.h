#pragma once

#include <string>
#include <string_view>

namespace logonomic::cli_addon {

/**
 * @brief The host-derived provider identity of an add-on package: the
 * canonical internal provenance `provider_id` of every session the package
 * binds. It satisfies the session-provenance grammar (is_valid_provider_id),
 * so the exact version-1 provenance store and trailer accept it unchanged;
 * the full package ID lives only in the provider binding.
 */

/// "p" + the first 63 lowercase hex digits of SHA-256 over the
/// length-delimited UTF-8 package ID: bytes = decimal(len) ":" utf8(package_id).
std::string derived_provider_id(std::string_view package_id);

/// True for a derived identifier: 'p' followed by 63 lowercase hex digits.
bool is_derived_provider_id(std::string_view provider_id);

} // namespace logonomic::cli_addon
