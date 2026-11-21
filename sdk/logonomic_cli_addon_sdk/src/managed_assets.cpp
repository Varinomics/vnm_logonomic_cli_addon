#include "logonomic_cli_addon/managed_assets.h"
#include "logonomic_cli_addon/limits.h"
#include "strict_json.h"

#include <set>

namespace logonomic::cli_addon {

namespace {

std::optional<Managed_assets_declaration> fail(Contract_failure* out_failure, const char* reason_code, std::string detail)
{
    if (out_failure != nullptr) {
        out_failure->reason_code = reason_code;
        out_failure->detail      = std::move(detail);
    }
    return std::nullopt;
}

} // namespace

std::optional<Managed_assets_declaration> decode_managed_assets_declaration(
    const Json_value&      payload,
    const Integrity_index& index,
    Contract_failure*      out_failure)
{
    if (!payload.is_object()) {
        return fail(out_failure, reason::k_invalid_sidecar, "payload is not an object");
    }
    const std::optional<std::string> unexpected = strict::unexpected_member(payload, {"assets"});
    if (unexpected.has_value()) {
        return fail(out_failure, reason::k_invalid_sidecar, "unexpected member " + *unexpected);
    }
    const Json_value* assets = strict::member_of_type(payload, "assets", Json_type::ARRAY);
    if (assets == nullptr) {
        return fail(out_failure, reason::k_invalid_sidecar, "assets is missing or not an array");
    }
    if (assets->as_array().empty() || assets->as_array().size() > limits::k_max_managed_assets) {
        return fail(out_failure, reason::k_invalid_sidecar, "assets must hold 1 to k_max_managed_assets paths");
    }

    Managed_assets_declaration declaration;
    std::set<std::string> seen;
    std::uint64_t total_bytes = 0;
    for (const Json_value& value : assets->as_array()) {
        if (!value.is_string()) {
            return fail(out_failure, reason::k_invalid_sidecar, "assets holds a value that is not a string");
        }
        const std::string& path = value.as_string();
        if (!is_valid_package_relative_path(path) || path_first_segment(path) != k_segment_assets) {
            return fail(out_failure, reason::k_path_escape, "asset is not a canonical path below assets/: " + path);
        }
        if (!seen.insert(path).second) {
            return fail(out_failure, reason::k_invalid_sidecar, "assets repeats " + path);
        }
        const Index_entry* entry = index.find(path);
        if (entry == nullptr || entry->kind != Resource_kind::ASSET) {
            return fail(out_failure, reason::k_asset_reference_invalid, "asset is not indexed with kind asset: " + path);
        }
        if (entry->bytes > limits::k_max_managed_asset_bytes) {
            return fail(out_failure, reason::k_asset_too_large, "asset exceeds k_max_managed_asset_bytes: " + path);
        }
        total_bytes += entry->bytes;
        if (total_bytes > limits::k_max_managed_assets_bytes) {
            return fail(out_failure, reason::k_asset_too_large, "assets together exceed k_max_managed_assets_bytes at " + path);
        }
        declaration.assets.push_back(path);
    }
    return declaration;
}

Json_value encode_managed_assets_declaration(const Managed_assets_declaration& declaration)
{
    Json_value assets = Json_value::array();
    for (const std::string& asset : declaration.assets) {
        assets.push_back(Json_value(asset));
    }
    Json_value payload = Json_value::object();
    payload.set("assets", std::move(assets));
    return payload;
}

} // namespace logonomic::cli_addon
