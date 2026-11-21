#include "logonomic_cli_addon/process_proposal.h"
#include "logonomic_cli_addon/bootstrap.h"
#include "logonomic_cli_addon/limits.h"
#include "strict_json.h"

#include <set>
#include <string_view>

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

using strict::Name_entry;
using strict::name_of;
using strict::value_of;

constexpr Name_entry<Working_directory_intent> k_working_directory_names[] = {
    {Working_directory_intent::SESSION_WORKSPACE,    "session_workspace"},
    {Working_directory_intent::PACKAGE_ROOT,         "package_root"},
    {Working_directory_intent::SCRATCH_DIRECTORY,    "scratch_directory"},
    {Working_directory_intent::PROFILE_STORAGE_ROOT, "profile_storage_root"},
};

constexpr Name_entry<Stdin_behavior> k_stdin_names[] = {
    {Stdin_behavior::CLOSED,  "closed"},
    {Stdin_behavior::INHERIT, "inherit"},
    {Stdin_behavior::PAYLOAD, "payload"},
};

constexpr Name_entry<Start_intent> k_start_names[] = {
    {Start_intent::ATTACHED,    "attached"},
    {Start_intent::DETACHED,    "detached"},
    {Start_intent::NEW_SESSION, "new_session"},
    {Start_intent::BREAKAWAY,   "breakaway"},
};

constexpr Name_entry<Cleanup_intent> k_cleanup_names[] = {
    {Cleanup_intent::REMOVE_SCRATCH, "remove_scratch"},
    {Cleanup_intent::RETAIN_SCRATCH, "retain_scratch"},
};

template <typename Enum, std::size_t N>
bool read_enum_member(
    const Json_value&           object,
    std::string_view            member,
    const Name_entry<Enum>    (&table)[N],
    Enum&                       out_value,
    Contract_failure*           out_failure)
{
    const Json_value* value = object.find(member);
    if (value == nullptr) {
        return true;   // keep the default
    }
    if (!value->is_string()) {
        return report(out_failure, reason::k_frame_violation, std::string(member) + " must be a string");
    }
    const std::optional<Enum> parsed = value_of(table, value->as_string());
    if (!parsed.has_value()) {
        return report(out_failure, reason::k_frame_violation, std::string(member) + " is not in the vocabulary: " + value->as_string());
    }
    out_value = *parsed;
    return true;
}

bool read_string_array(
    const Json_value&         object,
    std::string_view          member,
    std::vector<std::string>& out_values,
    Contract_failure*         out_failure)
{
    const Json_value* value = object.find(member);
    if (value == nullptr) {
        return true;
    }
    if (!value->is_array()) {
        return report(out_failure, reason::k_frame_violation, std::string(member) + " must be an array");
    }
    for (const Json_value& item : value->as_array()) {
        if (!item.is_string()) {
            return report(out_failure, reason::k_frame_violation, std::string(member) + " must contain strings only");
        }
        out_values.push_back(item.as_string());
    }
    return true;
}

constexpr std::string_view k_launcher_program_member = "launcher_program";

Json_value encode_executable(const Process_executable& executable)
{
    if (executable.kind == Process_executable::Kind::LAUNCHER_PROGRAM) {
        Json_value reference = Json_value::object();
        reference.set(std::string(k_launcher_program_member), Json_value(true));
        return reference;
    }
    return Json_value(executable.path);
}

} // namespace

const char* working_directory_intent_name(Working_directory_intent intent) { return name_of(k_working_directory_names, intent); }
const char* stdin_behavior_name(Stdin_behavior behavior)                   { return name_of(k_stdin_names, behavior); }
const char* start_intent_name(Start_intent intent)                         { return name_of(k_start_names, intent); }
const char* cleanup_intent_name(Cleanup_intent intent)                     { return name_of(k_cleanup_names, intent); }

