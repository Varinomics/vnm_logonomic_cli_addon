#include "logonomic_cli_addon/launch_preparation.h"
#include "logonomic_cli_addon/capability_ids.h"
#include "logonomic_cli_addon/limits.h"
#include "logonomic_cli_addon/package_source.h"
#include "logonomic_cli_addon/sha256.h"
#include "strict_json.h"
#include "launch_patch_fields_p.h"

#include <algorithm>
#include <cstdint>
#include <set>
#include <stdexcept>

namespace logonomic::cli_addon {

namespace {

constexpr std::string_view k_token_open       = "{{";
constexpr std::string_view k_token_close      = "}}";
constexpr std::string_view k_asset_prefix     = "asset:";
constexpr std::string_view k_asset_json_prefix = "asset_json:";

bool report(Contract_failure* out_failure, const char* reason_code, std::string detail)
{
    if (out_failure != nullptr) {
        out_failure->reason_code = reason_code;
        out_failure->detail      = std::move(detail);
    }
    return false;
}

std::optional<Launch_patch> patch_defect(Contract_failure* out_failure, std::string detail)
{
    report(out_failure, reason::k_launch_patch_invalid, std::move(detail));
    return std::nullopt;
}

bool is_asset_path(std::string_view path)
{
    return is_valid_package_relative_path(path) && path_first_segment(path) == k_segment_assets;
}

bool contains(const std::vector<std::string>& paths, const std::string& path)
{
    return std::find(paths.begin(), paths.end(), path) != paths.end();
}

bool opens_token(std::string_view value)
{
    return value.find(k_token_open) != std::string_view::npos;
}

Json_value encode_initial_prompt_request(const Initial_prompt_request& prompt)
{
    Json_value member = Json_value::object();
    member.set("version",         Json_value(k_interactive_initial_prompt_version));
    member.set("text",            Json_value(prompt.text));
    member.set("referenced_file", Json_value(prompt.referenced_file));
    return member;
}

std::optional<Initial_prompt_request> decode_initial_prompt_request(
    const Json_value& value, Contract_failure* out_failure)
{
    if (!value.is_object()) {
        report(out_failure, reason::k_frame_violation, "prepare.initial_prompt is not an object");
        return std::nullopt;
    }
    const std::optional<std::string> unexpected =
        strict::unexpected_member(value, {"version", "text", "referenced_file"});
    if (unexpected.has_value()) {
        report(out_failure, reason::k_unexpected_member, "prepare.initial_prompt." + *unexpected);
        return std::nullopt;
    }
    const std::string* text = strict::string_member(value, "text");
    const std::string* file = strict::string_member(value, "referenced_file");
    if (!strict::integer_member(value, "version",
            k_interactive_initial_prompt_version, k_interactive_initial_prompt_version) ||
        text == nullptr || file == nullptr)
    {
        report(out_failure, reason::k_frame_violation,
            "prepare.initial_prompt needs version 1, text and referenced_file");
        return std::nullopt;
    }
    if (text->size() > limits::k_max_initial_prompt_text_bytes) {
        report(out_failure, reason::k_payload_too_large,
            "prepare.initial_prompt.text exceeds k_max_initial_prompt_text_bytes");
        return std::nullopt;
    }
    if (text->empty() || !is_valid_utf8(*text) || !strict::has_no_control_characters(*text) ||
        opens_token(*text))
    {
        report(out_failure, reason::k_frame_violation,
            "prepare.initial_prompt.text is not one nonempty UTF-8 line without {{");
        return std::nullopt;
    }
    if (!strict::is_bounded_absolute_path(*file) || !strict::has_no_control_characters(*file) ||
        opens_token(*file))
    {
        report(out_failure, reason::k_frame_violation,
            "prepare.initial_prompt.referenced_file is not a bounded absolute path "
            "without control characters or {{");
        return std::nullopt;
    }
    return Initial_prompt_request{*text, *file};
}

Json_value encode_initial_prompt_acknowledgment(const Initial_prompt_acknowledgment& acknowledgment)
{
    Json_value member = Json_value::object();
    member.set("version",     Json_value(k_interactive_initial_prompt_version));
    member.set("text_sha256", Json_value(acknowledgment.text_sha256_hex));
    return member;
}

std::optional<Initial_prompt_acknowledgment> decode_initial_prompt_acknowledgment(
    const Json_value& value, Contract_failure* out_failure)
{
    const std::string* digest = strict::string_member(value, "text_sha256");
    if (!value.is_object() || strict::unexpected_member(value, {"version", "text_sha256"}).has_value() ||
        !strict::integer_member(value, "version",
            k_interactive_initial_prompt_version, k_interactive_initial_prompt_version) ||
        digest == nullptr || !is_valid_sha256_hex(*digest))
    {
        report(out_failure, reason::k_launch_patch_invalid,
            "initial_prompt echo is not {version: 1, text_sha256: 64 lowercase hex}");
        return std::nullopt;
    }
    return Initial_prompt_acknowledgment{*digest};
}

} // namespace

Json_value encode_codex_invocation_request(const Codex_invocation_request&)
{
    Json_value payload = Json_value::object();
    payload.set("version", Json_value(k_terminal_codex_invocation_version));
    return payload;
}

Json_value encode_codex_invocation(const Codex_invocation& invocation)
{
    if (invocation.argument_prefix_size == 0 ||
        invocation.argument_prefix_size > limits::k_max_argv_entries)
    {
        throw std::invalid_argument("Codex invocation prefix exceeds the argv bound");
    }
    Json_value payload = Json_value::object();
    payload.set("version", Json_value(k_terminal_codex_invocation_version));
    payload.set("argument_prefix_size", Json_value((std::int64_t)invocation.argument_prefix_size));
    return payload;
}

std::optional<Codex_invocation_request> decode_codex_invocation_request(
    const Json_value& payload, Contract_failure* out_failure)
{
    const auto* version = strict::member_of_type(payload, "version", Json_type::INTEGER);
    if (!payload.is_object() || version == nullptr || version->as_integer() < 1) {
        report(out_failure, reason::k_frame_violation, "codex_invocation needs a positive integer version");
        return std::nullopt;
    }
    if (version->as_integer() != k_terminal_codex_invocation_version) {
        report(out_failure, reason::k_unknown_capability, "unsupported terminal-codex-invocation record version");
        return std::nullopt;
    }
    if (strict::unexpected_member(payload, {"version"})) {
        report(out_failure, reason::k_unexpected_member, "codex_invocation request has unexpected members");
        return std::nullopt;
    }
    return Codex_invocation_request{};
}

std::optional<Codex_invocation> decode_codex_invocation(
    const Json_value& payload, Contract_failure* out_failure)
{
    const auto* version = strict::member_of_type(payload, "version", Json_type::INTEGER);
    if (!payload.is_object() || version == nullptr || version->as_integer() < 1) {
        report(out_failure, reason::k_launch_patch_invalid, "codex_invocation needs a positive integer version");
        return std::nullopt;
    }
    if (version->as_integer() != k_terminal_codex_invocation_version) {
        report(out_failure, reason::k_unknown_capability, "unsupported terminal-codex-invocation record version");
        return std::nullopt;
    }
    const auto prefix = strict::integer_member(payload, "argument_prefix_size", 1, limits::k_max_argv_entries);
    if (strict::unexpected_member(payload, {"version", "argument_prefix_size"}) || !prefix) {
        report(out_failure, reason::k_launch_patch_invalid,
            "codex_invocation needs a bounded positive argument_prefix_size");
        return std::nullopt;
    }
    return Codex_invocation{(std::size_t)*prefix};
}

bool validate_terminal_codex_invocation_declaration(const Json_value& payload, Contract_failure* out_failure)
{
    if (!payload.is_object() || !payload.as_object().empty()) {
        return report(out_failure, reason::k_invalid_sidecar, "terminal-codex-invocation@1 sidecar must be empty");
    }
    return true;
}

bool validate_terminal_codex_invocation_dependency(
    const std::vector<Capability_pair>& live_pairs, Contract_failure* out_failure)
{
    const bool producer_live = std::any_of(live_pairs.begin(), live_pairs.end(), [](const Capability_pair& pair) {
        return (pair.capability_id == k_launch_preparation_capability_id &&
                pair.version == k_launch_preparation_execution_version) ||
            (pair.capability_id == k_interactive_resume_capability_id &&
                pair.version == k_interactive_resume_environment_version);
    });
    if (!producer_live) {
        return report(out_failure, reason::k_dependency_capability_unavailable,
            "terminal-codex-invocation@1 needs live launch-preparation@3 or interactive-resume@2");
    }
    return true;
}

Json_value encode_launch_environment(const Launch_environment& environment)
{
    Json_value payload = Json_value::object();
    payload.set("platform", Json_value(environment.platform_id));
    payload.set("effective_environment", environment.effective_environment);
    if (environment.bootstrap) {
        Json_value evidence = Json_value::object();
        evidence.set("version", Json_value(1));
        evidence.set("resolved_home", Json_value(environment.bootstrap->resolved_home));
        payload.set("bootstrap", std::move(evidence));
    }
    return payload;
}

std::optional<Launch_environment> decode_launch_environment(
    const Json_value& payload, Contract_failure* out_failure)
{
    if (!payload.is_object() ||
        strict::unexpected_member(payload, {"platform", "effective_environment", "bootstrap"}))
    {
        report(out_failure, reason::k_frame_violation, "launch_environment has invalid members");
        return std::nullopt;
    }
    const std::string* platform = strict::string_member(payload, "platform");
    const Json_value* ambient = strict::member_of_type(payload, "effective_environment", Json_type::OBJECT);
    std::string detail;
    if (platform == nullptr || !is_valid_platform_id(*platform) || ambient == nullptr ||
        !validate_ambient_environment(*ambient, *platform, &detail))
    {
        report(out_failure, reason::k_frame_violation, "launch_environment is invalid: " + detail);
        return std::nullopt;
    }
    Launch_environment result{*platform, *ambient, std::nullopt};
    if (const auto* evidence = payload.find("bootstrap")) {
        const auto* version = strict::member_of_type(*evidence, "version", Json_type::INTEGER);
        const auto* home = strict::string_member(*evidence, "resolved_home");
        if (!evidence->is_object() || evidence->as_object().size() != 2 ||
            !version || version->as_integer() != 1 || !home || home->empty() ||
            home->size() > limits::k_max_working_directory_bytes ||
            !is_valid_utf8(*home) || home->find('\0') != std::string::npos ||
            !path_from_utf8(*home).is_absolute())
        {
            report(out_failure, reason::k_frame_violation, "unsupported environment-bootstrap evidence");
            return std::nullopt;
        }
        result.bootstrap = Environment_bootstrap_evidence{*home};
    }
    return result;
}

Json_value encode_prepare_request(const Prepare_request& request, std::uint16_t version)
{
    if (version != 3) {
        throw std::invalid_argument("unsupported prepare version");
    }
    Json_value assets = Json_value::array();
    for (const std::string& asset : request.declared_assets) {
        assets.push_back(Json_value(asset));
    }
    Json_value payload = Json_value::object();
    payload.set("launcher",          encode_launcher_request(request.launcher));
    payload.set("working_directory", Json_value(request.working_directory));
    payload.set("title_instruction", Json_value(request.title_instruction));
    payload.set("declared_assets",   std::move(assets));
    if (request.profile_storage.has_value()) {
        payload.set(k_profile_storage_member, profile_storage_member(*request.profile_storage));
    }
    if (request.account_context) {
        payload.set(k_account_context_member, encode_account_context(*request.account_context));
    }
    Json_value argv = Json_value::array();
    for (const auto& argument : request.launcher_argv) argv.push_back(Json_value(argument));
    payload.set("launcher_argv", std::move(argv));
    if (request.launch_environment) {
        payload.set("launch_environment", encode_launch_environment(*request.launch_environment));
    }
    if (request.initial_prompt) {
        payload.set(k_initial_prompt_member, encode_initial_prompt_request(*request.initial_prompt));
    }
    if (request.codex_invocation) {
        payload.set(k_codex_invocation_member, encode_codex_invocation_request(*request.codex_invocation));
    }
    if (request.session_wake) {
        payload.set(k_session_wake_member, encode_session_wake_offer(*request.session_wake));
    }
    if (request.terminal_identity) {
        payload.set(k_terminal_identity_member, encode_terminal_identity_offer(*request.terminal_identity));
    }
    return payload;
}

std::optional<Prepare_request> decode_prepare_request(
    const Json_value& payload, Contract_failure* out_failure, std::uint16_t version)
{
    if (version != 3) {
        report(out_failure, reason::k_frame_violation, "unsupported prepare version");
        return std::nullopt;
    }
    if (!payload.is_object()) {
        report(out_failure, reason::k_frame_violation, "prepare payload is not an object");
        return std::nullopt;
    }
    Json_value members = payload;
    members.erase(k_codex_invocation_member);
    members.erase(k_session_wake_member);
    members.erase(k_terminal_identity_member);
    members.erase("launch_environment");
    members.erase("launcher_argv");
    members.erase(k_initial_prompt_member);
    const std::optional<std::string> unexpected = strict::unexpected_member(members, {
        "launcher", "working_directory", "title_instruction", "declared_assets",
        k_profile_storage_member, k_account_context_member,
    });
    if (unexpected.has_value()) {
        report(out_failure, reason::k_unexpected_member, "prepare." + *unexpected);
        return std::nullopt;
    }
    Prepare_request request;
    const Json_value* environment = payload.find("launch_environment");
    if (environment == nullptr) {
        report(out_failure, reason::k_frame_violation, "prepare.launch_environment is missing");
        return std::nullopt;
    }
    request.launch_environment = decode_launch_environment(*environment, out_failure);
    if (!request.launch_environment) {
        return std::nullopt;
    }
    const auto* argv = strict::member_of_type(payload, "launcher_argv", Json_type::ARRAY);
    if (!argv || argv->as_array().empty() || argv->as_array().size() > limits::k_max_argv_entries) {
        report(out_failure, reason::k_frame_violation, "prepare.launcher_argv is invalid");
        return std::nullopt;
    }
    std::size_t total = 0;
    for (const auto& argument : argv->as_array()) {
        if (!argument.is_string() || argument.as_string().find('\0') != std::string::npos ||
            !is_valid_utf8(argument.as_string()) || argument.as_string().size() > limits::k_max_argv_entry_bytes ||
            (total += argument.as_string().size()) > limits::k_max_argv_total_bytes)
        {
            report(out_failure, reason::k_frame_violation, "prepare.launcher_argv exceeds its grammar or bound");
            return std::nullopt;
        }
        request.launcher_argv.push_back(argument.as_string());
    }
    if (request.launcher_argv.front().empty()) {
        report(out_failure, reason::k_frame_violation, "prepare.launcher_argv has an empty program");
        return std::nullopt;
    }
    const Json_value* launcher = strict::member_of_type(payload, "launcher", Json_type::OBJECT);
    const std::string* working_directory = strict::string_member(payload, "working_directory");
    const std::string* title_instruction = strict::string_member(payload, "title_instruction");
    const Json_value* assets = strict::member_of_type(payload, "declared_assets", Json_type::ARRAY);
    if (launcher == nullptr || working_directory == nullptr || title_instruction == nullptr || assets == nullptr) {
        report(out_failure, reason::k_frame_violation,
            "launcher, working_directory, title_instruction or declared_assets is missing or mistyped");
        return std::nullopt;
    }
    Contract_failure launcher_failure;
    std::optional<Launcher_request> decoded_launcher = decode_launcher_request(*launcher, &launcher_failure);
    if (!decoded_launcher.has_value()) {
        report(out_failure, launcher_failure.reason_code.c_str(), "prepare." + launcher_failure.detail);
        return std::nullopt;
    }
    request.launcher          = std::move(*decoded_launcher);
    request.working_directory = *working_directory;
    request.title_instruction = *title_instruction;
    if (request.working_directory.size() > limits::k_max_working_directory_bytes ||
        request.working_directory.find('\0') != std::string::npos ||
        !is_valid_utf8(request.working_directory) ||
        request.title_instruction.size() > limits::k_max_title_instruction_bytes ||
        request.title_instruction.find('\0') != std::string::npos ||
        !is_valid_utf8(request.title_instruction))
    {
        report(out_failure, reason::k_payload_too_large,
            "working_directory or title_instruction exceeds its byte bound or contains NUL");
        return std::nullopt;
    }
    if (assets->as_array().size() > limits::k_max_managed_assets) {
        report(out_failure, reason::k_payload_too_large, "declared_assets exceeds k_max_managed_assets");
        return std::nullopt;
    }
    std::set<std::string> seen_assets;
    for (const Json_value& asset : assets->as_array()) {
        if (!asset.is_string() || !is_asset_path(asset.as_string()) ||
            !seen_assets.insert(asset.as_string()).second)
        {
            report(out_failure, reason::k_frame_violation, "declared_assets holds a value that is not a canonical path below assets/");
            return std::nullopt;
        }
        request.declared_assets.push_back(asset.as_string());
    }
    if (const Json_value* storage = payload.find(k_profile_storage_member); storage != nullptr) {
        std::string detail;
        request.profile_storage = decode_profile_storage_context(*storage, &detail);
        if (!request.profile_storage.has_value()) {
            report(out_failure, reason::k_frame_violation, detail);
            return std::nullopt;
        }
    }
    if (const Json_value* account = payload.find(k_account_context_member); account != nullptr) {
        request.account_context = decode_account_context(*account, out_failure);
        if (!request.account_context) {
            return std::nullopt;
        }
    }
    if (const Json_value* prompt = payload.find(k_initial_prompt_member); prompt != nullptr) {
        request.initial_prompt = decode_initial_prompt_request(*prompt, out_failure);
        if (!request.initial_prompt) {
            return std::nullopt;
        }
    }
    if (const auto* invocation = payload.find(k_codex_invocation_member)) {
        Contract_failure invocation_failure;
        request.codex_invocation = decode_codex_invocation_request(*invocation, &invocation_failure);
        if (!request.codex_invocation && invocation_failure.reason_code != reason::k_unknown_capability) {
            report(out_failure, invocation_failure.reason_code.c_str(), invocation_failure.detail);
            return std::nullopt;
        }
    }
    if (const auto* wake = payload.find(k_session_wake_member)) {
        request.session_wake = decode_session_wake_offer(*wake, nullptr);
    }
    if (const auto* identity = payload.find(k_terminal_identity_member)) {
        request.terminal_identity = decode_terminal_identity_offer(*identity, out_failure);
        if (!request.terminal_identity) return std::nullopt;
    }
    return request;
}

const char* working_directory_patch_name(Working_directory_patch patch)
{
    switch (patch) {
        case Working_directory_patch::INHERIT:         return "inherit";
        case Working_directory_patch::REPOSITORY_ROOT: return "repository_root";
        default:                                       return "inherit";
    }
}

namespace launch_patch_fields {

std::optional<Launch_patch> decode_fields(const Json_value& payload, Contract_failure* out_failure)
{
    if (!payload.is_object()) {
        return patch_defect(out_failure, "patch is not an object");
    }
    const std::optional<std::string> unexpected =
        strict::unexpected_member(payload, {
            "environment", "working_directory", k_profile_storage_member, k_account_context_member,
            k_codex_invocation_member, k_session_wake_member, k_terminal_identity_member});
    if (unexpected.has_value()) {
        return patch_defect(out_failure, "unexpected member " + *unexpected);
    }
    const Json_value* environment = strict::member_of_type(payload, "environment", Json_type::OBJECT);
    if (environment == nullptr) {
        return patch_defect(out_failure, "environment is missing or not an object");
    }
    const std::optional<std::string> unexpected_environment = strict::unexpected_member(*environment, {"set", "unset"});
    if (unexpected_environment.has_value()) {
        return patch_defect(out_failure, "unexpected member environment." + *unexpected_environment);
    }
    const Json_value* set   = strict::member_of_type(*environment, "set", Json_type::OBJECT);
    const Json_value* unset = strict::member_of_type(*environment, "unset", Json_type::ARRAY);
    if (set == nullptr || unset == nullptr) {
        return patch_defect(out_failure, "environment.set must be an object and environment.unset an array");
    }

    Launch_patch patch;
    if (const auto* invocation = payload.find(k_codex_invocation_member)) {
        Contract_failure invocation_failure;
        patch.codex_invocation = decode_codex_invocation(*invocation, &invocation_failure);
        if (!patch.codex_invocation && invocation_failure.reason_code != reason::k_unknown_capability) {
            return patch_defect(out_failure, invocation_failure.detail);
        }
    }
    for (const Json_member& member : set->as_object()) {
        if (!member.value.is_string()) {
            return patch_defect(out_failure, "environment.set." + member.name + " is not a string");
        }
        patch.set.emplace_back(member.name, member.value.as_string());
    }
    for (const Json_value& name : unset->as_array()) {
        if (!name.is_string()) {
            return patch_defect(out_failure, "environment.unset holds a value that is not a string");
        }
        patch.unset.push_back(name.as_string());
    }

    const std::string* working_directory = strict::string_member(payload, "working_directory");
    if (working_directory == nullptr) {
        return patch_defect(out_failure, "working_directory is missing or not a string");
    }
    if (*working_directory == working_directory_patch_name(Working_directory_patch::INHERIT)) {
        patch.working_directory = Working_directory_patch::INHERIT;
    }
    else
    if (*working_directory == working_directory_patch_name(Working_directory_patch::REPOSITORY_ROOT)) {
        patch.working_directory = Working_directory_patch::REPOSITORY_ROOT;
    }
    else {
        return patch_defect(out_failure, "working_directory is not inherit or repository_root: " + *working_directory);
    }

    if (const Json_value* storage = payload.find(k_profile_storage_member); storage != nullptr) {
        std::string detail;
        patch.profile_storage = decode_profile_storage_context(*storage, &detail);
        if (!patch.profile_storage.has_value()) {
            return patch_defect(out_failure, detail);
        }
    }
    if (const Json_value* account = payload.find(k_account_context_member); account != nullptr) {
        patch.account_context = decode_account_context(*account, out_failure);
        if (!patch.account_context) {
            return std::nullopt;
        }
    }
    if (const auto* wake = payload.find(k_session_wake_member)) {
        patch.session_wake = decode_session_wake_preparation(*wake, nullptr);
    }
    if (const auto* identity = payload.find(k_terminal_identity_member)) {
        patch.terminal_identity = decode_terminal_identity_preparation(*identity, out_failure);
        if (!patch.terminal_identity) return std::nullopt;
    }
    return patch;
}

Json_value encode_fields(const Launch_patch& patch)
{
    Json_value set = Json_value::object();
    for (const auto& [name, value] : patch.set) {
        set.set(name, Json_value(value));
    }
    Json_value unset = Json_value::array();
    for (const std::string& name : patch.unset) {
        unset.push_back(Json_value(name));
    }
    Json_value environment = Json_value::object();
    environment.set("set",   std::move(set));
    environment.set("unset", std::move(unset));

    Json_value payload = Json_value::object();
    payload.set("environment",       std::move(environment));
    payload.set("working_directory", Json_value(working_directory_patch_name(patch.working_directory)));
    if (patch.profile_storage.has_value()) {
        payload.set(k_profile_storage_member, profile_storage_member(*patch.profile_storage));
    }
    if (patch.account_context) {
        payload.set(k_account_context_member, encode_account_context(*patch.account_context));
    }
    if (patch.codex_invocation) {
        payload.set(k_codex_invocation_member, encode_codex_invocation(*patch.codex_invocation));
    }
    if (patch.session_wake) {
        payload.set(k_session_wake_member, encode_session_wake_preparation(*patch.session_wake));
    }
    if (patch.terminal_identity) {
        payload.set(k_terminal_identity_member, encode_terminal_identity_preparation(*patch.terminal_identity));
    }
    return payload;
}

} // namespace launch_patch_fields

std::optional<Launch_patch> decode_launch_patch(const Json_value& payload, Contract_failure* out_failure, std::uint16_t version)
{
    if (version != 3) {
        return patch_defect(out_failure, "unsupported patch version");
    }
    if (!payload.is_object()) {
        return patch_defect(out_failure, "patch is not an object");
    }
    Json_value common = payload;
    for (const char* member : {"argv", "disposition", "apply_managed_configuration", k_initial_prompt_member}) {
        common.erase(member);
    }
    auto patch = launch_patch_fields::decode_fields(common, out_failure);
    if (!patch) {
        return std::nullopt;
    }
    if (const auto* intent = payload.find("apply_managed_configuration")) {
        if (!intent->is_boolean()) return patch_defect(out_failure, "configuration intent must be boolean");
        patch->apply_managed_configuration = intent->as_boolean();
    }
    const auto* disposition = strict::member_of_type(payload, "disposition", Json_type::OBJECT);
    if (!disposition || strict::unexpected_member(*disposition,
            {"version", "prepared", "lifecycle_events", "coordination_delivery"}) ||
        !strict::integer_member(*disposition, "version", 1, 1))
    {
        return patch_defect(out_failure, "launch disposition is missing or invalid");
    }
    const auto* prepared = strict::member_of_type(*disposition, "prepared", Json_type::BOOLEAN);
    const auto* lifecycle = strict::member_of_type(*disposition, "lifecycle_events", Json_type::BOOLEAN);
    const auto* coordination = strict::member_of_type(*disposition, "coordination_delivery", Json_type::BOOLEAN);
    if (!prepared || !lifecycle || !coordination) {
        return patch_defect(out_failure, "launch disposition requires explicit feature admission");
    }
    patch->disposition = Launch_disposition{prepared->as_boolean(), lifecycle->as_boolean(), coordination->as_boolean()};
    if (const auto* argv = payload.find("argv"); argv != nullptr) {
        if (!argv->is_array()) return patch_defect(out_failure, "argv is not an array");
        patch->argv.emplace();
        for (const auto& argument : argv->as_array()) {
            if (!argument.is_string()) return patch_defect(out_failure, "argv contains a non-string");
            patch->argv->push_back(argument.as_string());
        }
    }
    if (const auto* prompt = payload.find(k_initial_prompt_member); prompt != nullptr) {
        patch->initial_prompt = decode_initial_prompt_acknowledgment(*prompt, out_failure);
        if (!patch->initial_prompt) return std::nullopt;
    }
    return patch;
}

Json_value encode_launch_patch(const Launch_patch& patch, std::uint16_t version)
{
    if (version != 3) {
        throw std::invalid_argument("unsupported patch version");
    }
    if (!patch.disposition) {
        throw std::invalid_argument("launch-preparation@3 requires an explicit disposition");
    }
    Json_value payload = launch_patch_fields::encode_fields(patch);
    Json_value disposition = Json_value::object();
    disposition.set("version", Json_value(1));
    disposition.set("prepared", Json_value(patch.disposition->prepared));
    disposition.set("lifecycle_events", Json_value(patch.disposition->lifecycle_events));
    disposition.set("coordination_delivery", Json_value(patch.disposition->coordination_delivery));
    payload.set("disposition", std::move(disposition));
    if (patch.apply_managed_configuration) {
        payload.set("apply_managed_configuration", Json_value(true));
    }
    if (patch.argv) {
        Json_value argv = Json_value::array();
        for (const auto& argument : *patch.argv) argv.push_back(Json_value(argument));
        payload.set("argv", std::move(argv));
    }
    if (patch.initial_prompt) {
        payload.set(k_initial_prompt_member, encode_initial_prompt_acknowledgment(*patch.initial_prompt));
    }
    return payload;
}

std::string initial_prompt_text_digest_hex(std::string_view text)
{
    return to_hex(Sha256::digest(text));
}

bool validate_initial_prompt_acknowledgment(
    const std::optional<Initial_prompt_request>& request,
    const Launch_patch&                          patch,
    Contract_failure*                            out_failure)
{
    if (!request) {
        if (patch.initial_prompt) {
            return report(out_failure, reason::k_launch_patch_invalid,
                "an initial-prompt echo answers a request without an initial prompt");
        }
        return true;
    }
    if (!patch.initial_prompt) {
        return report(out_failure, reason::k_initial_prompt_unacknowledged,
            "the launch patch does not acknowledge the initial prompt");
    }
    if (patch.initial_prompt->text_sha256_hex != initial_prompt_text_digest_hex(request->text)) {
        return report(out_failure, reason::k_initial_prompt_mismatch,
            "the echoed digest does not name the initial prompt the host sent");
    }
    if (!patch.disposition || !patch.disposition->prepared || !patch.argv) {
        return report(out_failure, reason::k_launch_patch_invalid,
            "an acknowledged initial prompt needs a prepared launch with a replacement argv");
    }
    return true;
}

bool validate_codex_invocation_acknowledgment(
    const std::optional<Codex_invocation_request>& request,
    const Launch_patch&                           patch,
    std::span<const std::string>                   actual_argv,
    Contract_failure*                             out_failure)
{
    if (!request) {
        if (patch.codex_invocation) {
            return report(out_failure, reason::k_launch_patch_invalid,
                "a Codex invocation result answers a request without a capability offer");
        }
        return true;
    }
    if (!patch.codex_invocation) {
        return report(out_failure, reason::k_unknown_capability,
            "the launch result does not acknowledge terminal-codex-invocation@1");
    }
    if (patch.disposition && (!patch.disposition->prepared || !patch.argv)) {
        return report(out_failure, reason::k_launch_patch_invalid,
            "a Codex invocation result needs a prepared replacement invocation");
    }
    const std::size_t prefix_size = patch.codex_invocation->argument_prefix_size;
    if (prefix_size == 0 || prefix_size > actual_argv.size() ||
        prefix_size > limits::k_max_argv_entries)
    {
        return report(out_failure, reason::k_launch_patch_invalid,
            "Codex invocation prefix is outside the actual command");
    }
    return true;
}

bool validate_interactive_initial_prompt_dependency(
    const std::vector<Capability_pair>& live_pairs,
    Contract_failure*                   out_failure)
{
    const Capability_pair execution{
        k_launch_preparation_capability_id,
        k_launch_preparation_execution_version,
    };
    if (std::find(live_pairs.begin(), live_pairs.end(), execution) == live_pairs.end()) {
        return report(out_failure, reason::k_dependency_capability_unavailable,
            "interactive-initial-prompt@1 requires launch-preparation@3 live in the same generation");
    }
    return true;
}

std::optional<std::vector<Asset_token>> scan_asset_tokens(std::string_view value, Contract_failure* out_failure)
{
    std::vector<Asset_token> tokens;
    std::size_t search = 0;
    while (true) {
        const std::size_t open = value.find(k_token_open, search);
        if (open == std::string_view::npos) {
            return tokens;
        }
        const std::size_t close = value.find(k_token_close, open + k_token_open.size());
        if (close == std::string_view::npos) {
            report(out_failure, reason::k_launch_patch_invalid, "unterminated {{ at byte " + std::to_string(open));
            return std::nullopt;
        }
        const std::string_view body = value.substr(open + k_token_open.size(), close - open - k_token_open.size());
        Asset_token token;
        token.begin = open;
        token.end   = close + k_token_close.size();
        std::string_view path;
        if (body.substr(0, k_asset_prefix.size()) == k_asset_prefix) {
            path = body.substr(k_asset_prefix.size());
        }
        else
        if (body.substr(0, k_asset_json_prefix.size()) == k_asset_json_prefix) {
            token.json_encoded = true;
            path = body.substr(k_asset_json_prefix.size());
        }
        else {
            report(out_failure, reason::k_launch_patch_invalid,
                "{{ sequence is neither {{asset:...}} nor {{asset_json:...}} at byte " + std::to_string(open));
            return std::nullopt;
        }
        if (!is_asset_path(path) || path.find('{') != std::string_view::npos) {
            report(out_failure, reason::k_launch_patch_invalid,
                "asset token path is not a canonical path below assets/: " + std::string(path));
            return std::nullopt;
        }
        token.canonical_path = std::string(path);
        if (tokens.size() >= limits::k_max_asset_tokens_per_value) {
            report(out_failure, reason::k_launch_patch_invalid, "more than k_max_asset_tokens_per_value asset tokens in one value");
            return std::nullopt;
        }
        tokens.push_back(std::move(token));
        search = close + k_token_close.size();
    }
}

bool validate_launch_patch(
    const Launch_patch&             patch,
    const std::vector<std::string>& declared_assets,
    const std::vector<std::string>& indexed_assets,
    Contract_failure*               out_failure)
{
    if (patch.set.size() > limits::k_max_launch_patch_entries || patch.unset.size() > limits::k_max_launch_patch_entries) {
        return report(out_failure, reason::k_payload_too_large, "environment.set or environment.unset exceeds k_max_launch_patch_entries");
    }
    std::set<std::string> set_names;
    for (const auto& [name, value] : patch.set) {
        if (!is_valid_environment_name(name) || is_reserved_environment_name(name)) {
            return report(out_failure, reason::k_reserved_environment_variable, "environment.set name is reserved or out of grammar: " + name);
        }
        const std::string comparison_name = ascii_lowercase(name);
        if (!set_names.insert(comparison_name).second) {
            return report(out_failure, reason::k_launch_patch_invalid, "environment.set repeats " + name);
        }
        if (value.size() > limits::k_max_environment_value_bytes) {
            return report(out_failure, reason::k_payload_too_large, "environment.set." + name + " exceeds k_max_environment_value_bytes");
        }
        if (value.find('\0') != std::string::npos) {
            return report(out_failure, reason::k_launch_patch_invalid, "environment.set." + name + " contains NUL");
        }
        Contract_failure token_failure;
        const std::optional<std::vector<Asset_token>> tokens = scan_asset_tokens(value, &token_failure);
        if (!tokens.has_value()) {
            return report(out_failure, token_failure.reason_code.c_str(), "environment.set." + name + ": " + token_failure.detail);
        }
        for (const Asset_token& token : *tokens) {
            if (!contains(declared_assets, token.canonical_path)) {
                return report(out_failure, reason::k_asset_undeclared,
                    "environment.set." + name + " references an asset the package does not declare: " + token.canonical_path);
            }
            if (!contains(indexed_assets, token.canonical_path)) {
                return report(out_failure, reason::k_asset_reference_invalid,
                    "environment.set." + name + " references an asset the index lacks: " + token.canonical_path);
            }
        }
    }
    if (patch.disposition && !patch.disposition->prepared &&
        (patch.argv || !patch.set.empty() || !patch.unset.empty() || patch.apply_managed_configuration ||
         patch.disposition->lifecycle_events || patch.disposition->coordination_delivery))
    {
        return report(out_failure, reason::k_launch_patch_invalid, "unmanaged launch must be an unchanged launch");
    }
    if (patch.argv) {
        if (patch.argv->empty() || patch.argv->front().empty() || patch.argv->size() > limits::k_max_argv_entries) {
            return report(out_failure, reason::k_launch_patch_invalid, "replacement argv has no program or exceeds its bound");
        }
        std::size_t total = 0;
        for (const auto& argument : *patch.argv) {
            if (argument.find('\0') != std::string::npos || !is_valid_utf8(argument) ||
                argument.size() > limits::k_max_argv_entry_bytes ||
                (total += argument.size()) > limits::k_max_argv_total_bytes)
            {
                return report(out_failure, reason::k_launch_patch_invalid, "replacement argv exceeds its grammar or bound");
            }
            const auto tokens = scan_asset_tokens(argument, out_failure);
            if (!tokens) return false;
            for (const auto& token : *tokens) {
                if (!contains(declared_assets, token.canonical_path) || !contains(indexed_assets, token.canonical_path)) {
                    return report(out_failure, reason::k_asset_reference_invalid, "replacement argv references an undeclared asset");
                }
            }
        }
    }
    std::set<std::string> unset_names;
    for (const std::string& name : patch.unset) {
        if (!is_valid_environment_name(name) || is_reserved_environment_name(name)) {
            return report(out_failure, reason::k_reserved_environment_variable, "environment.unset name is reserved or out of grammar: " + name);
        }
        const std::string comparison_name = ascii_lowercase(name);
        if (!unset_names.insert(comparison_name).second) {
            return report(out_failure, reason::k_launch_patch_invalid, "environment.unset repeats " + name);
        }
        if (set_names.count(comparison_name) != 0) {
            return report(out_failure, reason::k_launch_patch_invalid, "environment.unset names a variable that environment.set also sets: " + name);
        }
    }
    return true;
}

std::optional<std::string> substitute_asset_tokens(
    std::string_view                                        value,
    const std::vector<Asset_token>&                         tokens,
    const std::vector<std::pair<std::string, std::string>>& materialized_paths)
{
    std::string result;
    const auto append_bounded = [&result](std::string_view bytes) {
        if (bytes.size() > limits::k_max_environment_value_bytes -
                std::min(
                    result.size(),
                    limits::k_max_environment_value_bytes))
        {
            return false;
        }
        result.append(bytes);
        return true;
    };
    std::size_t copied = 0;
    for (const Asset_token& token : tokens) {
        const auto materialized = std::find_if(materialized_paths.begin(), materialized_paths.end(),
            [&token](const std::pair<std::string, std::string>& entry) { return entry.first == token.canonical_path; });
        if (materialized == materialized_paths.end()) {
            return std::nullopt;
        }
        if (!append_bounded(value.substr(copied, token.begin - copied))) {
            return std::nullopt;
        }
        std::string replacement;
        if (token.json_encoded) {
            replacement = serialize_json(Json_value(materialized->second));
        }
        else {
            replacement = materialized->second;
        }
        if (!append_bounded(replacement)) {
            return std::nullopt;
        }
        copied = token.end;
    }
    if (!append_bounded(value.substr(copied))) {
        return std::nullopt;
    }
    return result;
}

} // namespace logonomic::cli_addon
