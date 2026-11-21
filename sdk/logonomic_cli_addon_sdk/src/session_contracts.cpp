#include "logonomic_cli_addon/session_contracts.h"
#include "logonomic_cli_addon/capability_ids.h"
#include "logonomic_cli_addon/probe_contracts.h"
#include "logonomic_cli_addon/current_account.h"
#include "logonomic_cli_addon/limits.h"
#include "logonomic_cli_addon/quota.h"
#include "logonomic_cli_addon/sha256.h"
#include "strict_json.h"
#include "launch_patch_fields_p.h"

#include <algorithm>
#include <cstdint>
#include <set>
#include <stdexcept>

namespace logonomic::cli_addon {

namespace {

using strict::Name_entry;
using strict::name_of;
using strict::value_of;

constexpr std::string_view k_base64_alphabet =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

bool report(Contract_failure* out_failure, const char* reason_code, std::string detail)
{
    if (out_failure != nullptr) {
        out_failure->reason_code = reason_code;
        out_failure->detail      = std::move(detail);
    }
    return false;
}

bool valid_failure_diagnostics(const Structured_failure_diagnostics& diagnostics)
{
    const bool source_valid = diagnostics.source == "native_error" ||
        diagnostics.source == "protocol_error" || diagnostics.source == "process_exit";
    return source_valid && !diagnostics.message.empty() &&
        diagnostics.message.size() <= limits::k_max_structured_failure_message_bytes &&
        diagnostics.message.find('\0') == std::string::npos && is_valid_utf8(diagnostics.message);
}

Json_value encode_chunks(std::string_view bytes)
{
    Json_value chunks = Json_value::array();
    for (std::size_t offset = 0; offset < bytes.size(); offset += limits::k_exchange_chunk_bytes) {
        chunks.push_back(Json_value(encode_base64(bytes.substr(offset, limits::k_exchange_chunk_bytes))));
    }
    return chunks;
}

bool read_chunks(
    const Json_value& payload,
    std::string_view  member,
    std::size_t       limit,
    std::string&      bytes,
    Contract_failure* failure)
{
    const Json_value* chunks = strict::member_of_type(payload, member, Json_type::ARRAY);
    if (chunks == nullptr || chunks->as_array().size() >
            (limit + limits::k_exchange_chunk_bytes - 1) / limits::k_exchange_chunk_bytes)
    {
        return report(failure, reason::k_payload_too_large, std::string(member) + " has invalid chunks");
    }
    bytes.clear();
    std::size_t index = 0;
    for (const Json_value& chunk : chunks->as_array()) {
        const auto decoded = chunk.is_string() ? decode_base64(chunk.as_string()) : std::nullopt;
        if (!decoded || decoded->empty() || decoded->size() > limits::k_exchange_chunk_bytes ||
            decoded->size() > limit - bytes.size() ||
            (++index != chunks->as_array().size() && decoded->size() != limits::k_exchange_chunk_bytes))
        {
            return report(failure, reason::k_payload_too_large, std::string(member) + " exceeds its byte bound");
        }
        bytes += *decoded;
    }
    return true;
}

bool read_exchange_state(const Json_value& payload, std::string& state, Contract_failure* failure)
{
    if (!read_chunks(payload, "protocol_state_base64_chunks",
            limits::k_max_exchange_protocol_state_utf8_bytes, state, failure))
    {
        return false;
    }
    if (!exchange_protocol_state_valid(state)) {
        return report(failure, reason::k_payload_too_large, "protocol state exceeds its UTF-16 bound or is invalid");
    }
    return true;
}

bool report(Contract_failure* out_failure, const Contract_failure& failure, const std::string& prefix)
{
    return report(out_failure, failure.reason_code.c_str(), prefix + failure.detail);
}

constexpr Name_entry<Structured_purpose> k_purpose_names[] = {
    {Structured_purpose::READ_ONLY_REVIEW, "read_only_review"},
    {Structured_purpose::IMPLEMENTATION,   "implementation"},
};

constexpr Name_entry<Presentation_event_kind> k_presentation_kind_names[] = {
    {Presentation_event_kind::REVIEW_INITIALIZED,   "review_initialized"},
    {Presentation_event_kind::REASONING,            "reasoning"},
    {Presentation_event_kind::ANALYSIS_UPDATE,      "analysis_update"},
    {Presentation_event_kind::TOOL_RESPONSE,        "tool_response"},
    {Presentation_event_kind::RATE_LIMIT_STATUS,    "rate_limit_status"},
    {Presentation_event_kind::RESULT_AUTHENTICATED, "result_authenticated"},
};

constexpr std::uint16_t k_presentation_delta_version = 1;

bool has_nul(std::string_view text) { return text.find('\0') != std::string_view::npos; }

bool validate_model_selection_text(
    const std::optional<std::string>& value,
    std::size_t                      maximum,
    std::string_view                 name,
    Contract_failure*               out_failure)
{
    if (!value) {
        return true;
    }
    const std::string field = "model_selection." + std::string(name);
    if (value->empty() || has_nul(*value)) {
        return report(out_failure, reason::k_frame_violation, field + " is empty or contains NUL");
    }
    if (value->size() > maximum) {
        return report(out_failure, reason::k_payload_too_large, field + " exceeds its byte bound");
    }
    if (!is_valid_utf8(*value)) {
        return report(out_failure, reason::k_invalid_utf8, field + " is not UTF-8");
    }
    return true;
}

bool validate_model_selection_values(const Delegated_model_selection& selection, Contract_failure* out_failure)
{
    if (!selection.model && !selection.reasoning_effort) {
        return report(out_failure, reason::k_frame_violation, "model_selection has no requested settings");
    }
    return validate_model_selection_text(
            selection.model, limits::k_max_delegated_model_bytes, "model", out_failure) &&
        validate_model_selection_text(
            selection.reasoning_effort, limits::k_max_delegated_reasoning_effort_bytes,
            "reasoning_effort", out_failure);
}

// The members every prepare request shares.
constexpr std::string_view k_member_launcher          = "launcher";
constexpr std::string_view k_member_launcher_argv     = "launcher_argv";
constexpr std::string_view k_member_working_directory = "working_directory";
constexpr std::string_view k_member_declared_assets   = "declared_assets";
constexpr std::string_view k_member_purpose           = "purpose";
constexpr std::string_view k_member_prompt            = "prompt_base64";
constexpr std::string_view k_member_process           = "process";
constexpr std::string_view k_member_inspection        = "inspection";

bool is_asset_path(std::string_view path)
{
    return is_valid_package_relative_path(path) && path_first_segment(path) == k_segment_assets;
}

// An absolute path in either platform's native form: '/' first, or a drive
// letter with ':' and a separator, or a UNC prefix.
bool is_absolute_native_path(std::string_view path)
{
    if (path.empty() || has_nul(path) || path.size() > limits::k_max_start_absolute_path_bytes) {
        return false;
    }
    if (path.front() == '/') {
        return true;
    }
    if (path.size() >= 2 && path[0] == '\\' && path[1] == '\\') {
        return true;
    }
    const unsigned char drive = (unsigned char)path.front();
    const bool is_letter = (drive >= 'A' && drive <= 'Z') || (drive >= 'a' && drive <= 'z');
    return is_letter && path.size() >= 3 && path[1] == ':' && (path[2] == '\\' || path[2] == '/');
}

bool read_bool(
    const Json_value& object,
    std::string_view  name,
    bool&             out_value,
    Contract_failure* out_failure,
    const char*       context)
{
    const Json_value* value = strict::member_of_type(object, name, Json_type::BOOLEAN);
    if (value == nullptr) {
        return report(out_failure, reason::k_frame_violation,
            std::string(context) + "." + std::string(name) + " is missing or not a boolean");
    }
    out_value = value->as_boolean();
    return true;
}

// A required string member whose bound is a contract byte bound: absent or
// mistyped is `frame_violation`, over the bound or NUL is `payload_too_large`.
bool read_bounded_string(
    const Json_value& object,
    std::string_view  name,
    std::size_t       max_bytes,
    std::string&      out_value,
    Contract_failure* out_failure,
    const char*       context)
{
    const std::string* value = strict::string_member(object, name);
    if (value == nullptr) {
        return report(out_failure, reason::k_frame_violation,
            std::string(context) + "." + std::string(name) + " is missing or not a string");
    }
    if (value->size() > max_bytes || has_nul(*value)) {
        return report(out_failure, reason::k_payload_too_large,
            std::string(context) + "." + std::string(name) + " exceeds its byte bound or contains NUL");
    }
    out_value = *value;
    return true;
}

// A required `*_base64` member decoded to raw bytes under a decoded-size bound.
bool read_base64_member(
    const Json_value& object,
    std::string_view  name,
    std::uint64_t     max_decoded_bytes,
    std::string&      out_bytes,
    Contract_failure* out_failure,
    const char*       context)
{
    const std::string* text = strict::string_member(object, name);
    if (text == nullptr) {
        return report(out_failure, reason::k_frame_violation,
            std::string(context) + "." + std::string(name) + " is missing or not a string");
    }
    std::optional<std::string> bytes = decode_base64(*text);
    if (!bytes.has_value()) {
        return report(out_failure, reason::k_frame_violation,
            std::string(context) + "." + std::string(name) + " is not standard base64");
    }
    if (bytes->size() > max_decoded_bytes) {
        return report(out_failure, reason::k_payload_too_large,
            std::string(context) + "." + std::string(name) + " decodes beyond its byte bound");
    }
    out_bytes = std::move(*bytes);
    return true;
}

bool read_purpose(
    const Json_value&   object,
    Structured_purpose& out_purpose,
    Contract_failure*   out_failure,
    const char*         context)
{
    const std::string* name = strict::string_member(object, k_member_purpose);
    const std::optional<Structured_purpose> purpose = name == nullptr ? std::nullopt : parse_structured_purpose(*name);
    if (!purpose.has_value()) {
        return report(out_failure, reason::k_frame_violation,
            std::string(context) + ".purpose is missing or outside the vocabulary");
    }
    out_purpose = *purpose;
    return true;
}

Json_value encode_string_array(const std::vector<std::string>& values)
{
    Json_value array = Json_value::array();
    for (const std::string& value : values) {
        array.push_back(Json_value(value));
    }
    return array;
}

// The host-resolved argv of a launcher under the argv bounds.
bool read_launcher_argv(
    const Json_value&         object,
    std::vector<std::string>& out_argv,
    Contract_failure*         out_failure,
    const char*               context)
{
    const Json_value* array = strict::member_of_type(object, k_member_launcher_argv, Json_type::ARRAY);
    if (array == nullptr || array->as_array().empty()) {
        return report(out_failure, reason::k_frame_violation,
            std::string(context) + ".launcher_argv is missing, not an array, or empty");
    }
    if (array->as_array().size() > limits::k_max_argv_entries) {
        return report(out_failure, reason::k_payload_too_large,
            std::string(context) + ".launcher_argv exceeds k_max_argv_entries");
    }
    std::size_t total_bytes = 0;
    for (const Json_value& entry : array->as_array()) {
        if (!entry.is_string()) {
            return report(out_failure, reason::k_frame_violation,
                std::string(context) + ".launcher_argv holds a value that is not a string");
        }
        const std::string& argument = entry.as_string();
        if (argument.size() > limits::k_max_argv_entry_bytes || has_nul(argument)) {
            return report(out_failure, reason::k_payload_too_large,
                std::string(context) + ".launcher_argv entry exceeds k_max_argv_entry_bytes or contains NUL");
        }
        total_bytes += argument.size();
        out_argv.push_back(argument);
    }
    if (total_bytes > limits::k_max_argv_total_bytes) {
        return report(out_failure, reason::k_payload_too_large,
            std::string(context) + ".launcher_argv exceeds k_max_argv_total_bytes");
    }
    return true;
}

void encode_launch_context(
    const Session_launch_context& context, Json_value& payload)
{
    if (context.launch_environment) {
        payload.set("launch_environment", encode_launch_environment(*context.launch_environment));
    }
    payload.set(std::string(k_member_launcher),          encode_launcher_request(context.launcher));
    payload.set(std::string(k_member_launcher_argv),     encode_string_array(context.launcher_argv));
    payload.set(std::string(k_member_working_directory), Json_value(context.working_directory));
    payload.set(std::string(k_member_declared_assets),   encode_string_array(context.declared_assets));
    if (context.profile_storage.has_value()) {
        payload.set(k_profile_storage_member, profile_storage_member(*context.profile_storage));
    }
    if (context.account_context) {
        payload.set(k_account_context_member, encode_account_context(*context.account_context));
    }
}

bool decode_launch_context(
    const Json_value&       payload,
    Session_launch_context& out_context,
    Contract_failure*       out_failure,
    const char*             context_name)
{
    const Json_value* environment = payload.find("launch_environment");
    if (environment == nullptr) {
        return report(out_failure, reason::k_frame_violation,
            std::string(context_name) + ".launch_environment is missing");
    }
    out_context.launch_environment = decode_launch_environment(*environment, out_failure);
    if (!out_context.launch_environment) {
        return false;
    }
    const Json_value* launcher = strict::member_of_type(payload, k_member_launcher, Json_type::OBJECT);
    if (launcher == nullptr) {
        return report(out_failure, reason::k_frame_violation,
            std::string(context_name) + ".launcher is missing or not an object");
    }
    Contract_failure launcher_failure;
    std::optional<Launcher_request> decoded_launcher = decode_launcher_request(*launcher, &launcher_failure);
    if (!decoded_launcher.has_value()) {
        return report(out_failure, launcher_failure, std::string(context_name) + ".");
    }
    out_context.launcher = std::move(*decoded_launcher);
    if (!read_launcher_argv(payload, out_context.launcher_argv, out_failure, context_name) ||
        !read_bounded_string(payload, k_member_working_directory, limits::k_max_working_directory_bytes,
            out_context.working_directory, out_failure, context_name))
    {
        return false;
    }
    const Json_value* assets = strict::member_of_type(payload, k_member_declared_assets, Json_type::ARRAY);
    if (assets == nullptr) {
        return report(out_failure, reason::k_frame_violation,
            std::string(context_name) + ".declared_assets is missing or not an array");
    }
    if (assets->as_array().size() > limits::k_max_managed_assets) {
        return report(out_failure, reason::k_payload_too_large,
            std::string(context_name) + ".declared_assets exceeds k_max_managed_assets");
    }
    std::set<std::string> seen_assets;
    for (const Json_value& asset : assets->as_array()) {
        if (!asset.is_string() || !is_asset_path(asset.as_string()) || !seen_assets.insert(asset.as_string()).second) {
            return report(out_failure, reason::k_frame_violation,
                std::string(context_name) + ".declared_assets holds a value that is not a distinct canonical path "
                "below assets/");
        }
        out_context.declared_assets.push_back(asset.as_string());
    }
    if (const Json_value* storage = payload.find(k_profile_storage_member); storage != nullptr) {
        std::string detail;
        out_context.profile_storage = decode_profile_storage_context(*storage, &detail);
        if (!out_context.profile_storage.has_value()) {
            return report(out_failure, reason::k_frame_violation, std::string(context_name) + "." + detail);
        }
    }
    if (const Json_value* account = payload.find(k_account_context_member); account != nullptr) {
        out_context.account_context = decode_account_context(*account, out_failure);
        if (!out_context.account_context) {
            return false;
        }
    }
    return true;
}

bool read_prompt(const Json_value& payload, std::string& out_prompt, Contract_failure* out_failure, const char* context)
{
    return read_base64_member(
        payload, k_member_prompt, limits::k_max_structured_prompt_bytes, out_prompt, out_failure, context);
}

bool read_process(
    const Json_value& payload,
    Process_proposal& out_process,
    Contract_failure* out_failure,
    const char*       context)
{
    const Json_value* process = payload.find(k_member_process);
    if (process == nullptr) {
        return report(out_failure, reason::k_frame_violation, std::string(context) + ".process is missing");
    }
    Contract_failure process_failure;
    std::optional<Process_proposal> decoded = decode_process_proposal(*process, &process_failure);
    if (!decoded.has_value()) {
        return report(out_failure, process_failure, std::string(context) + ".");
    }
    out_process = std::move(*decoded);
    return true;
}

Json_value encode_exchange_setup(const Inspection_setup& setup)
{
    Json_value object = Json_value::object();
    object.set("incremental", Json_value(setup.incremental));
    object.set("keep_stdin_open", Json_value(setup.keep_stdin_open));
    object.set("protocol_state_base64_chunks", encode_chunks(setup.protocol_state));
    return object;
}

std::optional<Inspection_setup> decode_exchange_setup(
    const Json_value& payload, Contract_failure* failure)
{
    const Json_value* setup = strict::member_of_type(payload, "inspection", Json_type::OBJECT);
    if (setup == nullptr || strict::unexpected_member(*setup,
            {"incremental", "keep_stdin_open", "protocol_state_base64_chunks"}))
    {
        report(failure, reason::k_frame_violation, "invalid inspection setup");
        return std::nullopt;
    }
    Inspection_setup decoded;
    if (!read_bool(*setup, "incremental", decoded.incremental, failure, "inspection") ||
        !read_bool(*setup, "keep_stdin_open", decoded.keep_stdin_open, failure, "inspection") ||
        !read_exchange_state(*setup, decoded.protocol_state, failure))
    {
        return std::nullopt;
    }
    if (decoded.keep_stdin_open && !decoded.incremental) {
        report(failure, reason::k_inspection_invalid, "persistent stdin requires incremental inspection");
        return std::nullopt;
    }
    return decoded;
}

bool read_profile_storage_echo(
    const Json_value&                       payload,
    std::optional<Profile_storage_context>& out_storage,
    Contract_failure*                       out_failure,
    const char*                             context)
{
    const Json_value* storage = payload.find(k_profile_storage_member);
    if (storage == nullptr) {
        return true;
    }
    std::string detail;
    out_storage = decode_profile_storage_context(*storage, &detail);
    if (!out_storage.has_value()) {
        return report(out_failure, reason::k_frame_violation, std::string(context) + "." + detail);
    }
    return true;
}

Json_value encode_presentation(const std::vector<Presentation_event>& events)
{
    Json_value records = Json_value::array();
    for (const Presentation_event& event : events) {
        Json_value record = Json_value::object();
        record.set("kind",  Json_value(presentation_event_kind_name(event.kind)));
        record.set("count", Json_value(event.count));
        records.push_back(std::move(record));
    }
    Json_value delta = Json_value::object();
    delta.set("version", Json_value(k_presentation_delta_version));
    delta.set("records", std::move(records));
    return delta;
}

// The strict decode of the optional presentation member; a defect is
// reported through out_error and leaves the inspection standing.
std::optional<std::vector<Presentation_event>> decode_presentation(const Json_value& value, std::string* out_error)
{
    if (!value.is_object()) {
        *out_error = "presentation is not an object";
        return std::nullopt;
    }
    const std::optional<std::string> unexpected = strict::unexpected_member(value, {"version", "records"});
    if (unexpected.has_value()) {
        *out_error = "presentation has unexpected member " + *unexpected;
        return std::nullopt;
    }
    const std::optional<std::int64_t> version =
        strict::integer_member(value, "version", k_presentation_delta_version, k_presentation_delta_version);
    if (!version.has_value()) {
        *out_error = "presentation.version is not 1";
        return std::nullopt;
    }
    const Json_value* records = strict::member_of_type(value, "records", Json_type::ARRAY);
    if (records == nullptr || records->as_array().empty()) {
        *out_error = "presentation.records is missing, not an array, or empty";
        return std::nullopt;
    }
    if (records->as_array().size() > limits::k_max_presentation_records_per_inspection) {
        *out_error = "presentation.records exceeds k_max_presentation_records_per_inspection";
        return std::nullopt;
    }
    std::vector<Presentation_event> events;
    for (const Json_value& record : records->as_array()) {
        if (!record.is_object() || strict::unexpected_member(record, {"kind", "count"}).has_value()) {
            *out_error = "presentation.records holds a record outside the {kind, count} shape";
            return std::nullopt;
        }
        const std::string* kind = strict::string_member(record, "kind");
        const std::optional<Presentation_event_kind> parsed_kind =
            kind == nullptr ? std::nullopt : parse_presentation_event_kind(*kind);
        const std::optional<std::int64_t> count = strict::integer_member(record, "count", 1, INT64_MAX);
        if (!parsed_kind.has_value() || !count.has_value()) {
            *out_error = "presentation.records holds a record whose kind is outside the vocabulary "
                "or whose count is not a positive integer";
            return std::nullopt;
        }
        events.push_back(Presentation_event{*parsed_kind, (std::uint64_t)*count});
    }
    return events;
}

Json_value encode_delegated_control(const Delegated_control& control)
{
    Json_value servers = Json_value::array();
    for (const Delegated_task_server& server : control.servers) {
        Json_value environment = Json_value::object();
        if (!control.connection) {
            for (const auto& [name, value] : server.environment) {
                environment.set(name, Json_value(value));
            }
        }
        Json_value object = Json_value::object();
        object.set("server_id",   Json_value(server.server_id));
        object.set("program",     Json_value(server.program));
        object.set("actions",     encode_string_array(server.actions));
        object.set("environment", std::move(environment));
        servers.push_back(std::move(object));
    }
    Json_value control_object = Json_value::object();
    control_object.set("version",               Json_value(k_delegated_control_version));
    control_object.set("task_id",               Json_value(control.task_id));
    control_object.set("servers",               std::move(servers));
    control_object.set("private_artifact_root", Json_value(control.private_artifact_root));
    if (control.connection) {
        Json_value connection = Json_value::object();
        connection.set("version", Json_value(k_delegated_connection_version));
        connection.set("relay_program", Json_value(control.connection->relay_program));
        connection.set("descriptor_path", Json_value(control.connection->descriptor_path));
        control_object.set("connection", std::move(connection));
    }
    return control_object;
}

bool read_delegated_server(const Json_value& value, Delegated_task_server& out_server, Contract_failure* out_failure)
{
    if (!value.is_object()) {
        return report(out_failure, reason::k_frame_violation, "control.servers holds a value that is not an object");
    }
    const std::optional<std::string> unexpected =
        strict::unexpected_member(value, {"server_id", "program", "actions", "environment"});
    if (unexpected.has_value()) {
        return report(out_failure, reason::k_unexpected_member, "control.servers[]." + *unexpected);
    }
    constexpr const char* context = "control.servers[]";
    if (!read_bounded_string(
            value, "server_id", limits::k_max_delegated_server_id_bytes, out_server.server_id, out_failure, context))
    {
        return false;
    }
    if (out_server.server_id.empty() || !strict::has_no_control_characters(out_server.server_id)) {
        return report(out_failure, reason::k_frame_violation,
            "control.servers[].server_id is empty or contains control characters");
    }
    const std::string* program = strict::string_member(value, "program");
    if (program == nullptr || !is_absolute_native_path(*program)) {
        return report(out_failure, reason::k_frame_violation,
            "control.servers[].program is missing or not a bounded absolute path");
    }
    out_server.program = *program;
    const Json_value* actions = strict::member_of_type(value, "actions", Json_type::ARRAY);
    if (actions == nullptr) {
        return report(out_failure, reason::k_frame_violation, "control.servers[].actions is missing or not an array");
    }
    if (actions->as_array().size() > limits::k_max_delegated_server_actions) {
        return report(out_failure, reason::k_payload_too_large,
            "control.servers[].actions exceeds k_max_delegated_server_actions");
    }
    std::set<std::string> seen_actions;
    for (const Json_value& action : actions->as_array()) {
        if (!action.is_string() || action.as_string().empty() ||
            action.as_string().size() > limits::k_max_delegated_server_id_bytes ||
            !strict::has_no_control_characters(action.as_string()) ||
            !seen_actions.insert(action.as_string()).second)
        {
            return report(out_failure, reason::k_frame_violation,
                "control.servers[].actions holds a value that is not a distinct bounded action name");
        }
        out_server.actions.push_back(action.as_string());
    }
    const Json_value* environment = strict::member_of_type(value, "environment", Json_type::OBJECT);
    if (environment == nullptr) {
        return report(out_failure, reason::k_frame_violation,
            "control.servers[].environment is missing or not an object");
    }
    if (environment->as_object().size() > limits::k_max_capability_environment_entries) {
        return report(out_failure, reason::k_payload_too_large,
            "control.servers[].environment exceeds k_max_capability_environment_entries");
    }
    for (const Json_member& member : environment->as_object()) {
        if (!is_valid_environment_name(member.name) || !member.value.is_string()) {
            return report(out_failure, reason::k_frame_violation,
                "control.servers[].environment holds an entry outside the environment grammar: " + member.name);
        }
        const std::string& value_text = member.value.as_string();
        if (value_text.size() > limits::k_max_environment_value_bytes || has_nul(value_text)) {
            return report(out_failure, reason::k_payload_too_large,
                "control.servers[].environment value out of bounds for " + member.name);
        }
        out_server.environment.emplace_back(member.name, member.value.as_string());
    }
    return true;
}

bool read_delegated_control(const Json_value& payload, Delegated_control& out_control, Contract_failure* out_failure)
{
    const Json_value* control = strict::member_of_type(payload, "control", Json_type::OBJECT);
    if (control == nullptr) {
        return report(out_failure, reason::k_frame_violation, "prepare.control is missing or not an object");
    }
    const std::optional<std::string> unexpected =
        strict::unexpected_member(*control, {"version", "task_id", "servers", "private_artifact_root", "connection"});
    if (unexpected.has_value()) {
        return report(out_failure, reason::k_unexpected_member, "prepare.control." + *unexpected);
    }
    const std::optional<std::int64_t> version =
        strict::integer_member(*control, "version", k_delegated_control_version, k_delegated_control_version);
    if (!version.has_value()) {
        return report(out_failure, reason::k_frame_violation, "prepare.control.version is not 1");
    }
    constexpr const char* context = "prepare.control";
    if (!read_bounded_string(
            *control, "task_id", limits::k_max_delegated_task_id_bytes, out_control.task_id, out_failure, context))
    {
        return false;
    }
    if (out_control.task_id.empty() || !strict::has_no_control_characters(out_control.task_id)) {
        return report(out_failure, reason::k_frame_violation,
            "prepare.control.task_id is empty or contains control characters");
    }
    const Json_value* servers = strict::member_of_type(*control, "servers", Json_type::ARRAY);
    if (servers == nullptr || servers->as_array().empty()) {
        return report(out_failure, reason::k_frame_violation,
            "prepare.control.servers is missing, not an array, or empty");
    }
    if (servers->as_array().size() > limits::k_max_delegated_task_servers) {
        return report(out_failure, reason::k_payload_too_large,
            "prepare.control.servers exceeds k_max_delegated_task_servers");
    }
    std::set<std::string> server_ids;
    for (const Json_value& value : servers->as_array()) {
        Delegated_task_server server;
        if (!read_delegated_server(value, server, out_failure)) {
            return false;
        }
        if (!server_ids.insert(server.server_id).second) {
            return report(out_failure, reason::k_frame_violation,
                "prepare.control.servers repeats server_id " + server.server_id);
        }
        out_control.servers.push_back(std::move(server));
    }
    const std::string* root = strict::string_member(*control, "private_artifact_root");
    if (root == nullptr || !is_absolute_native_path(*root)) {
        return report(out_failure, reason::k_frame_violation,
            "prepare.control.private_artifact_root is missing or not a bounded absolute path");
    }
    out_control.private_artifact_root = *root;
    if (const Json_value* connection = control->find("connection")) {
        if (!connection->is_object()) {
            return report(out_failure, reason::k_frame_violation, "control.connection is not an object");
        }
        if (strict::unexpected_member(*connection, {"version", "relay_program", "descriptor_path"})) {
            return report(out_failure, reason::k_unexpected_member, "control.connection has an unexpected member");
        }
        if (!strict::integer_member(*connection, "version", 1, 1)) {
            return report(out_failure, reason::k_unsupported_version, "control.connection.version is not 1");
        }
        const std::string* relay = strict::string_member(*connection, "relay_program");
        const std::string* descriptor = strict::string_member(*connection, "descriptor_path");
        if (!relay || !descriptor || !is_absolute_native_path(*relay) || !is_absolute_native_path(*descriptor)) {
            return report(out_failure, reason::k_frame_violation, "control.connection paths are not bounded absolute paths");
        }
        for (const Delegated_task_server& server : out_control.servers) {
            if (!server.environment.empty()) {
                return report(out_failure, reason::k_frame_violation, "connection-bearing server environment is not empty");
            }
        }
        out_control.connection = Delegated_connection{*relay, *descriptor};
    }
    return true;
}

Json_value encode_resume_acknowledgment(const Resume_acknowledgment& acknowledgment)
{
    Json_value object = Json_value::object();
    object.set("version",       Json_value(k_resume_acknowledgment_version));
    object.set("record_digest", Json_value(acknowledgment.record_digest_hex));
    return object;
}

std::optional<Json_value> decode_resume_envelope(
    const Json_value& value,
    Contract_failure* out_failure,
    const char*       context)
{
    if (!value.is_object()) {
        report(out_failure, reason::k_frame_violation, std::string(context) + " is not an object");
        return std::nullopt;
    }
    const std::optional<std::string> unexpected = strict::unexpected_member(value, {"version", "record"});
    if (unexpected.has_value()) {
        report(out_failure, reason::k_unexpected_member, std::string(context) + "." + *unexpected);
        return std::nullopt;
    }
    if (strict::integer_member(value, "version", k_resume_record_version, k_resume_record_version) == std::nullopt) {
        report(out_failure, reason::k_frame_violation, std::string(context) + ".version is not 1");
        return std::nullopt;
    }
    const Json_value* record = strict::member_of_type(value, "record", Json_type::OBJECT);
    if (record == nullptr) {
        report(out_failure, reason::k_frame_violation, std::string(context) + ".record is missing or not an object");
        return std::nullopt;
    }
    if (serialize_json_canonical(*record).size() > limits::k_max_resume_record_bytes) {
        report(out_failure, reason::k_payload_too_large,
            std::string(context) + ".record exceeds k_max_resume_record_bytes");
        return std::nullopt;
    }
    return *record;
}


} // namespace

bool exchange_protocol_state_valid(std::string_view state)
{
    if (!is_valid_utf8(state) || state.find('\0') != std::string_view::npos) {
        return false;
    }
    std::size_t units = 0;
    for (unsigned char byte : state) {
        if ((byte & 0xc0) != 0x80) {
            units += byte >= 0xf0 ? 2 : 1;
        }
    }
    return units <= limits::k_max_exchange_protocol_state_code_units;
}

std::optional<Json_value> parse_exchange_protocol_state(std::string_view state)
{
    if (!exchange_protocol_state_valid(state)) {
        return std::nullopt;
    }
    Json_parse_limits parse_limits;
    parse_limits.max_bytes        = limits::k_max_exchange_protocol_state_utf8_bytes;
    parse_limits.max_string_bytes = limits::k_max_exchange_protocol_state_utf8_bytes;
    return parse_json(state, parse_limits, nullptr);
}

bool validate_session_exchange_payload(const Json_value& payload, Contract_failure* out_failure)
{
    return serialize_json_canonical(payload).size() <= limits::k_max_exchange_payload_bytes ||
        report(out_failure, reason::k_payload_too_large, "session exchange exceeds its aggregate frame allowance");
}

// ---------------------------------------------------------------------------
// base64

std::string encode_base64(std::string_view bytes)
{
    std::string text;
    text.reserve((bytes.size() + 2) / 3 * 4);
    std::size_t index = 0;
    while (index + 3 <= bytes.size()) {
        const std::uint32_t triple =
            ((std::uint32_t)(unsigned char)bytes[index] << 16) |
            ((std::uint32_t)(unsigned char)bytes[index + 1] << 8) |
            (std::uint32_t)(unsigned char)bytes[index + 2];
        text.push_back(k_base64_alphabet[(triple >> 18) & 0x3Fu]);
        text.push_back(k_base64_alphabet[(triple >> 12) & 0x3Fu]);
        text.push_back(k_base64_alphabet[(triple >> 6) & 0x3Fu]);
        text.push_back(k_base64_alphabet[triple & 0x3Fu]);
        index += 3;
    }
    const std::size_t remaining = bytes.size() - index;
    if (remaining == 1) {
        const std::uint32_t value = (std::uint32_t)(unsigned char)bytes[index] << 16;
        text.push_back(k_base64_alphabet[(value >> 18) & 0x3Fu]);
        text.push_back(k_base64_alphabet[(value >> 12) & 0x3Fu]);
        text.append("==");
    }
    else
    if (remaining == 2) {
        const std::uint32_t value =
            ((std::uint32_t)(unsigned char)bytes[index] << 16) |
            ((std::uint32_t)(unsigned char)bytes[index + 1] << 8);
        text.push_back(k_base64_alphabet[(value >> 18) & 0x3Fu]);
        text.push_back(k_base64_alphabet[(value >> 12) & 0x3Fu]);
        text.push_back(k_base64_alphabet[(value >> 6) & 0x3Fu]);
        text.push_back('=');
    }
    return text;
}

std::optional<std::string> decode_base64(std::string_view text)
{
    if (text.size() % 4 != 0) {
        return std::nullopt;
    }
    std::size_t padding = 0;
    while (padding < 2 && padding < text.size() && text[text.size() - 1 - padding] == '=') {
        ++padding;
    }
    const std::size_t digits = text.size() - padding;
    std::string bytes;
    bytes.reserve(text.size() / 4 * 3);
    std::uint32_t accumulator = 0;
    int bits = 0;
    for (std::size_t i = 0; i < digits; ++i) {
        const std::size_t position = k_base64_alphabet.find(text[i]);
        if (position == std::string_view::npos) {
            return std::nullopt;
        }
        accumulator = (accumulator << 6) | (std::uint32_t)position;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            bytes.push_back((char)((accumulator >> bits) & 0xFFu));
        }
    }
    // A canonical encoding leaves no set bits behind the last byte.
    if ((accumulator & ((1u << bits) - 1u)) != 0) {
        return std::nullopt;
    }
    return bytes;
}

