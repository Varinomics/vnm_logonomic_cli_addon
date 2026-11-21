#include "logonomic_cli_addon/usage_repository_attribution.h"

#include "strict_json.h"

#include <limits>

namespace logonomic::cli_addon {
namespace {

bool fail(Contract_failure* out_failure, const char* detail)
{
    if (out_failure != nullptr) {
        *out_failure = {"usage_repository_attribution_invalid", detail};
    }
    return false;
}

} // namespace

bool decode_usage_repository_attribution_declaration(const Json_value& payload, Contract_failure* out_failure)
{
    if (!payload.is_object() || !payload.as_object().empty()) {
        return fail(out_failure, "usage repository attribution declaration must be an empty object");
    }
    return true;
}

Json_value encode_usage_repository_attribution_tag()
{
    auto payload = Json_value::object();
    payload.set("version", Json_value((int)k_usage_repository_attribution_version));
    return payload;
}

Usage_repository_attribution_tag_status decode_usage_repository_attribution_tag(
    const Json_value& payload, Contract_failure* out_failure)
{
    const auto version = strict::integer_member(payload, "version", 1, std::numeric_limits<std::int64_t>::max());
    if (!payload.is_object() || !version) {
        fail(out_failure, "usage repository attribution tag requires a positive integer version");
        return Usage_repository_attribution_tag_status::INVALID;
    }
    if (*version != k_usage_repository_attribution_version) {
        return Usage_repository_attribution_tag_status::UNSUPPORTED;
    }
    if (strict::unexpected_member(payload, {"version"})) {
        fail(out_failure, "usage repository attribution tag has an unexpected member");
        return Usage_repository_attribution_tag_status::INVALID;
    }
    return Usage_repository_attribution_tag_status::AVAILABLE;
}

} // namespace logonomic::cli_addon
