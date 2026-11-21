#include "logonomic_cli_addon/sidecar.h"
#include "logonomic_cli_addon/limits.h"
#include "strict_json.h"

namespace logonomic::cli_addon {

namespace {

std::optional<Capability_sidecar> fail(
    Contract_failure* out_failure,
    const char*       reason_code,
    std::string       detail)
{
    if (out_failure != nullptr) {
        out_failure->reason_code = reason_code;
        out_failure->detail      = std::move(detail);
    }
    return std::nullopt;
}

} // namespace

std::optional<Capability_sidecar> parse_sidecar(std::string_view bytes, Contract_failure* out_failure)
{
    if (bytes.size() > limits::k_max_sidecar_bytes) {
        return fail(out_failure, reason::k_custody_bound_exceeded, "k_max_sidecar_bytes exceeded by a sidecar");
    }
    if (!is_valid_utf8(bytes)) {
        return fail(out_failure, reason::k_invalid_utf8, "sidecar is not valid UTF-8");
    }

    Json_parse_limits parse_limits;
    parse_limits.max_bytes = limits::k_max_sidecar_bytes;
    Json_parse_error parse_error;
    const std::optional<Json_value> document = parse_json(bytes, parse_limits, &parse_error);
    if (!document.has_value()) {
        return fail(out_failure, reason::k_invalid_sidecar,
            "sidecar is not valid JSON at byte " + std::to_string(parse_error.offset) + ": " + parse_error.message);
    }
    if (!document->is_object()) {
        return fail(out_failure, reason::k_invalid_sidecar, "sidecar is not a JSON object");
    }
    const std::optional<std::string> unexpected = strict::unexpected_member(*document, {"capability_id", "version", "payload"});
    if (unexpected.has_value()) {
        return fail(out_failure, reason::k_invalid_sidecar, "unexpected member " + *unexpected);
    }

    Capability_sidecar sidecar;
    const std::string* capability_id = strict::string_member(*document, "capability_id");
    if (capability_id == nullptr || !is_valid_capability_id(*capability_id)) {
        return fail(out_failure, reason::k_invalid_sidecar, "capability_id is missing or out of grammar");
    }
    sidecar.capability_id = *capability_id;

    const std::optional<std::int64_t> version = strict::integer_member(*document, "version", 0, limits::k_max_version);
    if (!version.has_value()) {
        return fail(out_failure, reason::k_invalid_sidecar, "version is missing or not a version integer");
    }
    sidecar.version = (std::uint16_t)*version;

    const Json_value* payload = document->find("payload");
    if (payload == nullptr) {
        return fail(out_failure, reason::k_invalid_sidecar, "payload is missing");
    }
    sidecar.payload = *payload;
    return sidecar;
}

std::string serialize_sidecar(const Capability_sidecar& sidecar)
{
    Json_value document = Json_value::object();
    document.set("capability_id", Json_value(sidecar.capability_id));
    document.set("version",       Json_value((std::int64_t)sidecar.version));
    document.set("payload",       sidecar.payload);
    return serialize_json(document);
}

std::string sidecar_file_name(std::string_view capability_id, std::uint16_t version)
{
    return std::string(capability_id) + "@" + std::to_string(version) + ".json";
}

std::string sidecar_relative_path(std::string_view capability_id, std::uint16_t version)
{
    return std::string(k_segment_capabilities) + "/" + sidecar_file_name(capability_id, version);
}

} // namespace logonomic::cli_addon