// ---------------------------------------------------------------------------
// Purposes

const char* structured_purpose_name(Structured_purpose purpose)
{
    return name_of(k_purpose_names, purpose);
}

std::optional<Structured_purpose> parse_structured_purpose(std::string_view name)
{
    return value_of(k_purpose_names, name);
}

std::optional<Purposes_declaration> decode_purposes_declaration(
    const Json_value& payload, Contract_failure* out_failure)
{
    if (!payload.is_object()) {
        report(out_failure, reason::k_invalid_sidecar, "payload is not an object");
        return std::nullopt;
    }
    const std::optional<std::string> unexpected = strict::unexpected_member(payload, {"purposes", "environment_names"});
    if (unexpected.has_value()) {
        report(out_failure, reason::k_invalid_sidecar, "unexpected member " + *unexpected);
        return std::nullopt;
    }
    const Json_value* purposes = strict::member_of_type(payload, "purposes", Json_type::ARRAY);
    if (purposes == nullptr || purposes->as_array().empty() ||
        purposes->as_array().size() > limits::k_max_structured_purposes)
    {
        report(out_failure, reason::k_invalid_sidecar,
            "purposes is missing, not an array, or outside 1-k_max_structured_purposes entries");
        return std::nullopt;
    }
    Purposes_declaration declaration;
    for (const Json_value& value : purposes->as_array()) {
        const std::optional<Structured_purpose> purpose =
            value.is_string() ? parse_structured_purpose(value.as_string()) : std::nullopt;
        if (!purpose.has_value()) {
            report(out_failure, reason::k_invalid_sidecar, "purposes holds a value outside the vocabulary");
            return std::nullopt;
        }
        const bool repeated = std::find(declaration.purposes.begin(), declaration.purposes.end(), *purpose) !=
            declaration.purposes.end();
        if (repeated) {
            report(out_failure, reason::k_invalid_sidecar,
                std::string("purposes repeats ") + structured_purpose_name(*purpose));
            return std::nullopt;
        }
        declaration.purposes.push_back(*purpose);
    }
    Json_value environment = Json_value::object();
    if (const Json_value* names = payload.find("environment_names")) {
        environment.set("environment_names", *names);
    }
    auto input = decode_launcher_discovery_declaration(environment, out_failure);
    if (!input) {
        return std::nullopt;
    }
    declaration.environment_names = std::move(input->environment_names);
    return declaration;
}

