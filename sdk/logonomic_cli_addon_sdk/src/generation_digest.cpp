#include "logonomic_cli_addon/generation_digest.h"

namespace logonomic::cli_addon {

namespace {

void update_length_prefixed(Sha256& hash, std::string_view bytes)
{
    const std::uint64_t length = bytes.size();
    std::uint8_t prefix[8];
    for (std::size_t i = 0; i < 8; ++i) {
        prefix[i] = (std::uint8_t)(length >> (56 - 8 * i));
    }
    hash.update(prefix, sizeof(prefix));
    hash.update(bytes);
}

} // namespace

std::string compute_generation_digest_hex(
    std::string_view                  bootstrap_bytes,
    std::string_view                  index_bytes,
    const std::vector<Sha256_digest>& file_digests_in_index_order)
{
    Sha256 hash;
    update_length_prefixed(hash, bootstrap_bytes);
    update_length_prefixed(hash, index_bytes);
    for (const Sha256_digest& digest : file_digests_in_index_order) {
        hash.update(digest.data(), digest.size());
    }
    return to_hex(hash.finish());
}

std::string compute_generation_digest_hex(
    std::string_view       bootstrap_bytes,
    std::string_view       index_bytes,
    const Integrity_index& verified_index)
{
    std::vector<Sha256_digest> digests;
    digests.reserve(verified_index.files.size());
    for (const Index_entry& entry : verified_index.files) {
        digests.push_back(sha256_from_hex(entry.sha256_hex).value_or(Sha256_digest{}));
    }
    return compute_generation_digest_hex(bootstrap_bytes, index_bytes, digests);
}

} // namespace logonomic::cli_addon
