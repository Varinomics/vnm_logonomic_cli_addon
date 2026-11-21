#pragma once

#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace logonomic::cli_addon_host::usage_writer_custody {

// A registered allocation is bound to exactly one native generation. The
// identical record lives in that generation and beside the cache allocation.
struct Reference
{
    std::string root;
    std::string accepted_root;
    std::string generation;

    bool operator==(const Reference&) const = default;
};

std::string registry_for_scratch(std::string_view scratch);
std::string encode_reference(const Reference& reference);
std::optional<Reference> decode_reference(std::string_view bytes);

// Only the custodian creates admission, before any provider process is born.
// The registry is outside the nonce-only scratch sweep and survives host death.
bool initialize_generation(const std::string& generation, std::string* out_error);

// Registration and allocation marker publication share the native lock with
// closing admission. Missing or closed registries never admit a writer.
bool begin_write(const Reference& reference, const std::function<bool()>& publish_marker, std::string* out_error);

// Call only after a sealed response, rejected dispatch or native settlement.
// Removal is exact and retryable; an already removed matching marker succeeds.
bool settle_write(const Reference& reference, std::string* out_error);

// Call only after the actual native cleanup authority confirms settlement.
// Persists the proof before any positive Reaped/clean-exit observation is sent.
bool close_generation(const std::string& generation, std::string* out_error);

// Restart collection has no in-memory authority: it needs the exact registered
// pair and durable closed-generation proof. Missing/unknown evidence retains it.
bool settle_if_closed(const Reference& reference);

// Retries interrupted registration removal on restart. Open or unknown
// generations remain untouched; this sweep cannot manufacture settlement.
void recover_closed_generations(std::string_view scratch_root);

} // namespace logonomic::cli_addon_host::usage_writer_custody