Json_value encode_purposes_declaration(const Purposes_declaration& declaration)
{
    Json_value purposes = Json_value::array();
    for (const Structured_purpose purpose : declaration.purposes) {
        purposes.push_back(Json_value(structured_purpose_name(purpose)));
    }
    Json_value payload = Json_value::object();
    payload.set("purposes", std::move(purposes));
    payload.set("environment_names", encode_string_array(declaration.environment_names));
    return payload;
}

bool validate_delegated_session_dependency(
    const Purposes_declaration& delegated,
    const Purposes_declaration* live_structured,
    Contract_failure*           out_failure)
{
    if (live_structured == nullptr) {
        return report(out_failure, reason::k_dependency_capability_unavailable,
            "delegated-session requires its matching structured-session live in the same generation");
    }
    for (const Structured_purpose purpose : delegated.purposes) {
        const bool declared = std::find(live_structured->purposes.begin(), live_structured->purposes.end(), purpose) !=
            live_structured->purposes.end();
        if (!declared) {
            return report(out_failure, reason::k_dependency_capability_unavailable,
                std::string("structured-session does not declare the delegated purpose ") +
                    structured_purpose_name(purpose));
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// structured-session@3

Json_value encode_structured_prepare_request(const Structured_prepare_request& request)
{
    Json_value payload = Json_value::object();
    payload.set(std::string(k_member_purpose), Json_value(structured_purpose_name(request.purpose)));
    encode_launch_context(request.context, payload);
    payload.set(std::string(k_member_prompt), Json_value(encode_base64(request.prompt)));
    return payload;
}

std::optional<Structured_prepare_request> decode_structured_prepare_request(
    const Json_value& payload, Contract_failure* out_failure)
{
    if (!validate_session_exchange_payload(payload, out_failure)) {
        return std::nullopt;
    }
    if (!payload.is_object()) {
        report(out_failure, reason::k_frame_violation, "prepare payload is not an object");
        return std::nullopt;
    }
    const std::optional<std::string> unexpected = strict::unexpected_member(payload, {
        k_member_purpose, k_member_launcher, k_member_launcher_argv, k_member_working_directory,
        k_member_prompt, k_member_declared_assets, k_profile_storage_member, k_account_context_member, "launch_environment",
    });
    if (unexpected.has_value()) {
        report(out_failure, reason::k_unexpected_member, "prepare." + *unexpected);
        return std::nullopt;
    }
    Structured_prepare_request request;
    if (!read_purpose(payload, request.purpose, out_failure, "prepare") ||
        !decode_launch_context(payload, request.context, out_failure, "prepare") ||
        !read_prompt(payload, request.prompt, out_failure, "prepare"))
    {
        return std::nullopt;
    }
    return request;
}

Json_value encode_structured_prepare_result(const Structured_prepare_result& result)
{
    Json_value payload = Json_value::object();
    payload.set(std::string(k_member_process),    encode_process_proposal(result.process));
    payload.set(std::string(k_member_inspection), encode_exchange_setup(result.inspection));
    if (result.profile_storage.has_value()) {
        payload.set(k_profile_storage_member, profile_storage_member(*result.profile_storage));
    }
    if (result.account_context) {
        payload.set(k_account_context_member, encode_account_context(*result.account_context));
    }
    return payload;
}

std::optional<Structured_prepare_result> decode_structured_prepare_result(
    const Json_value& payload, Contract_failure* out_failure)
{
    if (!validate_session_exchange_payload(payload, out_failure)) {
        return std::nullopt;
    }
    if (!payload.is_object()) {
        report(out_failure, reason::k_frame_violation, "prepare result is not an object");
        return std::nullopt;
    }
    const std::optional<std::string> unexpected =
        strict::unexpected_member(payload, {
            k_member_process, k_member_inspection, k_profile_storage_member, k_account_context_member});
    if (unexpected.has_value()) {
        report(out_failure, reason::k_unexpected_member, "prepare." + *unexpected);
        return std::nullopt;
    }
    Structured_prepare_result result;
    if (!read_process(payload, result.process, out_failure, "prepare") ||
        !read_profile_storage_echo(payload, result.profile_storage, out_failure, "prepare"))
    {
        return std::nullopt;
    }
    if (const Json_value* account = payload.find(k_account_context_member); account != nullptr) {
        result.account_context = decode_account_context(*account, out_failure);
        if (!result.account_context) {
            return std::nullopt;
        }
    }
    auto inspection = decode_exchange_setup(payload, out_failure);
    if (!inspection) {
        return std::nullopt;
    }
    if (inspection->keep_stdin_open && result.process.stdin_behavior == Stdin_behavior::INHERIT) {
        report(out_failure, reason::k_inspection_invalid, "persistent stdin cannot inherit input");
        return std::nullopt;
    }
    result.inspection = std::move(*inspection);
    return result;
}

bool validate_structured_process_proposal(const Process_proposal& proposal, Contract_failure* out_failure)
{
    Process_proposal_policy policy;
    policy.launcher_program_allowed = true;
    if (!validate_process_proposal(proposal, policy, out_failure)) {
        return false;
    }
    if (proposal.stdin_behavior == Stdin_behavior::INHERIT) {
        return report(out_failure, reason::k_launch_patch_invalid,
            "a structured process has no interactive stdin to inherit");
    }
    return true;
}

namespace {

Json_value encode_probe_facts(const Structured_inspection& facts)
{
    Json_value inspection = Json_value::object();
    inspection.set("protocol_state_base64_chunks", encode_chunks(facts.protocol_state));
    inspection.set("stdout_base64_chunks", encode_chunks(facts.stdout_bytes));
    inspection.set("stderr_base64_chunks", encode_chunks(facts.stderr_bytes));
    inspection.set("final",          Json_value(facts.final));
    inspection.set("timed_out",      Json_value(facts.timed_out));
    inspection.set("exit_code",      Json_value((std::int64_t)facts.exit_code));
    return inspection;
}

std::optional<Structured_inspection> decode_probe_facts(
    const Json_value& value, Contract_failure* out_failure)
{
    if (!value.is_object()) {
        report(out_failure, reason::k_frame_violation, "inspection is not an object");
        return std::nullopt;
    }
    const Json_value* inspection = &value;
    const auto unexpected_inspection = strict::unexpected_member(*inspection, {
        "protocol_state_base64_chunks", "stdout_base64_chunks", "stderr_base64_chunks",
        "final", "timed_out", "exit_code"});
    if (unexpected_inspection.has_value()) {
        report(out_failure, reason::k_unexpected_member, "inspect.inspection." + *unexpected_inspection);
        return std::nullopt;
    }
    Structured_inspection facts;
    constexpr const char* context = "inspect.inspection";
    const std::optional<std::int64_t> exit_code =
        strict::integer_member(*inspection, "exit_code", INT32_MIN, INT32_MAX);
    if (!exit_code.has_value()) {
        report(out_failure, reason::k_frame_violation,
            "inspect.inspection.exit_code is missing or not a 32-bit integer");
        return std::nullopt;
    }
    facts.exit_code = (std::int32_t)*exit_code;
    const bool streams_valid = read_exchange_state(*inspection, facts.protocol_state, out_failure) &&
        read_chunks(*inspection, "stdout_base64_chunks", limits::k_max_structured_stream_bytes,
            facts.stdout_bytes, out_failure) &&
        read_chunks(*inspection, "stderr_base64_chunks", limits::k_max_structured_stderr_bytes,
            facts.stderr_bytes, out_failure);
    if (!streams_valid || !read_bool(*inspection, "final", facts.final, out_failure, context) ||
        !read_bool(*inspection, "timed_out", facts.timed_out, out_failure, context))
    {
        return std::nullopt;
    }
    return facts;
}

} // namespace

Json_value encode_structured_inspect_request(const Structured_inspect_request& request)
{
    Json_value payload = Json_value::object();
    payload.set(std::string(k_member_purpose), Json_value(structured_purpose_name(request.purpose)));
    payload.set(std::string(k_member_inspection), encode_probe_facts(request.inspection));
    if (request.report_failure_diagnostics) {
        Json_value diagnostics = Json_value::object();
        diagnostics.set("version", Json_value((std::int64_t)k_structured_failure_diagnostics_version));
        payload.set("failure_diagnostics", std::move(diagnostics));
    }
    encode_model_state_request(payload, request.model_state_version);
    return payload;
}

std::optional<Structured_inspect_request> decode_structured_inspect_request(
    const Json_value& value, Contract_failure* out_failure)
{
    Json_value payload = value;
    payload.erase("model_state");
    if (!validate_session_exchange_payload(payload, out_failure)) {
        return std::nullopt;
    }
    if (!payload.is_object()) {
        report(out_failure, reason::k_frame_violation, "inspect payload is not an object");
        return std::nullopt;
    }
    const auto unexpected = strict::unexpected_member(payload, {
        k_member_purpose, k_member_inspection, "failure_diagnostics"});
    if (unexpected) {
        report(out_failure, reason::k_unexpected_member, "inspect." + *unexpected);
        return std::nullopt;
    }
    Structured_inspect_request request;
    if (!read_purpose(payload, request.purpose, out_failure, "inspect")) {
        return std::nullopt;
    }
    const Json_value* inspection = payload.find(k_member_inspection);
    if (inspection == nullptr) {
        report(out_failure, reason::k_frame_violation, "inspect.inspection is missing");
        return std::nullopt;
    }
    auto facts = decode_probe_facts(*inspection, out_failure);
    if (!facts) {
        return std::nullopt;
    }
    request.inspection = std::move(*facts);
    if (const Json_value* diagnostics = payload.find("failure_diagnostics")) {
        request.report_failure_diagnostics = diagnostics->is_object() &&
            !strict::unexpected_member(*diagnostics, {"version"}) &&
            strict::integer_member(*diagnostics, "version",
                k_structured_failure_diagnostics_version, k_structured_failure_diagnostics_version).has_value();
    }
    decode_model_state_request(value, request.model_state_version, request.model_failure);
    return request;
}

Json_value encode_probe_prepare_request(const Probe_prepare_request& request, std::uint16_t version)
{
    if (version != 2) {
        throw std::invalid_argument("unsupported probe version");
    }
    Json_value payload = Json_value::object();
    encode_launch_context(request.context, payload);
    if (request.observe_current_account) {
        payload.set("current_account", encode_current_account_request());
    }
    return payload;
}

std::optional<Probe_prepare_request> decode_probe_prepare_request(
    const Json_value& payload, Contract_failure* out_failure, std::uint16_t version)
{
    if (version != 2) {
        report(out_failure, reason::k_unsupported_version, "unsupported probe version");
        return std::nullopt;
    }
    if (!payload.is_object() || strict::unexpected_member(payload, {
            k_member_launcher, k_member_launcher_argv, k_member_working_directory,
            k_member_declared_assets, k_profile_storage_member, k_account_context_member,
            "launch_environment", "current_account"}))
    {
        report(out_failure, reason::k_frame_violation, "probe prepare payload has invalid members");
        return std::nullopt;
    }
    Probe_prepare_request request;
    if (!decode_launch_context(payload, request.context, out_failure, "prepare")) {
        return std::nullopt;
    }
    if (const auto* current = payload.find("current_account")) {
        request.observe_current_account = decode_current_account_request(*current);
    }
    return request;
}

Json_value encode_probe_inspect_request(const Probe_inspect_request& request, std::uint16_t version)
{
    if (version != 2) {
        throw std::invalid_argument("unsupported probe version");
    }
    Json_value payload = Json_value::object();
    payload.set(std::string(k_member_inspection), encode_probe_facts(request.inspection));
    if (request.account_context) {
        payload.set(k_account_context_member, encode_account_context(*request.account_context));
    }
    if (request.observe_current_account) {
        payload.set("current_account", encode_current_account_request());
    }
    if (request.report_quota_failure) {
        payload.set("quota_failure", encode_quota_failure_request());
    }
    if (request.report_quota_cost) {
        payload.set("quota_cost", encode_quota_cost_request());
    }
    return payload;
}

std::optional<Probe_inspect_request> decode_probe_inspect_request(
    const Json_value& payload, Contract_failure* out_failure, std::uint16_t version)
{
    if (version != 2) {
        report(out_failure, reason::k_unsupported_version, "unsupported probe version");
        return std::nullopt;
    }
    if (!payload.is_object() || strict::unexpected_member(payload, {
            k_member_inspection, k_account_context_member, "current_account", "quota_failure", "quota_cost"}))
    {
        report(out_failure, reason::k_frame_violation, "probe inspect payload has invalid members");
        return std::nullopt;
    }
    const Json_value* inspection = payload.find(k_member_inspection);
    if (inspection == nullptr) {
        report(out_failure, reason::k_frame_violation, "probe inspect.inspection is missing");
        return std::nullopt;
    }
    auto facts = decode_probe_facts(*inspection, out_failure);
    if (!facts) {
        return std::nullopt;
    }
    Probe_inspect_request request;
    request.inspection = std::move(*facts);
    if (const Json_value* account = payload.find(k_account_context_member); account != nullptr) {
        request.account_context = decode_account_context(*account, out_failure);
        if (!request.account_context) {
            return std::nullopt;
        }
    }
    if (const auto* current = payload.find("current_account")) {
        request.observe_current_account = decode_current_account_request(*current);
    }
    if (const auto* failure_request = payload.find("quota_failure")) {
        request.report_quota_failure = decode_quota_failure_request(*failure_request);
    }
    if (const auto* cost_request = payload.find("quota_cost")) {
        request.report_quota_cost = decode_quota_cost_request(*cost_request);
    }
    return request;
}

const char* presentation_event_kind_name(Presentation_event_kind kind)
{
    return name_of(k_presentation_kind_names, kind);
}

std::optional<Presentation_event_kind> parse_presentation_event_kind(std::string_view name)
{
    return value_of(k_presentation_kind_names, name);
}

bool is_valid_failure_reason(std::string_view reason)
{
    return is_valid_reason_code(reason);
}

bool decode_structured_failure_diagnostics_declaration(
    const Json_value& payload, Contract_failure* out_failure)
{
    if (!payload.is_object() || !payload.as_object().empty()) {
        return report(out_failure, reason::k_frame_violation,
            "structured-failure-diagnostics declaration must be an empty object");
    }
    return true;
}

Json_value encode_structured_failure_diagnostics(const Structured_failure_diagnostics& diagnostics)
{
    if (!valid_failure_diagnostics(diagnostics)) {
        throw std::invalid_argument("invalid structured failure diagnostics");
    }
    Json_value value = Json_value::object();
    value.set("version", Json_value((std::int64_t)k_structured_failure_diagnostics_version));
    value.set("source", Json_value(diagnostics.source));
    value.set("message", Json_value(diagnostics.message));
    value.set("truncated", Json_value(diagnostics.truncated));
    return value;
}

std::optional<Structured_failure_diagnostics> decode_structured_failure_diagnostics(const Json_value& value)
{
    if (!value.is_object() || strict::unexpected_member(value, {"version", "source", "message", "truncated"}) ||
        !strict::integer_member(value, "version",
            k_structured_failure_diagnostics_version, k_structured_failure_diagnostics_version))
    {
        return std::nullopt;
    }
    const std::string* source  = strict::string_member(value, "source");
    const std::string* message = strict::string_member(value, "message");
    const Json_value* truncated = strict::member_of_type(value, "truncated", Json_type::BOOLEAN);
    if (!source || !message || !truncated) {
        return std::nullopt;
    }
    Structured_failure_diagnostics diagnostics{*source, *message, truncated->as_boolean()};
    return valid_failure_diagnostics(diagnostics)
        ? std::optional<Structured_failure_diagnostics>(std::move(diagnostics))
        : std::nullopt;
}

Json_value encode_structured_inspect_result(
    const Structured_inspect_result& result, bool report_failure_diagnostics)
{
    if (result.close_stdin && result.complete) {
        throw std::invalid_argument("stdin closure requires an incomplete inspection");
    }
    Json_value payload = Json_value::object();
    payload.set("complete",       Json_value(result.complete));
    if (result.close_stdin) {
        payload.set("close_stdin", Json_value(true));
    }
    payload.set("protocol_state_base64_chunks", encode_chunks(result.protocol_state));
    if (result.stdin_bytes.has_value()) {
        payload.set("stdin_base64_chunks", encode_chunks(*result.stdin_bytes));
    }
    if (result.result_bytes.has_value()) {
        payload.set("result_base64_chunks", encode_chunks(*result.result_bytes));
    }
    if (result.failure_reason.has_value()) {
        payload.set("failure_reason", Json_value(*result.failure_reason));
        if (report_failure_diagnostics && result.complete && result.failure_diagnostics &&
            valid_failure_diagnostics(*result.failure_diagnostics))
        {
            payload.set("failure_diagnostics", encode_structured_failure_diagnostics(*result.failure_diagnostics));
        }
    }
    if (result.presentation_state == Presentation_state::AVAILABLE) {
        payload.set("presentation", encode_presentation(result.presentation));
    }
    encode_structured_model_observation(payload, result.model_observation, result.model_failure);
    if (payload.find("model_state") && !validate_session_exchange_payload(payload, nullptr)) {
        // A full continuation takes precedence over optional sampling.
        payload.erase("model_state");
    }
    return payload;
}

std::optional<Structured_inspect_result> decode_structured_inspect_result(
    const Json_value& value,
    bool              final_inspection,
    Contract_failure* out_failure)
{
    if (!value.is_object()) {
        report(out_failure, reason::k_frame_violation, "inspect result is not an object");
        return std::nullopt;
    }
    // Optional evidence must not make an otherwise valid failure unreadable.
    Json_value payload = value;
    payload.erase("failure_diagnostics");
    payload.erase("model_state");
    if (!validate_session_exchange_payload(payload, out_failure)) {
        return std::nullopt;
    }
    const auto unexpected = strict::unexpected_member(payload, {"complete", "protocol_state_base64_chunks",
        "stdin_base64_chunks", "result_base64_chunks", "failure_reason", "presentation", "close_stdin"});
    if (unexpected.has_value()) {
        report(out_failure, reason::k_unexpected_member, "inspect." + *unexpected);
        return std::nullopt;
    }
    Structured_inspect_result result;
    if (const Json_value* close = payload.find("close_stdin")) {
        if (!close->is_boolean()) {
            report(out_failure, reason::k_frame_violation, "inspect.close_stdin is not Boolean");
            return std::nullopt;
        }
        result.close_stdin = close->as_boolean();
    }
    const bool state_valid = read_exchange_state(payload, result.protocol_state, out_failure);
    if (!state_valid || !read_bool(payload, "complete", result.complete, out_failure, "inspect"))
    {
        return std::nullopt;
    }
    const char* stdin_member  = "stdin_base64_chunks";
    const char* result_member = "result_base64_chunks";
    if (payload.find(stdin_member) != nullptr) {
        std::string bytes;
        const bool valid = read_chunks(
            payload, stdin_member, limits::k_max_structured_stream_bytes, bytes, out_failure);
        if (!valid) {
            return std::nullopt;
        }
        result.stdin_bytes = std::move(bytes);
    }
    if (payload.find(result_member) != nullptr) {
        std::string bytes;
        const bool valid = read_chunks(
            payload, result_member, limits::k_max_structured_stream_bytes, bytes, out_failure);
        if (!valid) {
            return std::nullopt;
        }
        result.result_bytes = std::move(bytes);
    }
    if (const Json_value* failure_reason = payload.find("failure_reason"); failure_reason != nullptr) {
        if (!failure_reason->is_string() || !is_valid_failure_reason(failure_reason->as_string())) {
            report(out_failure, reason::k_frame_violation,
                "inspect.failure_reason is not a string in the reason-code grammar");
            return std::nullopt;
        }
        result.failure_reason = failure_reason->as_string();
    }

    const bool has_terminal = result.result_bytes.has_value() || result.failure_reason.has_value();
    const bool has_both     = result.result_bytes.has_value() && result.failure_reason.has_value();
    if (result.complete && payload.find("close_stdin")) {
        report(out_failure, reason::k_inspection_invalid, "a complete inspection cannot request stdin closure");
        return std::nullopt;
    }
    if (result.complete && !result.protocol_state.empty()) {
        report(out_failure, reason::k_inspection_invalid, "a completed exchange has no continuation state");
        return std::nullopt;
    }
    if (result.complete && (!has_terminal || has_both || result.stdin_bytes.has_value())) {
        report(out_failure, reason::k_inspection_invalid,
            "a complete inspection carries exactly one of result_base64_chunks and failure_reason and no stdin_base64_chunks");
        return std::nullopt;
    }
    if (!result.complete && has_terminal) {
        report(out_failure, reason::k_inspection_invalid,
            "an incomplete inspection carries neither result_base64_chunks nor failure_reason");
        return std::nullopt;
    }
    if (final_inspection && !result.complete) {
        report(out_failure, reason::k_inspection_invalid, "the final inspection must be answered complete");
        return std::nullopt;
    }

    if (result.complete && result.failure_reason) {
        if (const Json_value* diagnostics = value.find("failure_diagnostics")) {
            result.failure_diagnostics = decode_structured_failure_diagnostics(*diagnostics);
        }
    }

    if (const Json_value* presentation = payload.find("presentation"); presentation != nullptr) {
        std::string error;
        std::optional<std::vector<Presentation_event>> events = decode_presentation(*presentation, &error);
        if (events.has_value()) {
            result.presentation_state = Presentation_state::AVAILABLE;
            result.presentation       = std::move(*events);
        }
        else {
            result.presentation_state = Presentation_state::INVALID;
            result.presentation_error = std::move(error);
        }
    }
    decode_structured_model_observation(value, result.model_observation, result.model_failure);
    return result;
}

// ---------------------------------------------------------------------------
// delegated-session@3

std::optional<Delegated_model_selection_declaration> decode_delegated_model_selection_declaration(
    const Json_value& payload, Contract_failure* out_failure)
{
    const Json_value* model = strict::member_of_type(payload, "model", Json_type::BOOLEAN);
    const Json_value* effort = strict::member_of_type(payload, "reasoning_effort", Json_type::BOOLEAN);
    if (!payload.is_object() || strict::unexpected_member(payload, {"model", "reasoning_effort"}) ||
        model == nullptr || effort == nullptr || (!model->as_boolean() && !effort->as_boolean()))
    {
        report(out_failure, reason::k_invalid_sidecar,
            "delegated-model-selection@1 needs model and reasoning_effort booleans with at least one true");
        return std::nullopt;
    }
    return Delegated_model_selection_declaration{model->as_boolean(), effort->as_boolean()};
}

Json_value encode_delegated_model_selection_declaration(const Delegated_model_selection_declaration& declaration)
{
    Json_value payload = Json_value::object();
    payload.set("model",            Json_value(declaration.model));
    payload.set("reasoning_effort", Json_value(declaration.reasoning_effort));
    return payload;
}

std::optional<Delegated_model_selection> decode_delegated_model_selection(
    const Json_value& value, Contract_failure* out_failure)
{
    if (!value.is_object()) {
        report(out_failure, reason::k_frame_violation, "model_selection is not an object");
        return std::nullopt;
    }
    if (const auto unexpected = strict::unexpected_member(value, {"version", "model", "reasoning_effort"})) {
        report(out_failure, reason::k_unexpected_member, "model_selection." + *unexpected);
        return std::nullopt;
    }
    const Json_value* version = strict::member_of_type(value, "version", Json_type::INTEGER);
    if (version == nullptr || version->as_integer() < 1) {
        report(out_failure, reason::k_frame_violation, "model_selection needs a positive integer version");
        return std::nullopt;
    }
    if (version->as_integer() != k_delegated_model_selection_version) {
        report(out_failure, reason::k_unsupported_version, "model_selection version is unavailable");
        return std::nullopt;
    }
    Delegated_model_selection selection;
    for (const auto& [name, destination] : {
            std::pair{"model", &selection.model},
            std::pair{"reasoning_effort", &selection.reasoning_effort}})
    {
        if (const Json_value* setting = value.find(name)) {
            if (!setting->is_string()) {
                report(out_failure, reason::k_frame_violation,
                    "model_selection." + std::string(name) + " is not a string");
                return std::nullopt;
            }
            *destination = setting->as_string();
        }
    }
    if (!validate_model_selection_values(selection, out_failure)) {
        return std::nullopt;
    }
    return selection;
}

Json_value encode_delegated_model_selection(const Delegated_model_selection& selection)
{
    Json_value value = Json_value::object();
    value.set("version", Json_value(k_delegated_model_selection_version));
    if (selection.model) {
        value.set("model", Json_value(*selection.model));
    }
    if (selection.reasoning_effort) {
        value.set("reasoning_effort", Json_value(*selection.reasoning_effort));
    }
    return value;
}

bool validate_delegated_model_selection(
    const Delegated_model_selection&             selection,
    const Delegated_model_selection_declaration* live_declaration,
    Contract_failure*                            out_failure)
{
    if (!validate_model_selection_values(selection, out_failure)) {
        return false;
    }
    if (live_declaration == nullptr || (selection.model && !live_declaration->model)) {
        return report(out_failure, reason::k_model_selection_unavailable,
            "the provider does not support the requested delegated model selection");
    }
    if (selection.reasoning_effort && !live_declaration->reasoning_effort) {
        return report(out_failure, reason::k_reasoning_effort_unavailable,
            "the provider does not support delegated reasoning effort selection");
    }
    return true;
}

bool validate_delegated_model_selection_acknowledgment(
    const std::optional<Delegated_model_selection>& requested,
    const std::optional<Delegated_model_selection>& acknowledged,
    Contract_failure*                              out_failure)
{
    if (requested && !validate_model_selection_values(*requested, out_failure)) {
        return false;
    }
    if (requested != acknowledged) {
        return report(out_failure, reason::k_acknowledgment_mismatch,
            "the delegated model selection acknowledgment does not match the request");
    }
    return true;
}

Json_value encode_delegated_prepare_request(const Delegated_prepare_request& request)
{
    Json_value payload = Json_value::object();
    payload.set(std::string(k_member_purpose), Json_value(structured_purpose_name(request.purpose)));
    encode_launch_context(request.context, payload);
    payload.set(std::string(k_member_prompt), Json_value(encode_base64(request.prompt)));
    payload.set("control", encode_delegated_control(request.control));
    if (request.model_selection) {
        payload.set("model_selection", encode_delegated_model_selection(*request.model_selection));
    }
    return payload;
}

std::optional<Delegated_prepare_request> decode_delegated_prepare_request(
    const Json_value& payload, Contract_failure* out_failure)
{
    if (!validate_session_exchange_payload(payload, out_failure)) {
        return std::nullopt;
    }
    if (!payload.is_object()) {
        report(out_failure, reason::k_frame_violation, "prepare payload is not an object");
        return std::nullopt;
    }
    const std::optional<std::string> unexpected = strict::unexpected_member(payload, {
        k_member_purpose, k_member_launcher, k_member_launcher_argv, k_member_working_directory,
        k_member_prompt, "control", k_member_declared_assets, k_profile_storage_member, k_account_context_member,
        "launch_environment", "model_selection",
    });
    if (unexpected.has_value()) {
        report(out_failure, reason::k_unexpected_member, "prepare." + *unexpected);
        return std::nullopt;
    }
    Delegated_prepare_request request;
    if (!read_purpose(payload, request.purpose, out_failure, "prepare") ||
        !decode_launch_context(payload, request.context, out_failure, "prepare") ||
        !read_prompt(payload, request.prompt, out_failure, "prepare") ||
        !read_delegated_control(payload, request.control, out_failure))
    {
        return std::nullopt;
    }
    if (const Json_value* selection = payload.find("model_selection")) {
        request.model_selection = decode_delegated_model_selection(*selection, out_failure);
        if (!request.model_selection) {
            return std::nullopt;
        }
    }
    return request;
}

bool is_valid_private_artifact_relative_path(std::string_view path)
{
    if (path.empty() || has_nul(path) || path.find('\\') != std::string_view::npos) {
        return false;
    }
    for (const std::string_view segment : split_path_segments(path)) {
        if (segment.empty() || segment == "." || segment == "..") {
            return false;
        }
    }
    return true;
}

Json_value encode_delegated_prepare_result(const Delegated_prepare_result& result)
{
    Json_value acknowledgment = Json_value::object();
    acknowledgment.set("version",    Json_value(k_delegated_acknowledgment_version));
    acknowledgment.set("server_ids", encode_string_array(result.acknowledgment.server_ids));
    if (result.acknowledgment.connection_version) {
        Json_value connection = Json_value::object();
        connection.set("version", Json_value(*result.acknowledgment.connection_version));
        acknowledgment.set("connection", std::move(connection));
    }
    Json_value artifacts = Json_value::array();
    for (const Private_artifact& artifact : result.private_artifacts) {
        Json_value object = Json_value::object();
        object.set("relative_path",  Json_value(artifact.relative_path));
        object.set("content_base64", Json_value(encode_base64(artifact.content)));
        artifacts.push_back(std::move(object));
    }
    Json_value payload = Json_value::object();
    payload.set(std::string(k_member_process),    encode_process_proposal(result.process));
    payload.set("acknowledgment",                 std::move(acknowledgment));
    payload.set("private_artifacts",              std::move(artifacts));
    payload.set(std::string(k_member_inspection), encode_exchange_setup(result.inspection));
    if (result.profile_storage.has_value()) {
        payload.set(k_profile_storage_member, profile_storage_member(*result.profile_storage));
    }
    if (result.account_context) {
        payload.set(k_account_context_member, encode_account_context(*result.account_context));
    }
    if (result.model_selection) {
        payload.set("model_selection", encode_delegated_model_selection(*result.model_selection));
    }
    return payload;
}

std::optional<Delegated_prepare_result> decode_delegated_prepare_result(
    const Json_value& payload, Contract_failure* out_failure)
{
    if (!validate_session_exchange_payload(payload, out_failure)) {
        return std::nullopt;
    }
    if (!payload.is_object()) {
        report(out_failure, reason::k_frame_violation, "prepare result is not an object");
        return std::nullopt;
    }
    const std::optional<std::string> unexpected = strict::unexpected_member(payload, {
        k_member_process, "acknowledgment", "private_artifacts", k_member_inspection,
        k_profile_storage_member, k_account_context_member, "model_selection",
    });
    if (unexpected.has_value()) {
        report(out_failure, reason::k_unexpected_member, "prepare." + *unexpected);
        return std::nullopt;
    }
    Delegated_prepare_result result;
    if (!read_process(payload, result.process, out_failure, "prepare") ||
        !read_profile_storage_echo(payload, result.profile_storage, out_failure, "prepare"))
    {
        return std::nullopt;
    }
    if (const Json_value* selection = payload.find("model_selection")) {
        result.model_selection = decode_delegated_model_selection(*selection, out_failure);
        if (!result.model_selection) {
            return std::nullopt;
        }
    }

    const Json_value* acknowledgment = strict::member_of_type(payload, "acknowledgment", Json_type::OBJECT);
    const bool acknowledgment_shape_valid = acknowledgment != nullptr &&
        !strict::unexpected_member(*acknowledgment, {"version", "server_ids", "connection"}).has_value() &&
        strict::integer_member(*acknowledgment, "version",
            k_delegated_acknowledgment_version, k_delegated_acknowledgment_version).has_value();
    if (!acknowledgment_shape_valid) {
        report(out_failure, reason::k_frame_violation,
            "prepare.acknowledgment is missing or not {version: 1, server_ids}");
        return std::nullopt;
    }
    if (const Json_value* connection = acknowledgment->find("connection")) {
        if (!connection->is_object() || strict::unexpected_member(*connection, {"version"})) {
            report(out_failure, reason::k_frame_violation, "acknowledgment.connection is not a version record");
            return std::nullopt;
        }
        if (!strict::integer_member(*connection, "version", 1, 1)) {
            report(out_failure, reason::k_unsupported_version, "acknowledgment.connection.version is not 1");
            return std::nullopt;
        }
        result.acknowledgment.connection_version = k_delegated_connection_version;
    }
    const Json_value* server_ids = strict::member_of_type(*acknowledgment, "server_ids", Json_type::ARRAY);
    if (server_ids == nullptr) {
        report(out_failure, reason::k_frame_violation, "prepare.acknowledgment.server_ids is missing or not an array");
        return std::nullopt;
    }
    for (const Json_value& server_id : server_ids->as_array()) {
        if (!server_id.is_string()) {
            report(out_failure, reason::k_frame_violation,
                "prepare.acknowledgment.server_ids holds a value that is not a string");
            return std::nullopt;
        }
        result.acknowledgment.server_ids.push_back(server_id.as_string());
    }

    const Json_value* artifacts = strict::member_of_type(payload, "private_artifacts", Json_type::ARRAY);
    if (artifacts == nullptr) {
        report(out_failure, reason::k_frame_violation, "prepare.private_artifacts is missing or not an array");
        return std::nullopt;
    }
    if (artifacts->as_array().size() > limits::k_max_delegated_private_artifacts) {
        report(out_failure, reason::k_private_artifact_invalid,
            "prepare.private_artifacts exceeds k_max_delegated_private_artifacts");
        return std::nullopt;
    }
    std::set<std::string> seen_paths;
    for (const Json_value& value : artifacts->as_array()) {
        if (!value.is_object() || strict::unexpected_member(value, {"relative_path", "content_base64"}).has_value()) {
            report(out_failure, reason::k_frame_violation,
                "prepare.private_artifacts holds a value outside the {relative_path, content_base64} shape");
            return std::nullopt;
        }
        Private_artifact artifact;
        const std::string* relative_path = strict::string_member(value, "relative_path");
        if (relative_path == nullptr) {
            report(out_failure, reason::k_frame_violation,
                "prepare.private_artifacts[].relative_path is missing or not a string");
            return std::nullopt;
        }
        if (!is_valid_private_artifact_relative_path(*relative_path) || !seen_paths.insert(*relative_path).second) {
            report(out_failure, reason::k_private_artifact_invalid,
                "prepare.private_artifacts[].relative_path is not a distinct relative path: " + *relative_path);
            return std::nullopt;
        }
        artifact.relative_path = *relative_path;
        const std::string* content = strict::string_member(value, "content_base64");
        std::optional<std::string> bytes = content == nullptr ? std::nullopt : decode_base64(*content);
        if (!bytes.has_value()) {
            report(out_failure, reason::k_frame_violation,
                "prepare.private_artifacts[].content_base64 is missing or not standard base64");
            return std::nullopt;
        }
        if (bytes->size() > limits::k_max_delegated_private_artifact_bytes) {
            report(out_failure, reason::k_private_artifact_invalid,
                "prepare.private_artifacts[].content_base64 decodes beyond k_max_delegated_private_artifact_bytes");
            return std::nullopt;
        }
        artifact.content = std::move(*bytes);
        result.private_artifacts.push_back(std::move(artifact));
    }
    if (const Json_value* account = payload.find(k_account_context_member); account != nullptr) {
        result.account_context = decode_account_context(*account, out_failure);
        if (!result.account_context) {
            return std::nullopt;
        }
    }
    auto inspection = decode_exchange_setup(payload, out_failure);
    if (!inspection) {
        return std::nullopt;
    }
    if (inspection->keep_stdin_open && result.process.stdin_behavior == Stdin_behavior::INHERIT) {
        report(out_failure, reason::k_inspection_invalid, "persistent stdin cannot inherit input");
        return std::nullopt;
    }
    result.inspection = std::move(*inspection);
    return result;
}

bool validate_delegated_acknowledgment(
    const Delegated_control&        control,
    const Delegated_acknowledgment& acknowledgment,
    Contract_failure*               out_failure)
{
    if (control.connection.has_value() != acknowledgment.connection_version.has_value() ||
        (acknowledgment.connection_version && *acknowledgment.connection_version != k_delegated_connection_version))
    {
        return report(out_failure, reason::k_acknowledgment_mismatch,
            "acknowledgment.connection does not match control.connection");
    }
    std::set<std::string> expected;
    for (const Delegated_task_server& server : control.servers) {
        expected.insert(server.server_id);
    }
    const std::set<std::string> acknowledged(acknowledgment.server_ids.begin(), acknowledgment.server_ids.end());
    if (expected != acknowledged) {
        return report(out_failure, reason::k_acknowledgment_mismatch,
            "acknowledgment.server_ids is not set-equal to control.servers[].server_id");
    }
    return true;
}

// ---------------------------------------------------------------------------
// interactive-resume@2

Json_value encode_resume_record(const Json_value& record)
{
    Json_value envelope = Json_value::object();
    envelope.set("version", Json_value(k_resume_record_version));
    envelope.set("record",  record);
    return envelope;
}

std::optional<Json_value> decode_resume_record(const Json_value& value, Contract_failure* out_failure)
{
    return decode_resume_envelope(value, out_failure, "resume");
}

Json_value encode_resume_inspect_request(const Resume_inspect_request& request)
{
    if (!request.context.launch_environment || !request.context.declared_assets.empty() ||
        request.context.codex_invocation || request.context.session_wake || request.context.terminal_identity)
    {
        throw std::invalid_argument("inspect_resume requires a read-only resolved launch context");
    }
    Json_value payload = Json_value::object();
    encode_launch_context(request.context, payload);
    payload.set("resume", encode_resume_record(request.resume_record));
    return payload;
}

std::optional<Resume_inspect_request> decode_resume_inspect_request(
    const Json_value& payload, Contract_failure* out_failure)
{
    if (!payload.is_object()) {
        report(out_failure, reason::k_frame_violation, "inspect_resume payload is not an object");
        return std::nullopt;
    }
    const auto unexpected = strict::unexpected_member(payload, {
        k_member_launcher, k_member_launcher_argv, k_member_working_directory,
        k_member_declared_assets, k_profile_storage_member, k_account_context_member,
        "resume", "launch_environment",
    });
    if (unexpected) {
        report(out_failure, reason::k_unexpected_member, "inspect_resume." + *unexpected);
        return std::nullopt;
    }
    Resume_inspect_request request;
    if (!decode_launch_context(payload, request.context, out_failure, "inspect_resume")) {
        return std::nullopt;
    }
    if (!request.context.declared_assets.empty()) {
        report(out_failure, reason::k_frame_violation, "inspect_resume cannot prepare managed assets");
        return std::nullopt;
    }
    const auto* resume = payload.find("resume");
    if (!resume) {
        report(out_failure, reason::k_frame_violation, "inspect_resume.resume is missing");
        return std::nullopt;
    }
    auto record = decode_resume_record(*resume, out_failure);
    if (!record) {
        return std::nullopt;
    }
    request.resume_record = std::move(*record);
    return request;
}

Json_value encode_resume_inspect_result(const Resume_inspect_result& result, std::uint16_t version)
{
    if (version != k_interactive_resume_inspection_version && version != k_interactive_resume_content_version) {
        throw std::invalid_argument("unsupported resume inspection version");
    }
    const char* availability = "unknown";
    switch (result.availability) {
        case Resume_availability::AVAILABLE: availability = "available"; break;
        case Resume_availability::ABSENT:    availability = "absent";    break;
        case Resume_availability::UNKNOWN:                             break;
        default: throw std::invalid_argument("invalid resume availability");
    }
    Json_value payload = Json_value::object();
    payload.set("availability", Json_value(availability));
    payload.set("request_digest", Json_value(result.request_digest_hex));
    if (version == k_interactive_resume_content_version) {
        const char* content = "unknown";
        switch (result.content) {
            case Resume_content::EMPTY:    content = "empty";    break;
            case Resume_content::NONEMPTY: content = "nonempty"; break;
            case Resume_content::UNKNOWN:                       break;
            default: throw std::invalid_argument("invalid resume content");
        }
        payload.set("content", Json_value(content));
    }
    return payload;
}

std::optional<Resume_inspect_result> decode_resume_inspect_result(
    const Json_value& payload, Contract_failure* out_failure, std::uint16_t version)
{
    if (version != k_interactive_resume_inspection_version && version != k_interactive_resume_content_version) {
        report(out_failure, reason::k_frame_violation, "unsupported resume inspection version");
        return std::nullopt;
    }
    if (!payload.is_object() || (version == k_interactive_resume_content_version
            ? strict::unexpected_member(payload, {"availability", "request_digest", "content"})
            : strict::unexpected_member(payload, {"availability", "request_digest"})))
    {
        report(out_failure, reason::k_frame_violation, "invalid inspect_resume result");
        return std::nullopt;
    }
    const auto* availability = strict::string_member(payload, "availability");
    const auto* digest       = strict::string_member(payload, "request_digest");
    if (!availability || !digest || !is_valid_sha256_hex(*digest)) {
        report(out_failure, reason::k_frame_violation, "inspect_resume result has invalid availability or digest");
        return std::nullopt;
    }
    Resume_inspect_result result;
    if (*availability == "available") {
        result.availability = Resume_availability::AVAILABLE;
    }
    else
    if (*availability == "absent") {
        result.availability = Resume_availability::ABSENT;
    }
    else
    if (*availability != "unknown") {
        report(out_failure, reason::k_frame_violation, "inspect_resume availability is outside the vocabulary");
        return std::nullopt;
    }
    result.request_digest_hex = *digest;
    if (version == k_interactive_resume_content_version) {
        if (const auto* content = payload.find("content")) {
            if (!content->is_string()) {
                report(out_failure, reason::k_frame_violation, "inspect_resume content is not a string");
                return std::nullopt;
            }
            if (content->as_string() == "empty") {
                result.content = Resume_content::EMPTY;
            }
            else
            if (content->as_string() == "nonempty") {
                result.content = Resume_content::NONEMPTY;
            }
        }
    }
    return result;
}

std::string resume_inspect_request_digest_hex(const Resume_inspect_request& request)
{
    return to_hex(Sha256::digest(serialize_json_canonical(encode_resume_inspect_request(request))));
}

bool validate_resume_inspection(
    const Resume_inspect_request& request,
    const Resume_inspect_result&  result,
    Contract_failure*             out_failure)
{
    if (result.request_digest_hex != resume_inspect_request_digest_hex(request)) {
        return report(out_failure, reason::k_acknowledgment_mismatch,
            "inspect_resume result does not name the exact request");
    }
    return true;
}

Json_value encode_resume_prepare_request(const Resume_prepare_request& request, std::uint16_t version)
{
    if (version != 2) {
        throw std::invalid_argument("unsupported resume version");
    }
    Json_value payload = Json_value::object();
    encode_launch_context(request.context, payload);
    if (request.context.codex_invocation) {
        payload.set(k_codex_invocation_member, encode_codex_invocation_request(*request.context.codex_invocation));
    }
    if (request.context.session_wake) {
        payload.set(k_session_wake_member, encode_session_wake_offer(*request.context.session_wake));
    }
    if (request.context.terminal_identity) {
        payload.set(k_terminal_identity_member, encode_terminal_identity_offer(*request.context.terminal_identity));
    }
    payload.set("title_instruction", Json_value(request.title_instruction));
    payload.set("resume",            encode_resume_record(request.resume_record));
    if (request.startup_observer) {
        payload.set("startup_observer", encode_startup_observer_offer(*request.startup_observer));
    }
    return payload;
}

std::optional<Resume_prepare_request> decode_resume_prepare_request(
    const Json_value& payload, Contract_failure* out_failure, std::uint16_t version)
{
    if (version != 2) {
        report(out_failure, reason::k_unsupported_version, "unsupported resume version");
        return std::nullopt;
    }
    if (!payload.is_object()) {
        report(out_failure, reason::k_frame_violation, "prepare_resume payload is not an object");
        return std::nullopt;
    }
    const std::optional<std::string> unexpected = strict::unexpected_member(payload, {
        k_member_launcher, k_member_launcher_argv, k_member_working_directory, "title_instruction",
        k_member_declared_assets, k_profile_storage_member, k_account_context_member, "resume",
        "launch_environment", "startup_observer", k_codex_invocation_member, k_session_wake_member,
        k_terminal_identity_member,
    });
    if (unexpected.has_value()) {
        report(out_failure, reason::k_unexpected_member, "prepare_resume." + *unexpected);
        return std::nullopt;
    }
    Resume_prepare_request request;
    if (const auto* wake = payload.find(k_session_wake_member)) {
        request.context.session_wake = decode_session_wake_offer(*wake, nullptr);
    }
    if (const auto* identity = payload.find(k_terminal_identity_member)) {
        request.context.terminal_identity = decode_terminal_identity_offer(*identity, out_failure);
        if (!request.context.terminal_identity) return std::nullopt;
    }
    if (const auto* invocation = payload.find(k_codex_invocation_member)) {
        Contract_failure invocation_failure;
        request.context.codex_invocation = decode_codex_invocation_request(*invocation, &invocation_failure);
        if (!request.context.codex_invocation && invocation_failure.reason_code != reason::k_unknown_capability) {
            report(out_failure, invocation_failure, "prepare_resume.");
            return std::nullopt;
        }
    }
    if (!decode_launch_context(payload, request.context, out_failure, "prepare_resume") ||
        !read_bounded_string(payload, "title_instruction", limits::k_max_title_instruction_bytes,
            request.title_instruction, out_failure, "prepare_resume"))
    {
        return std::nullopt;
    }
    const Json_value* resume = payload.find("resume");
    if (resume == nullptr) {
        report(out_failure, reason::k_frame_violation, "prepare_resume.resume is missing");
        return std::nullopt;
    }
    std::optional<Json_value> record = decode_resume_envelope(*resume, out_failure, "prepare_resume.resume");
    if (!record.has_value()) {
        return std::nullopt;
    }
    request.resume_record = std::move(*record);
    if (const auto* observer = payload.find("startup_observer"); observer != nullptr) {
        request.startup_observer = decode_startup_observer_offer(*observer, out_failure);
        if (!request.startup_observer) return std::nullopt;
    }
    return request;
}

Json_value encode_resume_prepare_result(const Resume_prepare_result& result, std::uint16_t version)
{
    if (version != 2) {
        throw std::invalid_argument("unsupported resume version");
    }
    Json_value patch = launch_patch_fields::encode_fields(result.patch);
    Json_value payload = Json_value::object();
    payload.set(std::string(k_member_process), encode_process_proposal(result.process));
    payload.set("environment",                 *patch.find("environment"));
    payload.set("acknowledgment",              encode_resume_acknowledgment(result.acknowledgment));
    if (result.startup_observer) {
        payload.set("startup_observer", encode_startup_observer_offer(*result.startup_observer));
    }
    if (result.patch.profile_storage.has_value()) {
        payload.set(k_profile_storage_member, profile_storage_member(*result.patch.profile_storage));
    }
    if (result.patch.account_context) {
        payload.set(k_account_context_member, encode_account_context(*result.patch.account_context));
    }
    if (result.patch.codex_invocation) {
        payload.set(k_codex_invocation_member, *patch.find(k_codex_invocation_member));
    }
    if (result.patch.session_wake) {
        payload.set(k_session_wake_member, *patch.find(k_session_wake_member));
    }
    if (result.patch.terminal_identity) {
        payload.set(k_terminal_identity_member, *patch.find(k_terminal_identity_member));
    }
    return payload;
}

std::optional<Resume_prepare_result> decode_resume_prepare_result(
    const Json_value& payload, Contract_failure* out_failure, std::uint16_t version)
{
    if (version != 2) {
        report(out_failure, reason::k_unsupported_version, "unsupported resume version");
        return std::nullopt;
    }
    if (!payload.is_object()) {
        report(out_failure, reason::k_frame_violation, "prepare_resume result is not an object");
        return std::nullopt;
    }
    const std::optional<std::string> unexpected = strict::unexpected_member(payload, {
        k_member_process, "environment", "acknowledgment", k_profile_storage_member, k_account_context_member,
        "startup_observer", k_codex_invocation_member, k_session_wake_member, k_terminal_identity_member,
    });
    if (unexpected.has_value()) {
        report(out_failure, reason::k_unexpected_member, "prepare_resume." + *unexpected);
        return std::nullopt;
    }
    Resume_prepare_result result;
    if (const auto* observer = payload.find("startup_observer"); observer != nullptr) {
        result.startup_observer = decode_startup_observer_offer(*observer, out_failure);
        if (!result.startup_observer) return std::nullopt;
    }
    if (!read_process(payload, result.process, out_failure, "prepare_resume")) {
        return std::nullopt;
    }

    // Current launch and resume share the same environment and optional fields.
    // Resume owns its process and acknowledgment instead of a launch disposition.
    const Json_value* environment = payload.find("environment");
    if (environment == nullptr) {
        report(out_failure, reason::k_launch_patch_invalid, "prepare_resume.environment is missing");
        return std::nullopt;
    }
    Json_value patch_object = Json_value::object();
    patch_object.set("environment",       *environment);
    patch_object.set("working_directory", Json_value(working_directory_patch_name(Working_directory_patch::INHERIT)));
    if (const Json_value* storage = payload.find(k_profile_storage_member); storage != nullptr) {
        patch_object.set(k_profile_storage_member, *storage);
    }
    if (const Json_value* account = payload.find(k_account_context_member); account != nullptr) {
        patch_object.set(k_account_context_member, *account);
    }
    if (const auto* invocation = payload.find(k_codex_invocation_member)) {
        patch_object.set(k_codex_invocation_member, *invocation);
    }
    if (const auto* wake = payload.find(k_session_wake_member)) {
        patch_object.set(k_session_wake_member, *wake);
    }
    if (const auto* identity = payload.find(k_terminal_identity_member)) {
        patch_object.set(k_terminal_identity_member, *identity);
    }
    Contract_failure patch_failure;
    std::optional<Launch_patch> patch = launch_patch_fields::decode_fields(patch_object, &patch_failure);
    if (!patch.has_value()) {
        report(out_failure, patch_failure, "prepare_resume.");
        return std::nullopt;
    }
    result.patch = std::move(*patch);

    const Json_value* acknowledgment = strict::member_of_type(payload, "acknowledgment", Json_type::OBJECT);
    const bool acknowledgment_shape_valid = acknowledgment != nullptr &&
        !strict::unexpected_member(*acknowledgment, {"version", "record_digest"}).has_value() &&
        strict::integer_member(*acknowledgment, "version",
            k_resume_acknowledgment_version, k_resume_acknowledgment_version).has_value();
    if (!acknowledgment_shape_valid) {
        report(out_failure, reason::k_frame_violation,
            "prepare_resume.acknowledgment is missing or not {version: 1, record_digest}");
        return std::nullopt;
    }
    const std::string* digest = strict::string_member(*acknowledgment, "record_digest");
    if (digest == nullptr || !is_valid_sha256_hex(*digest)) {
        report(out_failure, reason::k_frame_violation,
            "prepare_resume.acknowledgment.record_digest is not 64 lowercase hex characters");
        return std::nullopt;
    }
    result.acknowledgment.record_digest_hex = *digest;
    return result;
}

std::string resume_record_digest_hex(const Json_value& record)
{
    return to_hex(Sha256::digest(serialize_json_canonical(record)));
}

bool validate_resume_acknowledgment(
    const Json_value&            record,
    const Resume_acknowledgment& acknowledgment,
    Contract_failure*            out_failure)
{
    if (acknowledgment.record_digest_hex != resume_record_digest_hex(record)) {
        return report(out_failure, reason::k_acknowledgment_mismatch,
            "acknowledgment.record_digest does not name the record the host sent");
    }
    return true;
}

bool validate_resume_process_proposal(const Process_proposal& proposal, Contract_failure* out_failure)
{
    Process_proposal_policy policy;
    policy.launcher_program_allowed = true;
    if (!validate_process_proposal(proposal, policy, out_failure)) {
        return false;
    }
    if (proposal.working_directory != Working_directory_intent::SESSION_WORKSPACE) {
        return report(out_failure, reason::k_launch_patch_invalid, "a resume runs in the session workspace");
    }
    if (proposal.stdin_behavior != Stdin_behavior::INHERIT) {
        return report(out_failure, reason::k_launch_patch_invalid, "a resume inherits the terminal's stdin");
    }
    if (!proposal.environment_names.empty() || !proposal.environment.empty() || !proposal.assets.empty()) {
        return report(out_failure, reason::k_launch_patch_invalid,
            "a resume proposal carries its environment and asset references in the launch patch");
    }
    return true;
}

} // namespace logonomic::cli_addon
