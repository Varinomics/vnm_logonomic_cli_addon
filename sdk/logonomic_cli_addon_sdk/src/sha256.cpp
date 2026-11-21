#include "logonomic_cli_addon/sha256.h"

#include <cstring>

namespace logonomic::cli_addon {

namespace {

constexpr std::array<std::uint32_t, 64> k_round_constants = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
};

constexpr std::array<std::uint32_t, 8> k_initial_state = {
    0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
    0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u,
};

std::uint32_t rotate_right(std::uint32_t value, unsigned bits)
{
    return (value >> bits) | (value << (32u - bits));
}

std::uint32_t load_big_endian_32(const std::uint8_t* bytes)
{
    return ((std::uint32_t)bytes[0] << 24) | ((std::uint32_t)bytes[1] << 16) |
           ((std::uint32_t)bytes[2] << 8)  |  (std::uint32_t)bytes[3];
}

void store_big_endian_32(std::uint8_t* bytes, std::uint32_t value)
{
    bytes[0] = (std::uint8_t)(value >> 24);
    bytes[1] = (std::uint8_t)(value >> 16);
    bytes[2] = (std::uint8_t)(value >> 8);
    bytes[3] = (std::uint8_t)value;
}

constexpr char k_hex_digits[] = "0123456789abcdef";

} // namespace

Sha256::Sha256()
:
    m_state(k_initial_state),
    m_buffer{}
{}

void Sha256::process_block(const std::uint8_t* block)
{
    std::uint32_t schedule[64];
    for (std::size_t i = 0; i < 16; ++i) {
        schedule[i] = load_big_endian_32(block + 4 * i);
    }
    for (std::size_t i = 16; i < 64; ++i) {
        const std::uint32_t s0 =
            rotate_right(schedule[i - 15], 7) ^ rotate_right(schedule[i - 15], 18) ^ (schedule[i - 15] >> 3);
        const std::uint32_t s1 =
            rotate_right(schedule[i - 2], 17) ^ rotate_right(schedule[i - 2], 19) ^ (schedule[i - 2] >> 10);
        schedule[i] = schedule[i - 16] + s0 + schedule[i - 7] + s1;
    }

    std::uint32_t a = m_state[0];
    std::uint32_t b = m_state[1];
    std::uint32_t c = m_state[2];
    std::uint32_t d = m_state[3];
    std::uint32_t e = m_state[4];
    std::uint32_t f = m_state[5];
    std::uint32_t g = m_state[6];
    std::uint32_t h = m_state[7];

    for (std::size_t i = 0; i < 64; ++i) {
        const std::uint32_t big_sigma_1 = rotate_right(e, 6) ^ rotate_right(e, 11) ^ rotate_right(e, 25);
        const std::uint32_t choice      = (e & f) ^ (~e & g);
        const std::uint32_t temp_1      = h + big_sigma_1 + choice + k_round_constants[i] + schedule[i];
        const std::uint32_t big_sigma_0 = rotate_right(a, 2) ^ rotate_right(a, 13) ^ rotate_right(a, 22);
        const std::uint32_t majority    = (a & b) ^ (a & c) ^ (b & c);
        const std::uint32_t temp_2      = big_sigma_0 + majority;

        h = g;
        g = f;
        f = e;
        e = d + temp_1;
        d = c;
        c = b;
        b = a;
        a = temp_1 + temp_2;
    }

    m_state[0] += a;
    m_state[1] += b;
    m_state[2] += c;
    m_state[3] += d;
    m_state[4] += e;
    m_state[5] += f;
    m_state[6] += g;
    m_state[7] += h;
}

void Sha256::update(const void* data, std::size_t size)
{
    const std::uint8_t* bytes = static_cast<const std::uint8_t*>(data);
    m_total_bytes += size;

    if (m_buffered > 0) {
        const std::size_t take = std::min(size, m_buffer.size() - m_buffered);
        std::memcpy(m_buffer.data() + m_buffered, bytes, take);
        m_buffered += take;
        bytes      += take;
        size       -= take;
        if (m_buffered < m_buffer.size()) {
            return;
        }
        process_block(m_buffer.data());
        m_buffered = 0;
    }

    while (size >= m_buffer.size()) {
        process_block(bytes);
        bytes += m_buffer.size();
        size  -= m_buffer.size();
    }

    if (size > 0) {
        std::memcpy(m_buffer.data(), bytes, size);
        m_buffered = size;
    }
}

Sha256_digest Sha256::finish()
{
    const std::uint64_t bit_length = m_total_bytes * 8u;

    // Padding: 0x80, zeros to 56 mod 64, then the 64-bit big-endian bit length.
    const std::uint8_t terminator = 0x80;
    update(&terminator, 1);
    m_total_bytes -= 1;
    const std::uint8_t zero = 0;
    while (m_buffered != 56) {
        update(&zero, 1);
        m_total_bytes -= 1;
    }

    std::uint8_t length_bytes[8];
    for (std::size_t i = 0; i < 8; ++i) {
        length_bytes[i] = (std::uint8_t)(bit_length >> (56 - 8 * i));
    }
    update(length_bytes, 8);

    Sha256_digest digest;
    for (std::size_t i = 0; i < 8; ++i) {
        store_big_endian_32(digest.data() + 4 * i, m_state[i]);
    }
    return digest;
}

Sha256_digest Sha256::digest(std::string_view bytes)
{
    Sha256 hash;
    hash.update(bytes);
    return hash.finish();
}

std::string to_hex(const std::uint8_t* data, std::size_t size)
{
    std::string hex;
    hex.reserve(size * 2);
    for (std::size_t i = 0; i < size; ++i) {
        hex.push_back(k_hex_digits[data[i] >> 4]);
        hex.push_back(k_hex_digits[data[i] & 0x0F]);
    }
    return hex;
}

std::string to_hex(const Sha256_digest& digest)
{
    return to_hex(digest.data(), digest.size());
}

std::optional<Sha256_digest> sha256_from_hex(std::string_view hex)
{
    if (hex.size() != 64) {
        return std::nullopt;
    }

    Sha256_digest digest{};
    for (std::size_t i = 0; i < 32; ++i) {
        unsigned value = 0;
        for (std::size_t nibble = 0; nibble < 2; ++nibble) {
            const char c = hex[2 * i + nibble];
            unsigned digit = 0;
            if (c >= '0' && c <= '9') {
                digit = (unsigned)(c - '0');
            }
            else
            if (c >= 'a' && c <= 'f') {
                digit = (unsigned)(c - 'a' + 10);
            }
            else {
                return std::nullopt;
            }
            value = (value << 4) | digit;
        }
        digest[i] = (std::uint8_t)value;
    }
    return digest;
}

} // namespace logonomic::cli_addon