std::optional<Process_proposal> decode_process_proposal(const Json_value& value, Contract_failure* out_failure)
{
    if (!value.is_object()) {
        report(out_failure, reason::k_frame_violation, "process proposal is not an object");
        return std::nullopt;
    }
    const std::optional<std::string> unexpected = strict::unexpected_member(value, {
        "executable", "argv", "working_directory", "stdin", "stdin_payload", "start",
        "environment_names", "environment", "assets", "cleanup",
    });
    if (unexpected.has_value()) {
        report(out_failure, reason::k_unexpected_member, "process_proposal." + *unexpected);
        return std::nullopt;
    }

    Process_proposal proposal;
    const Json_value* executable = value.find("executable");
    if (executable == nullptr) {
        report(out_failure, reason::k_frame_violation, "executable is missing");
        return std::nullopt;
    }
    if (executable->is_string()) {
        proposal.executable = executable->as_string();
    }
    else
    if (executable->is_object() && executable->as_object().size() == 1 &&
        executable->as_object().front().name == k_launcher_program_member &&
        executable->as_object().front().value == Json_value(true))
    {
        proposal.executable = Process_executable::launcher_program();
    }
    else {
        report(out_failure, reason::k_frame_violation,
            "executable is neither a package path nor {\"launcher_program\": true}");
        return std::nullopt;
    }

    if (!read_string_array(value, "argv", proposal.argv, out_failure) ||
        !read_enum_member(value, "working_directory", k_working_directory_names, proposal.working_directory, out_failure) ||
        !read_enum_member(value, "stdin", k_stdin_names, proposal.stdin_behavior, out_failure) ||
        !read_enum_member(value, "start", k_start_names, proposal.start, out_failure) ||
        !read_string_array(value, "environment_names", proposal.environment_names, out_failure) ||
        !read_string_array(value, "assets", proposal.assets, out_failure) ||
        !read_enum_member(value, "cleanup", k_cleanup_names, proposal.cleanup, out_failure))
    {
        return std::nullopt;
    }

    if (const Json_value* payload = value.find("stdin_payload"); payload != nullptr) {
        if (!payload->is_string()) {
            report(out_failure, reason::k_frame_violation, "stdin_payload must be a string");
            return std::nullopt;
        }
        proposal.stdin_payload = payload->as_string();
    }
    if (const Json_value* environment = value.find("environment"); environment != nullptr) {
        if (!environment->is_object()) {
            report(out_failure, reason::k_frame_violation, "environment must be an object");
            return std::nullopt;
        }
        for (const Json_member& member : environment->as_object()) {
            if (!member.value.is_string()) {
                report(out_failure, reason::k_frame_violation, "environment values must be strings");
                return std::nullopt;
            }
            proposal.environment.emplace_back(member.name, member.value.as_string());
        }
    }
    return proposal;
}

Json_value encode_process_proposal(const Process_proposal& proposal)
{
    Json_value argv = Json_value::array();
    for (const std::string& argument : proposal.argv) {
        argv.push_back(Json_value(argument));
    }
    Json_value environment_names = Json_value::array();
    for (const std::string& name : proposal.environment_names) {
        environment_names.push_back(Json_value(name));
    }
    Json_value environment = Json_value::object();
    for (const auto& [name, value] : proposal.environment) {
        environment.set(name, Json_value(value));
    }
    Json_value assets = Json_value::array();
    for (const std::string& asset : proposal.assets) {
        assets.push_back(Json_value(asset));
    }

    Json_value object = Json_value::object();
    object.set("executable",        encode_executable(proposal.executable));
    object.set("argv",              std::move(argv));
    object.set("working_directory", Json_value(working_directory_intent_name(proposal.working_directory)));
    object.set("stdin",             Json_value(stdin_behavior_name(proposal.stdin_behavior)));
    if (proposal.stdin_behavior == Stdin_behavior::PAYLOAD) {
        object.set("stdin_payload", Json_value(proposal.stdin_payload));
    }
    object.set("start",             Json_value(start_intent_name(proposal.start)));
    object.set("environment_names", std::move(environment_names));
    object.set("environment",       std::move(environment));
    object.set("assets",            std::move(assets));
    object.set("cleanup",           Json_value(cleanup_intent_name(proposal.cleanup)));
    return object;
}

