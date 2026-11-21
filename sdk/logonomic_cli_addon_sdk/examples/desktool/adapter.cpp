#include <logonomic_cli_addon/adapter_runtime.h>
#include <logonomic_cli_addon/capability_ids.h>
#include <logonomic_cli_addon/ids.h>
#include <logonomic_cli_addon/launch_preparation.h>
#include <logonomic_cli_addon/launcher_discovery.h>
#include <logonomic_cli_addon/launcher_utilities.h>
#include <logonomic_cli_addon/package_source.h>

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace addon = logonomic::cli_addon;

constexpr char k_command[] = "desktool";
constexpr char k_display_name[] = "DeskTool";

bool native_command(
    std::string_view program, const addon::Json_value& ambient, std::string_view directory)
{
    const auto path = addon::resolve_program(program, addon::environment_pairs(ambient), directory);
    return path && addon::native_image(*path);
}

void discover(addon::Invocation_context& context)
{
    addon::Contract_failure failure;
    const auto request = addon::decode_discover_request(context.request().payload, &failure);
    if (!request) {
        context.fail(addon::Result_status::FAILED, failure.reason_code, failure.detail);
        return;
    }

    std::vector<addon::Launcher_candidate> candidates;
    if (request->platform_id == addon::host_platform_id() &&
        native_command(k_command, request->ambient_environment, request->default_home_fallback))
    {
        addon::Launcher_candidate candidate;
        candidate.candidate_id = k_command;
        candidate.display_name = k_display_name;
        candidate.script_body  = k_command;
        candidate.runner_key   = addon::k_runner_direct;
        candidate.is_default   = true;
        candidate.evidence.set("source", addon::Json_value("path"));
        candidates.push_back(std::move(candidate));
    }
    context.succeed(addon::encode_candidates(candidates));
}

void recognize(addon::Invocation_context& context)
{
    addon::Contract_failure failure;
    const auto request = addon::decode_recognize_request(context.request().payload, &failure);
    if (!request) {
        context.fail(addon::Result_status::FAILED, failure.reason_code, failure.detail);
        return;
    }

    addon::Launcher_recognition match;
    if (request->platform_id == addon::host_platform_id() && request->launcher.runner_key == addon::k_runner_direct) {
        const auto invocation = addon::literal_launcher_invocation(request->launcher, request->platform_id);
        if (invocation && !invocation->argv.empty()) {
            const auto& program = invocation->argv.front();
            const auto filename = addon::path_to_utf8(addon::path_from_utf8(program).filename());
#ifdef _WIN32
            const auto name = addon::lower(filename);
            const bool named_desktool = name == k_command || name == "desktool.exe";
#else
            const bool named_desktool = filename == k_command;
#endif
            match.recognized = named_desktool && native_command(program, request->ambient_environment, {});
        }
    }
    if (match.recognized) {
        match.display_name = k_display_name;
    }
    context.succeed(addon::encode_recognition(match));
}

void prepare(addon::Invocation_context& context)
{
    addon::Contract_failure failure;
    const auto request = addon::decode_prepare_request(context.request().payload, &failure);
    if (!request) {
        context.fail(addon::Result_status::FAILED, failure.reason_code, failure.detail);
        return;
    }
    if (request->initial_prompt) {
        context.fail(addon::Result_status::UNAVAILABLE, addon::reason::k_initial_prompt_unavailable);
        return;
    }
    // An unmanaged result keeps the user's complete command, environment and working directory.
    addon::Launch_patch patch;
    patch.disposition     = addon::Launch_disposition{false, false, false};
    patch.profile_storage = request->profile_storage;
    patch.account_context = request->account_context;
    context.succeed(addon::encode_launch_patch(patch));
}

void invoke(addon::Invocation_context& context)
{
    const auto& request = context.request();
    if (request.capability.capability_id == addon::k_launcher_discovery_capability_id) {
        if (request.operation == addon::k_discover_operation)  { return discover(context);  }
        if (request.operation == addon::k_recognize_operation) { return recognize(context); }
    }
    if (request.capability.capability_id == addon::k_launch_preparation_capability_id &&
        request.operation == addon::k_prepare_operation)
    {
        return prepare(context);
    }
    context.reject_unknown_operation();
}

} // namespace

int main()
{
    addon::Adapter_runtime_options options;
    options.adapter_name    = "desktool-adapter";
    options.adapter_version = LOGONOMIC_CLI_ADDON_ADAPTER_VERSION;
    options.capabilities    = {
        {addon::k_presentation_capability_id,       addon::k_presentation_version},
        {addon::k_launcher_discovery_capability_id, addon::k_launcher_discovery_version},
        {addon::k_launch_preparation_capability_id, addon::k_launch_preparation_execution_version},
    };

    addon::Adapter_runtime runtime(std::move(options));
    runtime.set_invoke_handler(invoke);
    return runtime.run();
}
