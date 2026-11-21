#include "logonomic_cli_addon/launcher_discovery.h"
#include "logonomic_cli_addon/limits.h"
#include "logonomic_cli_addon/presentation_record.h"
#include "strict_json.h"

#include <algorithm>
#include <set>

namespace logonomic::cli_addon {

namespace {

bool report(Contract_failure* out_failure, const char* reason_code, std::string detail)
{
    if (out_failure != nullptr) {
        out_failure->reason_code = reason_code;
        out_failure->detail      = std::move(detail);
    }
    return false;
}

bool is_windows_platform(std::string_view platform_id)
{
    return platform_id.substr(0, 8) == "windows-";
}

bool valid_ambient_environment(
    const Json_value& environment,
    std::string_view  platform_id,
    std::string*      out_detail)
{
    if (!environment.is_object() ||
        environment.as_object().size() >
            k_ambient_baseline_names.size() + limits::k_max_discovery_environment_names)
    {
        *out_detail = "ambient_environment is not an object within its entry bound";
        return false;
    }
    const bool windows = is_windows_platform(platform_id);
    std::set<std::string> names;
    for (const Json_member& member : environment.as_object()) {
        const std::string comparison_name = windows ? ascii_lowercase(member.name) : member.name;
        if (!is_valid_environment_name(member.name) || is_reserved_environment_name(member.name) ||
            !names.insert(comparison_name).second || !member.value.is_string() ||
            member.value.as_string().size() > limits::k_max_environment_value_bytes)
        {
            *out_detail = "ambient_environment names or values are outside their grammar";
            return false;
        }
    }
    return true;
}

// The candidate defect that fails a whole discover result.
bool candidate_defect(Contract_failure* out_failure, std::size_t position, std::string detail)
{
    return report(out_failure, reason::k_discovery_candidate_invalid,
        "candidates[" + std::to_string(position) + "]: " + std::move(detail));
}

bool read_required_string(const Json_value& object, const char* member, std::string& out_value, std::string* out_detail)
{
    const std::string* value = strict::string_member(object, member);
    if (value == nullptr) {
        *out_detail = std::string(member) + " is missing or not a string";
        return false;
    }
    out_value = *value;
    return true;
}

bool valid_launcher_request(const Launcher_request& launcher, std::string* out_detail)
{
    if (launcher.script_body.empty() || launcher.script_body.size() > limits::k_max_script_body_bytes ||
        launcher.script_body.find('\0') != std::string::npos || !is_valid_utf8(launcher.script_body))
    {
        *out_detail = "script_body is empty, invalid UTF-8, above k_max_script_body_bytes or contains NUL";
        return false;
    }
    if (!is_runner_key(launcher.runner_key)) {
        *out_detail = "runner_key is not a runner key: " + launcher.runner_key;
        return false;
    }
    if (launcher.runner_key == k_runner_direct && launcher.script_body.find_first_of("\r\n") != std::string::npos) {
        *out_detail = "script_body of a direct runner spans more than one line";
        return false;
    }
    if (launcher.environment_text.size() > limits::k_max_environment_text_bytes ||
        launcher.environment_text.find('\0') != std::string::npos || !is_valid_utf8(launcher.environment_text))
    {
        *out_detail = "environment_text is invalid UTF-8, above k_max_environment_text_bytes or contains NUL";
        return false;
    }
    return true;
}

// Every candidate rule except the platform's runner support, which decides
// dropping rather than failure.
bool decode_candidate(const Json_value& value, Launcher_candidate& out_candidate, std::string* out_detail)
{
    if (!value.is_object()) {
        *out_detail = "candidate is not an object";
        return false;
    }
    const std::optional<std::string> unexpected = strict::unexpected_member(value, {
        "candidate_id", "display_name", "script_body", "runner_key", "environment_text", "default", "evidence",
    });
    if (unexpected.has_value()) {
        *out_detail = "unexpected member " + *unexpected;
        return false;
    }

    Launcher_candidate candidate;
    std::string display_name;
    if (!read_required_string(value, "candidate_id", candidate.candidate_id, out_detail) ||
        !read_required_string(value, "display_name", display_name, out_detail) ||
        !read_required_string(value, "script_body", candidate.script_body, out_detail) ||
        !read_required_string(value, "runner_key", candidate.runner_key, out_detail) ||
        !read_required_string(value, "environment_text", candidate.environment_text, out_detail))
    {
        return false;
    }
    if (!is_valid_operation_name(candidate.candidate_id)) {
        *out_detail = "candidate_id is outside the operation-name grammar: " + candidate.candidate_id;
        return false;
    }
    std::string name_detail;
    const std::optional<std::string> normalized = normalized_display_name(display_name, &name_detail);
    if (!normalized.has_value()) {
        *out_detail = "display_name " + name_detail;
        return false;
    }
    candidate.display_name = *normalized;
    if (!valid_launcher_request(
            Launcher_request{candidate.script_body, candidate.runner_key, candidate.environment_text},
            out_detail))
    {
        return false;
    }
    if (const Json_value* is_default = value.find("default"); is_default != nullptr) {
        if (!is_default->is_boolean()) {
            *out_detail = "default is not a boolean";
            return false;
        }
        candidate.is_default = is_default->as_boolean();
    }
    if (const Json_value* evidence = value.find("evidence"); evidence != nullptr) {
        if (!evidence->is_object()) {
            *out_detail = "evidence is not an object";
            return false;
        }
        if (serialize_json_canonical(*evidence).size() > limits::k_max_evidence_bytes) {
            *out_detail = "evidence exceeds k_max_evidence_bytes";
            return false;
        }
        candidate.evidence = *evidence;
    }
    out_candidate = std::move(candidate);
    return true;
}

Json_value encode_candidate(const Launcher_candidate& candidate)
{
    Json_value object = Json_value::object();
    object.set("candidate_id",     Json_value(candidate.candidate_id));
    object.set("display_name",     Json_value(candidate.display_name));
    object.set("script_body",      Json_value(candidate.script_body));
    object.set("runner_key",       Json_value(candidate.runner_key));
    object.set("environment_text", Json_value(candidate.environment_text));
    object.set("default",          Json_value(candidate.is_default));
    object.set("evidence",         candidate.evidence);
    return object;
}

} // namespace

bool validate_ambient_environment(
    const Json_value& environment, std::string_view platform_id, std::string* out_detail)
{
    if (!valid_ambient_environment(environment, platform_id, out_detail)) {
        return false;
    }
    for (const Json_member& entry : environment.as_object()) {
        const std::string& value = entry.value.as_string();
        if (!is_valid_utf8(value) || value.find('\0') != std::string::npos) {
            *out_detail = "environment contains an invalid value";
            return false;
        }
    }
    return true;
}

bool is_runner_key(std::string_view runner_key)
{
    return runner_key == k_runner_direct || runner_key == k_runner_bash ||
           runner_key == k_runner_powershell || runner_key == k_runner_cmd;
}

bool is_runner_supported_on_platform(std::string_view runner_key, std::string_view platform_id)
{
    if (runner_key == k_runner_direct || runner_key == k_runner_bash) {
        return true;
    }
    if (runner_key == k_runner_powershell || runner_key == k_runner_cmd) {
        return is_windows_platform(platform_id);
    }
    return false;
}

std::optional<Launcher_discovery_declaration> decode_launcher_discovery_declaration(
    const Json_value& payload, Contract_failure* out_failure)
{
    if (!payload.is_object()) {
        report(out_failure, reason::k_invalid_sidecar, "payload is not an object");
        return std::nullopt;
    }
    const std::optional<std::string> unexpected = strict::unexpected_member(payload, {"environment_names"});
    if (unexpected.has_value()) {
        report(out_failure, reason::k_invalid_sidecar, "unexpected member " + *unexpected);
        return std::nullopt;
    }
    const Json_value* names = strict::member_of_type(payload, "environment_names", Json_type::ARRAY);
    if (names == nullptr) {
        report(out_failure, reason::k_invalid_sidecar, "environment_names is missing or not an array");
        return std::nullopt;
    }
    if (names->as_array().size() > limits::k_max_discovery_environment_names) {
        report(out_failure, reason::k_invalid_sidecar, "environment_names exceeds k_max_discovery_environment_names");
        return std::nullopt;
    }
    Launcher_discovery_declaration declaration;
    std::set<std::string> seen;
    for (const Json_value& name : names->as_array()) {
        if (!name.is_string() || !is_valid_environment_name(name.as_string())) {
            report(out_failure, reason::k_invalid_sidecar, "environment_names holds a name outside the environment-name grammar");
            return std::nullopt;
        }
        if (is_reserved_environment_name(name.as_string())) {
            report(out_failure, reason::k_reserved_environment_variable, "environment_names holds the reserved name " + name.as_string());
            return std::nullopt;
        }
        if (!seen.insert(name.as_string()).second) {
            report(out_failure, reason::k_invalid_sidecar, "environment_names repeats " + name.as_string());
            return std::nullopt;
        }
        declaration.environment_names.push_back(name.as_string());
    }
    return declaration;
}

Json_value encode_launcher_discovery_declaration(const Launcher_discovery_declaration& declaration)
{
    Json_value names = Json_value::array();
    for (const std::string& name : declaration.environment_names) {
        names.push_back(Json_value(name));
    }
    Json_value payload = Json_value::object();
    payload.set("environment_names", std::move(names));
    return payload;
}

Json_value ambient_view(
    const std::vector<std::pair<std::string, std::string>>& environment,
    const Launcher_discovery_declaration&                   declaration,
    std::string_view                                        platform_id)
{
    const bool case_insensitive = is_windows_platform(platform_id);
    const auto lookup = [&environment, case_insensitive](std::string_view name) -> const std::string* {
        for (const auto& [ambient_name, value] : environment) {
            const bool equal = case_insensitive
                ? ascii_lowercase(ambient_name) == ascii_lowercase(name)
                : ambient_name == name;
            if (equal) {
                return &value;
            }
        }
        return nullptr;
    };

    Json_value view = Json_value::object();
    std::set<std::string> emitted;
    const auto add = [&view, &lookup, &emitted, case_insensitive](std::string_view name) {
        const std::string* value = lookup(name);
        const std::string comparison_name = case_insensitive ? ascii_lowercase(name) : std::string(name);
        if (value != nullptr && value->size() <= limits::k_max_environment_value_bytes && is_valid_utf8(*value) &&
            emitted.insert(comparison_name).second)
        {
            view.set(std::string(name), Json_value(*value));
        }
    };
    for (const char* name : k_ambient_baseline_names) {
        add(name);
    }
    for (const std::string& name : declaration.environment_names) {
        add(name);
    }
    return view;
}

Json_value encode_discover_request(
    const Json_value& ambient_environment,
    std::string_view  default_home_fallback,
    std::string_view  platform_id)
{
    Json_value payload = Json_value::object();
    payload.set("ambient_environment",   ambient_environment);
    payload.set("default_home_fallback", Json_value(default_home_fallback));
    payload.set("platform",              Json_value(platform_id));
    return payload;
}

std::optional<Discover_request> decode_discover_request(const Json_value& payload, Contract_failure* out_failure)
{
    if (!payload.is_object()) {
        report(out_failure, reason::k_frame_violation, "discover payload is not an object");
        return std::nullopt;
    }
    const std::optional<std::string> unexpected =
        strict::unexpected_member(payload, {"ambient_environment", "default_home_fallback", "platform"});
    if (unexpected.has_value()) {
        report(out_failure, reason::k_unexpected_member, "discover." + *unexpected);
        return std::nullopt;
    }
    Discover_request request;
    const Json_value* ambient = strict::member_of_type(payload, "ambient_environment", Json_type::OBJECT);
    if (ambient == nullptr) {
        report(out_failure, reason::k_frame_violation, "ambient_environment is missing or not an object");
        return std::nullopt;
    }
    const std::string* fallback = strict::string_member(payload, "default_home_fallback");
    const std::string* platform = strict::string_member(payload, "platform");
    if (fallback == nullptr || fallback->size() > limits::k_max_working_directory_bytes ||
        fallback->find('\0') != std::string::npos || !is_valid_utf8(*fallback) ||
        platform == nullptr || !is_valid_platform_id(*platform)) {
        report(out_failure, reason::k_frame_violation, "default_home_fallback or platform is missing or out of grammar");
        return std::nullopt;
    }
    std::string ambient_detail;
    if (!valid_ambient_environment(*ambient, *platform, &ambient_detail)) {
        report(out_failure, reason::k_frame_violation, std::move(ambient_detail));
        return std::nullopt;
    }
    request.ambient_environment = *ambient;
    request.default_home_fallback = *fallback;
    request.platform_id           = *platform;
    return request;
}

Json_value encode_launcher_request(const Launcher_request& launcher)
{
    Json_value value = Json_value::object();
    value.set("script_body",      Json_value(launcher.script_body));
    value.set("runner_key",       Json_value(launcher.runner_key));
    value.set("environment_text", Json_value(launcher.environment_text));
    return value;
}

std::optional<Launcher_request> decode_launcher_request(
    const Json_value& value,
    Contract_failure* out_failure)
{
    if (!value.is_object()) {
        report(out_failure, reason::k_frame_violation, "launcher is not an object");
        return std::nullopt;
    }
    const std::optional<std::string> unexpected =
        strict::unexpected_member(value, {"script_body", "runner_key", "environment_text"});
    if (unexpected.has_value()) {
        report(out_failure, reason::k_unexpected_member, "launcher." + *unexpected);
        return std::nullopt;
    }
    Launcher_request launcher;
    std::string detail;
    if (!read_required_string(value, "script_body", launcher.script_body, &detail) ||
        !read_required_string(value, "runner_key", launcher.runner_key, &detail) ||
        !read_required_string(value, "environment_text", launcher.environment_text, &detail) ||
        !valid_launcher_request(launcher, &detail))
    {
        report(out_failure, reason::k_frame_violation, std::move(detail));
        return std::nullopt;
    }
    return launcher;
}

Json_value encode_recognize_request(
    const Launcher_request& launcher,
    const Json_value&       ambient_environment,
    std::string_view        platform_id)
{
    Json_value payload = Json_value::object();
    payload.set("launcher",            encode_launcher_request(launcher));
    payload.set("ambient_environment", ambient_environment);
    payload.set("platform",            Json_value(platform_id));
    return payload;
}

std::optional<Recognize_request> decode_recognize_request(
    const Json_value& payload,
    Contract_failure* out_failure)
{
    if (!payload.is_object()) {
        report(out_failure, reason::k_frame_violation, "recognize payload is not an object");
        return std::nullopt;
    }
    const std::optional<std::string> unexpected =
        strict::unexpected_member(payload, {"launcher", "ambient_environment", "platform"});
    if (unexpected.has_value()) {
        report(out_failure, reason::k_unexpected_member, "recognize." + *unexpected);
        return std::nullopt;
    }
    const Json_value* launcher_value = strict::member_of_type(payload, "launcher", Json_type::OBJECT);
    const Json_value* ambient = strict::member_of_type(payload, "ambient_environment", Json_type::OBJECT);
    const std::string* platform = strict::string_member(payload, "platform");
    if (launcher_value == nullptr || ambient == nullptr || platform == nullptr || !is_valid_platform_id(*platform)) {
        report(out_failure, reason::k_frame_violation,
            "launcher, ambient_environment or platform is missing or mistyped");
        return std::nullopt;
    }
    std::string ambient_detail;
    if (!valid_ambient_environment(*ambient, *platform, &ambient_detail)) {
        report(out_failure, reason::k_frame_violation, std::move(ambient_detail));
        return std::nullopt;
    }
    std::optional<Launcher_request> launcher = decode_launcher_request(*launcher_value, out_failure);
    if (!launcher.has_value()) {
        return std::nullopt;
    }
    return Recognize_request{std::move(*launcher), *ambient, *platform};
}

std::optional<std::vector<Launcher_candidate>> decode_discover_result(
    const Json_value&              payload,
    std::string_view               platform_id,
    Contract_failure*              out_failure,
    std::vector<Contract_failure>* out_rejected)
{
    if (!payload.is_object()) {
        report(out_failure, reason::k_discovery_candidate_invalid, "discover result is not an object");
        return std::nullopt;
    }
    const std::optional<std::string> unexpected = strict::unexpected_member(payload, {"candidates"});
    if (unexpected.has_value()) {
        report(out_failure, reason::k_discovery_candidate_invalid, "unexpected member " + *unexpected);
        return std::nullopt;
    }
    const Json_value* candidates = strict::member_of_type(payload, "candidates", Json_type::ARRAY);
    if (candidates == nullptr) {
        report(out_failure, reason::k_discovery_candidate_invalid, "candidates is missing or not an array");
        return std::nullopt;
    }
    if (candidates->as_array().size() > limits::k_max_discovery_candidates) {
        report(out_failure, reason::k_discovery_candidate_invalid, "more than k_max_discovery_candidates candidates");
        return std::nullopt;
    }

    std::vector<Launcher_candidate> result;
    std::set<std::string> ids;
    bool has_default = false;
    for (std::size_t i = 0; i < candidates->as_array().size(); ++i) {
        Launcher_candidate candidate;
        std::string detail;
        if (!decode_candidate(candidates->as_array()[i], candidate, &detail)) {
            candidate_defect(out_failure, i, std::move(detail));
            return std::nullopt;
        }
        if (!ids.insert(candidate.candidate_id).second) {
            candidate_defect(out_failure, i, "duplicate candidate_id " + candidate.candidate_id);
            return std::nullopt;
        }
        if (!is_runner_supported_on_platform(candidate.runner_key, platform_id)) {
            if (out_rejected != nullptr) {
                out_rejected->push_back(Contract_failure{reason::k_discovery_candidate_invalid,
                    "candidates[" + std::to_string(i) + "] (" + candidate.candidate_id + "): runner " +
                    candidate.runner_key + " is not supported on " + std::string(platform_id)});
            }
            continue;
        }
        if (candidate.is_default) {
            if (has_default) {
                candidate_defect(out_failure, i, "a second candidate is marked default");
                return std::nullopt;
            }
            has_default = true;
        }
        result.push_back(std::move(candidate));
    }
    return result;
}

std::optional<Launcher_recognition> decode_recognize_result(const Json_value& payload, Contract_failure* out_failure)
{
    if (!payload.is_object()) {
        report(out_failure, reason::k_frame_violation, "recognize result is not an object");
        return std::nullopt;
    }
    const Json_value* recognized = strict::member_of_type(payload, "recognized", Json_type::BOOLEAN);
    if (recognized == nullptr) {
        report(out_failure, reason::k_frame_violation, "recognized is missing or not a boolean");
        return std::nullopt;
    }
    Launcher_recognition recognition;
    recognition.recognized = recognized->as_boolean();
    if (!recognition.recognized) {
        const std::optional<std::string> unexpected = strict::unexpected_member(payload, {"recognized"});
        if (unexpected.has_value()) {
            report(out_failure, reason::k_unexpected_member, "recognize." + *unexpected + " with recognized false");
            return std::nullopt;
        }
        return recognition;
    }
    const std::optional<std::string> unexpected =
        strict::unexpected_member(payload, {"recognized", "display_name", "compatibility_warning", "evidence"});
    if (unexpected.has_value()) {
        report(out_failure, reason::k_unexpected_member, "recognize." + *unexpected);
        return std::nullopt;
    }
    const std::string* display_name = strict::string_member(payload, "display_name");
    std::string detail;
    const std::optional<std::string> normalized =
        display_name == nullptr ? std::nullopt : normalized_display_name(*display_name, &detail);
    if (!normalized.has_value()) {
        report(out_failure, reason::k_frame_violation,
            display_name == nullptr ? std::string("display_name is missing or not a string") : "display_name " + detail);
        return std::nullopt;
    }
    recognition.display_name = *normalized;
    if (const Json_value* warning = payload.find("compatibility_warning"); warning != nullptr) {
        if (!warning->is_object() || strict::unexpected_member(*warning, {"code", "message"}).has_value()) {
            report(out_failure, reason::k_frame_violation, "compatibility_warning is not an object of code and message");
            return std::nullopt;
        }
        const std::string* code    = strict::string_member(*warning, "code");
        const std::string* message = strict::string_member(*warning, "message");
        if (code == nullptr || !is_valid_reason_code(*code) || message == nullptr ||
            message->size() > limits::k_max_result_message_bytes || !strict::has_no_control_characters(*message))
        {
            report(out_failure, reason::k_frame_violation, "compatibility_warning code or message is out of grammar");
            return std::nullopt;
        }
        recognition.compatibility_warning = std::make_pair(*code, *message);
    }
    if (const Json_value* evidence = payload.find("evidence"); evidence != nullptr) {
        if (!evidence->is_object() || serialize_json_canonical(*evidence).size() > limits::k_max_evidence_bytes) {
            report(out_failure, reason::k_frame_violation, "evidence is not an object within k_max_evidence_bytes");
            return std::nullopt;
        }
        recognition.evidence = *evidence;
    }
    return recognition;
}

Json_value encode_candidates(const std::vector<Launcher_candidate>& candidates)
{
    Json_value array = Json_value::array();
    for (const Launcher_candidate& candidate : candidates) {
        array.push_back(encode_candidate(candidate));
    }
    Json_value payload = Json_value::object();
    payload.set("candidates", std::move(array));
    return payload;
}

Json_value encode_recognition(const Launcher_recognition& recognition)
{
    Json_value payload = Json_value::object();
    payload.set("recognized", Json_value(recognition.recognized));
    if (!recognition.recognized) {
        return payload;
    }
    payload.set("display_name", Json_value(recognition.display_name));
    if (recognition.compatibility_warning.has_value()) {
        Json_value warning = Json_value::object();
        warning.set("code",    Json_value(recognition.compatibility_warning->first));
        warning.set("message", Json_value(recognition.compatibility_warning->second));
        payload.set("compatibility_warning", std::move(warning));
    }
    payload.set("evidence", recognition.evidence);
    return payload;
}

} // namespace logonomic::cli_addon
