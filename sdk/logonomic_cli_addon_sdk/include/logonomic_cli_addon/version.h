#pragma once

#include <array>
#include <cstdint>

namespace logonomic::cli_addon {

/**
 * @brief Version identity of this SDK and of the contract generations it
 * implements. The protocol, bootstrap and index versions are wire and file
 * contracts; the SDK version is informational.
 */
inline constexpr int k_sdk_version_major = 1;
inline constexpr int k_sdk_version_minor = 0;
inline constexpr int k_sdk_version_patch = 0;

inline constexpr std::uint16_t k_bootstrap_version        = 1;
inline constexpr std::uint16_t k_index_version            = 1;
inline constexpr std::uint16_t k_core_protocol_generation = 1;

/// Core protocol generations this SDK can speak, on both the host and the
/// adapter side.
inline constexpr std::array<std::uint16_t, 1> k_supported_core_protocol_generations = {
    k_core_protocol_generation,
};

/// Fixed file names inside a package directory.
inline constexpr const char* k_bootstrap_file_name = "logonomic-cli-addon.json";
inline constexpr const char* k_index_file_name     = "package.files.json";

} // namespace logonomic::cli_addon