bool validate_process_proposal(
    const Process_proposal&        proposal,
    const Process_proposal_policy& policy,
    Contract_failure*              out_failure)
{
    if (proposal.start != Start_intent::ATTACHED) {
        return report(out_failure, reason::k_detached_helper_proposal,
            std::string("start intent ") + start_intent_name(proposal.start));
    }

    if (proposal.executable.kind == Process_executable::Kind::LAUNCHER_PROGRAM) {
        if (!policy.launcher_program_allowed) {
            return report(out_failure, reason::k_path_escape,
                "executable names the launcher program, which this capability does not allow");
        }
    }
    else {
        const std::string& path = proposal.executable.path;
        const std::string_view executable_segment = path_first_segment(path);
        if (!is_valid_package_relative_path(path) ||
            (executable_segment != k_segment_helpers && executable_segment != k_segment_adapter))
        {
            return report(out_failure, reason::k_path_escape, "executable is not a canonical path below helpers/ or adapter/: " + path);
        }
        if (has_shell_script_extension(path)) {
            return report(out_failure, reason::k_shell_adapter_entry_point, "executable is a shell entry point: " + path);
        }
    }

    if (proposal.argv.size() > limits::k_max_argv_entries) {
        return report(out_failure, reason::k_payload_too_large, "argv exceeds k_max_argv_entries");
    }
    std::size_t argv_bytes = 0;
    for (const std::string& argument : proposal.argv) {
        if (argument.size() > limits::k_max_argv_entry_bytes || !strict::has_no_control_characters(argument)) {
            return report(out_failure, reason::k_payload_too_large, "argv entry exceeds k_max_argv_entry_bytes or contains control characters");
        }
        argv_bytes += argument.size();
    }
    if (argv_bytes > limits::k_max_argv_total_bytes) {
        return report(out_failure, reason::k_payload_too_large, "argv exceeds k_max_argv_total_bytes");
    }

    if (proposal.stdin_behavior != Stdin_behavior::PAYLOAD && !proposal.stdin_payload.empty()) {
        return report(out_failure, reason::k_frame_violation, "stdin_payload without stdin payload behavior");
    }
    if (proposal.stdin_payload.size() > limits::k_max_json_string_bytes) {
        return report(out_failure, reason::k_payload_too_large, "stdin_payload exceeds k_max_json_string_bytes");
    }

    if (proposal.environment_names.size() + proposal.environment.size() > limits::k_max_capability_environment_entries) {
        return report(out_failure, reason::k_payload_too_large, "environment exceeds k_max_capability_environment_entries");
    }
    std::set<std::string> environment_names;
    for (const std::string& name : proposal.environment_names) {
        if (!is_valid_environment_name(name) || is_reserved_environment_name(name)) {
            return report(out_failure, reason::k_reserved_environment_variable, "environment name is reserved or out of grammar: " + name);
        }
        if (!environment_names.insert(ascii_lowercase(name)).second) {
            return report(out_failure, reason::k_frame_violation,
                "environment repeats an ASCII-case-equivalent name: " + name);
        }
    }
    for (const auto& [name, value] : proposal.environment) {
        if (!is_valid_environment_name(name) || is_reserved_environment_name(name)) {
            return report(out_failure, reason::k_reserved_environment_variable, "environment name is reserved or out of grammar: " + name);
        }
        if (!environment_names.insert(ascii_lowercase(name)).second) {
            return report(out_failure, reason::k_frame_violation,
                "environment repeats an ASCII-case-equivalent name: " + name);
        }
        if (value.size() > limits::k_max_environment_value_bytes || value.find('\0') != std::string::npos ||
            !is_valid_utf8(value))
        {
            return report(out_failure, reason::k_payload_too_large, "environment value out of bounds for " + name);
        }
    }

    for (const std::string& asset : proposal.assets) {
        const std::string_view segment = path_first_segment(asset);
        if (!is_valid_package_relative_path(asset) || (segment != k_segment_assets && segment != k_segment_helpers)) {
            return report(out_failure, reason::k_path_escape, "asset is not a canonical path below assets/ or helpers/: " + asset);
        }
    }
    return true;
}

} // namespace logonomic::cli_addon
