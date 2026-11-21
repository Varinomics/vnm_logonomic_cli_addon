#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace logonomic::cli_addon {

using Sha256_digest = std::array<std::uint8_t, 32>;

/**
 * @brief Incremental SHA-256 (FIPS 180-4). Feed any number of byte ranges,
 * then finish once; a finished object must not be updated again.
 */
class Sha256
{
public:
    Sha256();

    void update(const void* data, std::size_t size);
    void update(std::string_view bytes) { update(bytes.data(), bytes.size()); }

    Sha256_digest finish();

    static Sha256_digest digest(std::string_view bytes);

private:
    void process_block(const std::uint8_t* block);

    std::array<std::uint32_t, 8> m_state;
    std::array<std::uint8_t, 64> m_buffer;
    std::size_t                  m_buffered = 0;
    std::uint64_t                m_total_bytes = 0;
};

/// Lowercase hexadecimal rendering.
std::string to_hex(const std::uint8_t* data, std::size_t size);
std::string to_hex(const Sha256_digest& digest);

/// Parses exactly 64 lowercase hexadecimal characters.
std::optional<Sha256_digest> sha256_from_hex(std::string_view hex);

} // namespace logonomic::cli_addon
