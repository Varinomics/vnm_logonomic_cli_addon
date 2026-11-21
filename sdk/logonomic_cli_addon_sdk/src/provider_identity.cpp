#include "logonomic_cli_addon/provider_identity.h"
#include "logonomic_cli_addon/sha256.h"

#include <algorithm>

namespace logonomic::cli_addon {

namespace {

constexpr std::size_t k_derived_hex_digits = 63;

} // namespace

std::string derived_provider_id(std::string_view package_id)
{
    Sha256 digest;
    const std::string length = std::to_string(package_id.size());
    digest.update(length);
    digest.update(":");
    digest.update(package_id);
    return "p" + to_hex(digest.finish()).substr(0, k_derived_hex_digits);
}

bool is_derived_provider_id(std::string_view provider_id)
{
    if (provider_id.size() != 1 + k_derived_hex_digits || provider_id.front() != 'p') {
        return false;
    }
    return std::all_of(provider_id.begin() + 1, provider_id.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}

} // namespace logonomic::cli_addon
