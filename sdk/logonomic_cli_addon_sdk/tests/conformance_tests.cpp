#include "sdk_test_support.h"

#include "logonomic_cli_addon/capability_ids.h"
#include "logonomic_cli_addon/helper_client.h"
#include "logonomic_cli_addon/helper_ingress.h"
#include "logonomic_cli_addon/host_simulator.h"
#include "logonomic_cli_addon/ids.h"
#include "logonomic_cli_addon/json.h"
#include "logonomic_cli_addon/lifecycle_events.h"
#include "logonomic_cli_addon/limits.h"
#include "logonomic_cli_addon/package_source.h"
#include "logonomic_cli_addon/package_validator.h"
#include "logonomic_cli_addon/process_proposal.h"
#include "logonomic_cli_addon/session_contracts.h"
#include "logonomic_cli_addon/sha256.h"
#include "logonomic_cli_addon/startup_observer.h"
#include "logonomic_cli_addon/transcript.h"
#include "simulator_process.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace sdk_tests {

namespace {

namespace addon = logonomic::cli_addon;
namespace fs    = std::filesystem;

constexpr const char* k_positive_id = "org.example.unknown-fixture";
constexpr int k_probe_deadline_ms   = 3000;

const addon::Capability_pair k_probe{"conformance-probe", 1};
const addon::Capability_pair k_profile_storage{addon::k_profile_storage_capability_id, addon::k_profile_storage_version};
const addon::Capability_pair k_unadopted{"org.example.unadopted", 7};

fs::path package_directory(const std::string& suffix)
{
    return fixture_packages_directory() / (std::string(k_positive_id) + suffix);
}

addon::Simulator_options options_for(const std::string& suffix)
{
    addon::Simulator_options options;
    options.package_directory = package_directory(suffix);
    return options;
}

std::string validate_reason(const std::string& suffix)
{
    addon::Directory_package_source source(package_directory(suffix));
    addon::Package_validator validator(addon::Validation_options{});
    addon::Contract_failure failure;
    return validator.validate(source, &failure).has_value() ? std::string("ok") : failure.reason_code;
}

std::string describe(const std::optional<addon::Host_outcome>& outcome)
{
    if (!outcome.has_value()) {
        return "no outcome";
    }
    return std::string(addon::host_status_name(outcome->status)) + "/" + outcome->reason_code;
}

bool outcome_is(const std::optional<addon::Host_outcome>& outcome, addon::Host_status status, const char* reason = nullptr)
{
    return outcome.has_value() && outcome->status == status && (reason == nullptr || outcome->reason_code == reason);
}

std::optional<addon::Host_outcome> probe(addon::Host_simulator& session, const std::string& operation, addon::Json_value payload,
    int deadline_ms = k_probe_deadline_ms)
{
    const std::string id = session.invoke(k_probe, operation, std::move(payload), deadline_ms);
    return session.wait_outcome(id, deadline_ms + addon::limits::k_cancel_grace_ms + 500);
}

std::optional<addon::Json_value> load_script(const std::string& name)
{
    const std::optional<std::string> text = read_file(scripts_directory() / name);
    if (!text.has_value()) {
        return std::nullopt;
    }
    return addon::parse_json(*text, addon::Json_parse_limits{}, nullptr);
}

bool has_line_containing(const std::vector<std::string>& lines, std::string_view needle)
{
    return std::any_of(lines.begin(), lines.end(), [needle](const std::string& line) {
        return line.find(needle) != std::string::npos;
    });
}

#if !defined(_WIN32)
bool ends_with_lines(const std::vector<std::string>& lines, const std::vector<std::string>& tail)
{
    return lines.size() >= tail.size() &&
           std::equal(tail.begin(), tail.end(), lines.end() - (std::ptrdiff_t)tail.size());
}
#endif

bool fixtures_available(const char* test)
{
    if (fixture_packages_directory().empty() || !fs::is_directory(package_directory(""))) {
        check(false, test, "assembled fixture packages are required (--fixture-packages <dir>)");
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// The provider-session packages (docs/conformance.md, "The provider-session
// packages"): the session, lifecycle and ingress contracts' host-side
// decoders are the oracles of every answer, and the packaged helper is run
// for real against the scripted fake wrapper the test executable doubles as.

const addon::Capability_pair k_structured_session{
    addon::k_structured_session_capability_id,
    addon::k_structured_session_version,
};
const addon::Capability_pair k_delegated_session{
    addon::k_delegated_session_capability_id,
    addon::k_delegated_session_version,
};
const addon::Capability_pair k_interactive_resume{
    addon::k_interactive_resume_capability_id,
    addon::k_interactive_resume_environment_version,
};
const addon::Capability_pair k_lifecycle_events{
    addon::k_lifecycle_events_capability_id,
    addon::k_lifecycle_events_version,
};
const addon::Capability_pair k_coordination_delivery{
    addon::k_coordination_delivery_capability_id,
    addon::k_coordination_delivery_version,
};
const addon::Capability_pair k_helper_ingress{
    addon::k_helper_ingress_capability_id,
    addon::k_helper_ingress_degradation_version,
};

constexpr const char* k_sessions_suffix            = "-provider-sessions";
constexpr const char* k_sessions_continuous_suffix = "-provider-sessions-continuous";
constexpr const char* k_classify_hang_suffix       = "-provider-sessions-classify-hang";
constexpr const char* k_foreign_digest_suffix      = "-provider-sessions-foreign-digest";
constexpr const char* k_events_file_name           = ".unknown-fixture-events.jsonl";
constexpr const char* k_transition_nonce           = "0123456789abcdef0123456789abcdef";
#if defined(_WIN32)
constexpr const char* k_helper_relative_path       = "helpers/unknown_fixture_helper.exe";
#else
constexpr const char* k_helper_relative_path       = "helpers/unknown_fixture_helper";
#endif
// A helper run takes milliseconds; a run that reaches this is stuck.
constexpr int k_child_timeout_ms = 20000;

fs::path helper_executable(const char* suffix)
{
    return package_directory(suffix) / fs::path(k_helper_relative_path);
}

std::optional<addon::Host_outcome> invoke_and_wait(
    addon::Host_simulator&                     session,
    const addon::Capability_pair&              pair,
    const char*                                operation,
    addon::Json_value                          payload,
    int                                        deadline_ms = k_probe_deadline_ms,
    const addon::Simulator_invocation_options& invocation  = addon::Simulator_invocation_options())
{
    const std::string id = session.invoke(pair, operation, std::move(payload), deadline_ms, invocation);
    return session.wait_outcome(id, deadline_ms + addon::limits::k_cancel_grace_ms + 500);
}

addon::Session_launch_context session_context(const std::string& working_directory)
{
    addon::Session_launch_context context;
    context.launcher          = addon::Launcher_request{"unknown-fixture-cli", addon::k_runner_direct, ""};
    context.launcher_argv     = {"unknown-fixture-cli"};
    context.launch_environment = addon::Launch_environment{addon::host_platform_id(),
        addon::Json_value::object(), std::nullopt};
    context.working_directory = working_directory;
    return context;
}

addon::Json_value object_of(std::initializer_list<std::pair<const char*, addon::Json_value>> members)
{
    addon::Json_value object = addon::Json_value::object();
    for (const auto& [name, value] : members) {
        object.set(name, value);
    }
    return object;
}

/// One run of a child process to completion: its exit and what it wrote.
struct Child_run
{
    bool        started   = false;
    bool        completed = false;   // exited by itself, not signalled, within the timeout
    int         exit_code = -1;
    std::string stdout_bytes;
    std::string stderr_bytes;
};

Child_run run_child(
    const fs::path&                                         executable,
    const std::vector<std::string>&                         arguments,
    const std::vector<std::pair<std::string, std::string>>& environment_overrides,
    std::string_view                                        stdin_bytes)
{
    using Clock = std::chrono::steady_clock;
    namespace process = addon::process;
    // Bytes a child may still hand over after its exit is observed.
    constexpr int k_post_exit_drain_ms = 250;

    Child_run run;
    process::Spawn_request request;
    request.executable = addon::path_to_utf8(executable);
    request.argv.push_back(request.executable);
    request.argv.insert(request.argv.end(), arguments.begin(), arguments.end());
    for (const auto& [name, value] : addon::current_process_environment()) {
        const bool overridden = std::any_of(environment_overrides.begin(), environment_overrides.end(),
            [&name](const std::pair<std::string, std::string>& override) { return override.first == name; });
        if (!overridden) {
            request.environment.push_back(name + "=" + value);
        }
    }
    for (const auto& [name, value] : environment_overrides) {
        request.environment.push_back(name + "=" + value);
    }
    request.isolate = false;

    std::string spawn_error;
    std::unique_ptr<process::Child_process> child = process::Child_process::spawn(request, &spawn_error);
    if (!child) {
        return run;
    }
    run.started = true;
    const Clock::time_point deadline = Clock::now() + std::chrono::milliseconds(k_child_timeout_ms);
    std::size_t written      = 0;
    bool        stdin_closed = stdin_bytes.empty();
    if (stdin_closed) {
        child->close_stdin();
    }
    std::optional<process::Exit_status> exit;
    std::optional<Clock::time_point>    drain_deadline;
    bool stdout_eof = false;
    bool stderr_eof = false;
    while (true) {
        if (!stdin_closed) {
            const process::Write_result result = child->write_stdin(stdin_bytes.substr(written));
            if (result.status == process::Write_status::TRANSFERRED) {
                written += result.bytes;
            }
            if (written == stdin_bytes.size() || result.status == process::Write_status::CLOSED ||
                result.status == process::Write_status::FAILED)
            {
                child->close_stdin();
                stdin_closed = true;
            }
        }
        const Clock::time_point now = Clock::now();
        if (exit.has_value() && ((stdout_eof && stderr_eof) || now >= *drain_deadline)) {
            break;
        }
        if (now >= deadline) {
            child->kill();
            return run;
        }
        const process::Activity activity = child->wait_activity(100, !stdin_closed);
        run.stdout_bytes += activity.stdout_bytes;
        run.stderr_bytes += activity.stderr_bytes;
        stdout_eof = stdout_eof || activity.stdout_eof;
        stderr_eof = stderr_eof || activity.stderr_eof;
        if (activity.exit.has_value()) {
            exit           = activity.exit;
            drain_deadline = Clock::now() + std::chrono::milliseconds(k_post_exit_drain_ms);
        }
    }
    run.completed = !exit->signaled;
    run.exit_code = exit->code;
    return run;
}

int exit_of(const Child_run& run)
{
    return run.started && run.completed ? run.exit_code : -1;
}

std::optional<addon::Json_value> stdout_json(const Child_run& run)
{
    addon::Json_parse_error error;
    return addon::parse_json(run.stdout_bytes, addon::Json_parse_limits{}, &error);
}

/// The six variables of a bound terminal, naming the test executable as the
/// wrapper.
std::vector<std::pair<std::string, std::string>> bound_helper_environment()
{
    return {
        {addon::k_helper_env_vnmctl_path,        addon::path_to_utf8(test_executable())},
        {addon::k_helper_env_session_capability, addon::to_hex(addon::Sha256::digest("session capability"))},
        {addon::k_helper_env_package_id,         std::string(k_positive_id) + k_sessions_suffix},
        {addon::k_helper_env_generation_digest,  addon::to_hex(addon::Sha256::digest("generation"))},
        {addon::k_helper_env_generation_nonce,   addon::random_nonce_hex()},
        {addon::k_helper_env_helpers_dir,        addon::path_to_utf8(helper_executable(k_sessions_suffix).parent_path())},
    };
}

bool append_line(const fs::path& path, const std::string& line)
{
    std::ofstream output(path, std::ios::binary | std::ios::app);
    output << line << '\n';
    output.close();
    return output.good();
}

// structured-session@3, delegated-session@3, interactive-resume@2,
// helper-ingress@2 of the events-mode package,
// with the helper run for real and its output inspected incrementally.
bool check_provider_session_surfaces(const char* id)
{
    bool ok = true;
    addon::Contract_failure failure;
    {
        addon::Directory_package_source source(package_directory(k_sessions_suffix));
        addon::Package_validator validator(addon::Validation_options{});
        const auto package = validator.validate(source, &failure);
        SDK_CHECK(package.has_value(), "resume fixture validates: " + failure.reason_code + " " + failure.detail);
        const addon::Capability_pair resume_pair{
            addon::k_interactive_resume_capability_id, addon::k_interactive_resume_environment_version};
        const auto sidecar = std::find_if(package->sidecars.begin(), package->sidecars.end(),
            [&](const addon::Capability_sidecar& candidate) { return candidate.pair() == resume_pair; });
        SDK_CHECK(sidecar != package->sidecars.end(), "resume fixture declares interactive-resume@2");
        const auto declaration = addon::decode_resume_declaration(sidecar->payload, &failure, resume_pair.version);
        SDK_CHECK(declaration.has_value(), "resume fixture declaration decodes: " + failure.reason_code + " " + failure.detail);
        ok &= check(declaration->environment_names.empty(), id,
            "resume fixture requires no additional environment variables");

        addon::Host_simulator startup(options_for(k_sessions_suffix), nullptr);
        SDK_CHECK(startup.start(&failure),
            "startup fixture starts through the real SDK transport");
        const auto classify = [&](const addon::Startup_observation_result& observations) {
            return invoke_and_wait(startup, resume_pair, addon::k_classify_startup_operation,
                addon::encode_startup_observation_request({addon::serialize_json(
                    addon::encode_startup_observation_result(observations))}));
        };
        for (const addon::Startup_observation_result& observations : {
                 addon::Startup_observation_result{},
                 addon::Startup_observation_result{{"exact-fixture-identity"}},
                 addon::Startup_observation_result{{"exact-fixture-identity", "conflicting-fixture-identity"}}})
        {
            const auto outcome = classify(observations);
            SDK_CHECK(outcome_is(outcome, addon::Host_status::SUCCESS),
                "startup classification returns through the negotiated resume@2 operation");
            const auto decoded = addon::decode_startup_observation_result(outcome->payload, &failure);
            SDK_CHECK(decoded && decoded->provider_session_ids == observations.provider_session_ids,
                "classification preserves empty, exact and conflicting evidence without truncation");
        }
        SDK_CHECK(outcome_is(classify({{"duplicate", "duplicate"}}), addon::Host_status::FAILED),
            "duplicate startup identities are rejected, not silently deduplicated");
        SDK_CHECK(outcome_is(invoke_and_wait(startup, resume_pair, addon::k_classify_startup_operation,
            addon::encode_startup_observation_request({"private-malformed-fixture-body"})),
            addon::Host_status::FAILED), "malformed startup telemetry fails closed");
        SDK_CHECK(outcome_is(classify({{"still-live-after-rejection"}}), addon::Host_status::SUCCESS),
            "a rejected observation does not kill the provider transport");
    }
    using Purpose = addon::Structured_purpose;


    {
        addon::Directory_package_source source(package_directory(k_sessions_suffix));
        addon::Package_validator validator(addon::Validation_options{});
        const std::optional<addon::Validated_package> package = validator.validate(source, &failure);
        SDK_CHECK(package.has_value(), "provider-sessions validates: " + failure.reason_code + " " + failure.detail);
        const std::vector<addon::Capability_pair> declared = package->declared_capabilities();
        const auto declares = [&declared](const addon::Capability_pair& pair) {
            return std::find(declared.begin(), declared.end(), pair) != declared.end();
        };
        ok &= check(declares(k_structured_session) && declares(k_delegated_session) && declares(k_interactive_resume) &&
            declares(k_lifecycle_events) && declares(k_coordination_delivery) && declares(k_helper_ingress) &&
            declares({addon::k_model_context_delivery_capability_id, addon::k_model_context_delivery_version}) &&
            declares({addon::k_model_state_capability_id, addon::k_model_state_version}) &&
            declares({addon::k_structured_model_observation_capability_id, addon::k_structured_model_observation_version}),
            id, "provider-sessions declares the capabilities used by the session tests");
        const addon::Index_entry* helper = package->index.find(k_helper_relative_path);
        ok &= check(helper != nullptr && helper->origin == addon::Resource_origin::PACKAGE, id,
            "the packaged helper is indexed as a package-owned file");
    }

    addon::Transcript_recorder recorder;
    addon::Host_simulator session(options_for(k_sessions_suffix), &recorder);
    SDK_CHECK(session.start(&failure), "provider-sessions starts: " + failure.reason_code + " " + failure.detail);
    const std::vector<addon::Capability_pair> live = session.live_capabilities();
    const auto is_live = [&live](const addon::Capability_pair& pair) {
        return std::find(live.begin(), live.end(), pair) != live.end();
    };
    ok &= check(is_live(k_structured_session) && is_live(k_delegated_session) && is_live(k_interactive_resume) &&
        is_live(k_lifecycle_events) && is_live(k_coordination_delivery) && is_live(k_helper_ingress), id,
        "the session pairs, including current helper-ingress, are live for a host adopting everything");
    ok &= check(outcome_is(invoke_and_wait(session, k_coordination_delivery, "deliver", addon::Json_value::object()),
            addon::Host_status::UNAVAILABLE, addon::reason::k_unknown_operation), id,
        "the static pair implements no operation");

    std::string error;
    const std::optional<std::string> root = addon::allocate_profile_storage_root(
        session.profile_storage_root(), session.package()->bootstrap.package_id, &error);
    SDK_CHECK(root.has_value(), "profile root allocated: " + error);
    addon::Simulator_invocation_options with_profile;
    with_profile.profile_storage = addon::Profile_storage_context{addon::k_profile_storage_version, *root};
    const fs::path workspace = make_temp_directory("sessions-workspace");
    SDK_CHECK(!workspace.empty(), "workspace directory created");
    const addon::Session_launch_context context = session_context(addon::path_to_utf8(workspace));
    const fs::path helper = helper_executable(k_sessions_suffix);

    // structured-session@3 prepare: the helper in structured mode.
    addon::Structured_prepare_request prepare;
    prepare.purpose = Purpose::READ_ONLY_REVIEW;
    prepare.context = context;
    prepare.prompt  = "Review this.";
    std::optional<addon::Structured_prepare_result> structured;
    {
        const std::optional<addon::Host_outcome> outcome = invoke_and_wait(session, k_structured_session,
            addon::k_prepare_operation, addon::encode_structured_prepare_request(prepare), k_probe_deadline_ms, with_profile);
        SDK_CHECK(outcome_is(outcome, addon::Host_status::SUCCESS), "structured prepare: " + describe(outcome));
        structured = addon::decode_structured_prepare_result(outcome->payload, &failure);
        SDK_CHECK(structured.has_value(),
            "structured prepare result decodes: " + failure.reason_code + " " + failure.detail);
        ok &= check(addon::validate_structured_process_proposal(structured->process, &failure), id,
            "the structured proposal passes the structured policy: " + failure.reason_code);
        ok &= check(structured->process.executable == addon::Process_executable(k_helper_relative_path) &&
            structured->process.argv == std::vector<std::string>{"structured", "--purpose", "read_only_review"} &&
            structured->process.stdin_behavior == addon::Stdin_behavior::PAYLOAD &&
            structured->process.stdin_payload == prepare.prompt &&
            structured->process.working_directory == addon::Working_directory_intent::SESSION_WORKSPACE, id,
            "the structured proposal launches the helper in structured mode with the prompt on stdin");
        ok &= check(structured->inspection.incremental && structured->inspection.protocol_state.empty(), id,
            "incremental inspection from the empty initial state");
        ok &= check(structured->profile_storage.has_value() && structured->profile_storage->root == *root, id,
            "structured prepare echoes the profile storage");
        ok &= check(has_line_containing(recorder.lines(), "! profile_storage acknowledged"), id,
            "the simulator accepts the echo");

        addon::Structured_prepare_request binary = prepare;
        binary.prompt = std::string("\xFF\xFE", 2);
        ok &= check(outcome_is(invoke_and_wait(session, k_structured_session, addon::k_prepare_operation,
                addon::encode_structured_prepare_request(binary), k_probe_deadline_ms, with_profile),
                addon::Host_status::FAILED, addon::reason::k_invalid_utf8), id,
            "a prompt that is not UTF-8 text is refused as invalid_utf8");
    }

    // The helper's structured run, inspected in two increments split inside
    // a protocol line and preceded by vendor noise.
    {
        const Child_run run = run_child(helper, {"structured", "--purpose", "read_only_review"}, {}, prepare.prompt);
        SDK_CHECK(exit_of(run) == 0, "the helper's structured run exits 0: " + run.stderr_bytes);
        const std::string& output = run.stdout_bytes;
        ok &= check(output.rfind("unknown-fixture-helper 1 structured read_only_review\n", 0) == 0 &&
            output.find("\nprompt-bytes 12\n") != std::string::npos &&
            output.find("\npresentation review_initialized\n") != std::string::npos &&
            output.find("\nresult-begin\n") != std::string::npos &&
            output.find("\nresult-end\n") != std::string::npos, id,
            "the helper writes the line protocol");
        const std::size_t split = output.find("prompt-bytes") + 6;

        addon::Structured_inspect_request inspect;
        inspect.purpose                   = Purpose::READ_ONLY_REVIEW;
        inspect.inspection.protocol_state = structured->inspection.protocol_state;
        inspect.inspection.stdout_bytes   = "vendor chatter before the protocol\n" + output.substr(0, split);
        std::optional<addon::Host_outcome> outcome = invoke_and_wait(session, k_structured_session,
            addon::k_inspect_operation, addon::encode_structured_inspect_request(inspect));
        SDK_CHECK(outcome_is(outcome, addon::Host_status::SUCCESS), "incremental inspect: " + describe(outcome));
        const std::optional<addon::Structured_inspect_result> partial =
            addon::decode_structured_inspect_result(outcome->payload, false, &failure);
        SDK_CHECK(partial.has_value(),
            "incremental inspect result decodes: " + failure.reason_code + " " + failure.detail);
        ok &= check(!partial->complete && !partial->protocol_state.empty() &&
            partial->presentation_state == addon::Presentation_state::UNAVAILABLE, id,
            "an incremental inspection is incomplete, carries state and reports no presentation before any presentation line");

        inspect.inspection.protocol_state = partial->protocol_state;
        inspect.inspection.stdout_bytes   = output.substr(split);
        inspect.inspection.final          = true;
        outcome = invoke_and_wait(session, k_structured_session,
            addon::k_inspect_operation, addon::encode_structured_inspect_request(inspect));
        SDK_CHECK(outcome_is(outcome, addon::Host_status::SUCCESS), "final inspect: " + describe(outcome));
        const std::optional<addon::Structured_inspect_result> completed =
            addon::decode_structured_inspect_result(outcome->payload, true, &failure);
        SDK_CHECK(completed.has_value(), "final inspect result decodes: " + failure.reason_code + " " + failure.detail);
        ok &= check(completed->complete && completed->result_bytes.has_value() && !completed->failure_reason.has_value() &&
            completed->result_bytes->find("structured read_only_review run over 12 prompt bytes") != std::string::npos, id,
            "the final inspection completes with the helper's result");
        ok &= check(completed->presentation_state == addon::Presentation_state::AVAILABLE &&
            completed->presentation.size() == 3 &&
            completed->presentation[0].kind == addon::Presentation_event_kind::REVIEW_INITIALIZED &&
            completed->presentation[0].count == 1 &&
            completed->presentation[2].kind == addon::Presentation_event_kind::ANALYSIS_UPDATE, id,
            "the presentation lines of the increment become presentation records");

        // The failure reasons of a final inspection, and a foreign state.
        const auto final_reason = [&](bool timed_out, std::int32_t exit_code, const std::string& state) {
            addon::Structured_inspect_request request;
            request.purpose                   = Purpose::READ_ONLY_REVIEW;
            request.inspection.protocol_state = state;
            request.inspection.final          = true;
            request.inspection.timed_out      = timed_out;
            request.inspection.exit_code      = exit_code;
            const std::optional<addon::Host_outcome> answer = invoke_and_wait(session, k_structured_session,
                addon::k_inspect_operation, addon::encode_structured_inspect_request(request));
            if (!outcome_is(answer, addon::Host_status::SUCCESS)) {
                return describe(answer);
            }
            const std::optional<addon::Structured_inspect_result> decoded =
                addon::decode_structured_inspect_result(answer->payload, true, &failure);
            return decoded && decoded->failure_reason ? *decoded->failure_reason : std::string("no failure reason");
        };
        ok &= check(final_reason(false, 0, "") == "startup_failed", id,
            "a final inspection without the header is startup_failed");
        ok &= check(final_reason(true, 0, "") == "process_timed_out", id,
            "a timed-out final inspection is process_timed_out");
        ok &= check(final_reason(false, 3, "") == "process_exited_3", id,
            "a non-zero exit is process_exited_<code>");
        ok &= check(final_reason(false, 0, partial->protocol_state) == "incomplete_output", id,
            "a final inspection with an open result is incomplete_output");
        ok &= check(final_reason(false, 0, "not a state") ==
                std::string(addon::host_status_name(addon::Host_status::FAILED)) + "/" + addon::reason::k_inspection_invalid,
            id, "a protocol_state the fixture did not issue is inspection_invalid");
    }

    // delegated-session@3 prepare, then the helper's delegated run over the
    // declared files the host would have written and tracks for cleanup.
    for (const std::string prompt : {"Delegate this.", "orchestration-large-result", "orchestration-invalid-utf8"})
    {
        const fs::path artifact_root = workspace / "delegation" / addon::random_nonce_hex();
        addon::Delegated_prepare_request delegated;
        delegated.purpose = Purpose::IMPLEMENTATION;
        delegated.context = context;
        delegated.prompt  = prompt;
        delegated.control.task_id = "task/" + prompt;
        delegated.control.servers.push_back({
            "coordination", addon::path_to_utf8(test_executable()), {"set_title"}, {{"EXAMPLE_TOKEN", "1"}}});
        delegated.control.private_artifact_root = addon::path_to_utf8(artifact_root);
        const std::optional<addon::Host_outcome> outcome = invoke_and_wait(session, k_delegated_session,
            addon::k_prepare_operation, addon::encode_delegated_prepare_request(delegated),
            addon::limits::k_delegated_prepare_deadline_ms, with_profile);
        SDK_CHECK(outcome_is(outcome, addon::Host_status::SUCCESS), "delegated prepare: " + describe(outcome));
        const std::optional<addon::Delegated_prepare_result> result =
            addon::decode_delegated_prepare_result(outcome->payload, &failure);
        SDK_CHECK(result.has_value(),
            "delegated prepare result decodes: " + failure.reason_code + " " + failure.detail);
        ok &= check(addon::validate_structured_process_proposal(result->process, &failure), id,
            "the delegated proposal passes the structured policy: " + failure.reason_code);
        ok &= check(addon::validate_delegated_acknowledgment(delegated.control, result->acknowledgment, &failure), id,
            "the acknowledgment names every server: " + failure.reason_code);
        const fs::path& task_root = artifact_root;
        const bool raw_result = prompt != "Delegate this.";
        ok &= check(result->process.argv == std::vector<std::string>{
                "delegated", "--purpose", "implementation", "--artifact-root", addon::path_to_utf8(task_root)} &&
            result->process.cleanup == addon::Cleanup_intent::RETAIN_SCRATCH, id,
            "the delegated proposal names its task-owned artifact root and retains its scratch");
        SDK_CHECK(result->private_artifacts.size() == (raw_result ? 3u : 2u) &&
            result->private_artifacts[0].relative_path == "mcp/servers.json" &&
            result->private_artifacts[1].relative_path == "unknown-fixture-helper/summary.json" &&
            result->private_artifacts[1].content.empty(),
            "servers and initially empty helper summary are declared under the task root");
        if (raw_result) {
            SDK_CHECK(result->private_artifacts[2].relative_path == ".unknown-fixture-result.bin" &&
                result->private_artifacts[2].content.empty(),
                "the raw result file is declared for host-owned cleanup before launch");
        }
        const std::string& servers = result->private_artifacts[0].content;
        ok &= check(servers.find("\"server_id\":\"coordination\"") != std::string::npos &&
            servers.find("EXAMPLE_TOKEN") == std::string::npos, id,
            "the servers artifact names the server and never the control environment");
        ok &= check(result->profile_storage.has_value() && result->profile_storage->root == *root, id,
            "delegated prepare echoes the profile storage");

        for (const auto& artifact : result->private_artifacts) {
            SDK_CHECK(write_file(artifact_root / artifact.relative_path, artifact.content),
                "task artifact materialized as the host would");
        }
        const Child_run run = run_child(helper, result->process.argv, {}, delegated.prompt);
        SDK_CHECK(exit_of(run) == 0, "the helper's delegated run exits 0: " + run.stderr_bytes);
        ok &= check(run.stdout_bytes.rfind("unknown-fixture-helper 1 delegated implementation\n", 0) == 0 &&
            run.stdout_bytes.find("\nservers coordination\n") != std::string::npos &&
            run.stdout_bytes.find("\nartifact unknown-fixture-helper/summary.json\n") != std::string::npos, id,
            "the delegated run names the servers it found and the artifact it wrote");
        const std::optional<std::string> summary = read_file(task_root / "unknown-fixture-helper" / "summary.json");
        ok &= check(summary.has_value() && summary->find("\"servers\":[\"coordination\"]") != std::string::npos &&
            summary->find(delegated.prompt) == std::string::npos, id,
            "the summary below the artifact root names the servers and not the prompt");
        if (raw_result) {
            addon::Structured_inspect_request inspect;
            inspect.purpose = Purpose::IMPLEMENTATION;
            inspect.inspection.stdout_bytes = run.stdout_bytes;
            inspect.inspection.stderr_bytes = run.stderr_bytes;
            inspect.inspection.final = true;
            inspect.inspection.exit_code = exit_of(run);
            const auto inspected = invoke_and_wait(session, k_structured_session,
                addon::k_inspect_operation, addon::encode_structured_inspect_request(inspect));
            SDK_CHECK(outcome_is(inspected, addon::Host_status::SUCCESS), "raw result inspection: " + describe(inspected));
            const auto decoded = addon::decode_structured_inspect_result(inspected->payload, true, &failure);
            const std::string expected = prompt == "orchestration-large-result"
                ? std::string(81920, 'x') + "\xE2\x82\xAC" "-complete"
                : std::string("\xC3\x28", 2);
            SDK_CHECK(decoded && decoded->result_bytes == expected && !decoded->failure_reason,
                "the shared result decoder accepts exact large or invalid UTF8 bytes from the real helper");
            SDK_CHECK(write_file(task_root / ".unknown-fixture-result.bin", std::string(81933, 'x')),
                "over-bound fixture result written");
            const auto oversized = invoke_and_wait(session, k_structured_session,
                addon::k_inspect_operation, addon::encode_structured_inspect_request(inspect));
            SDK_CHECK(outcome_is(oversized, addon::Host_status::SUCCESS), "over-bound file inspection");
            const auto rejected = addon::decode_structured_inspect_result(oversized->payload, true, &failure);
            SDK_CHECK(rejected && rejected->failure_reason == "fixture_result_file_unreadable" && !rejected->result_bytes,
                "an over-bound result file fails rather than truncating or allocating without a bound");
        }
        // Host cleanup owns the declared files individually, not arbitrary
        // descendants. Removing precisely that list must cover helper output.
        for (const auto& artifact : result->private_artifacts) {
            std::error_code cleanup_error;
            SDK_CHECK(fs::remove(artifact_root / artifact.relative_path, cleanup_error) && !cleanup_error,
                "declared task file removed without recursive cleanup");
        }
        for (const auto& entry : fs::recursive_directory_iterator(task_root)) {
            SDK_CHECK(!entry.is_regular_file(), "every helper-generated private file was declared for cleanup");
        }
    }

    // interactive-resume@2 prepare_resume.
    {
        addon::Resume_prepare_request resume;
        resume.context           = context;
        resume.title_instruction = "Unknown fixture";
        resume.resume_record     = object_of({{"provider_session_id", addon::Json_value("session-a")}});
        const std::optional<addon::Host_outcome> outcome = invoke_and_wait(session, k_interactive_resume,
            addon::k_prepare_resume_operation, addon::encode_resume_prepare_request(resume), k_probe_deadline_ms, with_profile);
        SDK_CHECK(outcome_is(outcome, addon::Host_status::SUCCESS), "prepare_resume: " + describe(outcome));
        const std::optional<addon::Resume_prepare_result> result =
            addon::decode_resume_prepare_result(outcome->payload, &failure);
        SDK_CHECK(result.has_value(), "prepare_resume result decodes: " + failure.reason_code + " " + failure.detail);
        ok &= check(addon::validate_resume_process_proposal(result->process, &failure), id,
            "the resume proposal passes the resume policy: " + failure.reason_code);
        ok &= check(result->process.executable == addon::Process_executable::launcher_program() &&
            result->process.argv == std::vector<std::string>{"--resume", "session-a"} &&
            result->process.cleanup == addon::Cleanup_intent::RETAIN_SCRATCH, id,
            "the resume proposal names the launcher program with --resume <id>");
        ok &= check(result->patch.set == std::vector<std::pair<std::string, std::string>>{
                {"UNKNOWN_FIXTURE_RESUME", "1"},
                {"UNKNOWN_FIXTURE_RESUME_RECORD_DIGEST", addon::resume_record_digest_hex(resume.resume_record)},
            } &&
            result->patch.unset.empty() && result->patch.profile_storage.has_value() &&
            result->patch.profile_storage->root == *root, id,
            "the resume patch sets the fixture's variable and echoes the profile storage");
        ok &= check(addon::validate_resume_acknowledgment(resume.resume_record, result->acknowledgment, &failure), id,
            "the acknowledgment digests the record the host sent");
        const addon::Json_value other_record = object_of({{"provider_session_id", addon::Json_value("session-b")}});
        ok &= check(!addon::validate_resume_acknowledgment(other_record, result->acknowledgment, &failure) &&
            failure.reason_code == addon::reason::k_acknowledgment_mismatch, id,
            "the same acknowledgment does not name another record");

        addon::Resume_prepare_request unusable = resume;
        unusable.resume_record = object_of({{"note", addon::Json_value("no session identifier")}});
        ok &= check(outcome_is(invoke_and_wait(session, k_interactive_resume, addon::k_prepare_resume_operation,
                addon::encode_resume_prepare_request(unusable), k_probe_deadline_ms, with_profile),
                addon::Host_status::FAILED, addon::reason::k_resume_record_invalid), id,
            "a record without a session identifier is resume_record_invalid");
    }

    // helper-ingress@2 classify_event.
    {
        const auto classify = [&](const char* name, const addon::Json_value& body, std::uint64_t epoch,
                                  std::optional<addon::Ledger_transition> transition)
            -> std::pair<std::optional<addon::Host_outcome>, std::optional<addon::Classify_event_result>> {
            addon::Classify_event_request request;
            request.event.name                = name;
            request.event.body                = body;
            request.ledger.current_epoch      = epoch;
            request.ledger.current_transition = std::move(transition);
            request.transition_nonce          = k_transition_nonce;
            const std::optional<addon::Host_outcome> answer = invoke_and_wait(session, k_helper_ingress,
                addon::k_classify_event_operation, addon::encode_classify_event_request(request),
                addon::limits::k_classify_event_deadline_ms);
            if (!outcome_is(answer, addon::Host_status::SUCCESS)) {
                return {answer, std::nullopt};
            }
            return {answer, addon::decode_classify_event_result(answer->payload, &failure)};
        };
        const addon::Json_value startup = object_of({
            {"session_id", addon::Json_value("session-a")},
            {"source",     addon::Json_value("startup")},
            {"sequence",   addon::Json_value(7)},
        });
        const auto [start_outcome, start] = classify("SessionStart", startup, 0, std::nullopt);
        SDK_CHECK(start.has_value(),
            "SessionStart classifies: " + describe(start_outcome) + " " + failure.reason_code + " " + failure.detail);
        addon::Provider_event start_event;
        start_event.name = "SessionStart";
        start_event.body = startup;
        ok &= check(start->classification == addon::Classification::TRANSITION &&
            start->transition == addon::Transition_kind::START && start->observations.size() == 1, id,
            "SessionStart at startup is a start transition with one observation");
        const addon::Lifecycle_observation& started = start->observations.front();
        ok &= check(started.provider_event_name == "SessionStart" &&
            started.session_transition == addon::Session_transition::STARTED &&
            started.provider_session_id == "session-a" && started.session_source == "startup" &&
            started.provider_sequence == "7" &&
            started.provider_event_id == addon::provider_event_evidence_digest_hex(start_event) &&
            started.resume_record == object_of({{"provider_session_id", addon::Json_value("session-a")}}), id,
            "the started observation names the session, its source, the evidence digest and the resume record");
        ok &= check(start->helper_response == object_of({{"seen", addon::Json_value("SessionStart")}}), id,
            "the helper response echoes the event name");

        const addon::Ledger_transition current{addon::Transition_kind::START, "session-a", k_transition_nonce};
        const auto [resume_outcome, resumed] = classify("SessionStart",
            object_of({{"session_id", addon::Json_value("session-a")}, {"source", addon::Json_value("resume")}}), 1, current);
        ok &= check(resumed.has_value() && resumed->transition == addon::Transition_kind::RESUME, id,
            "SessionStart with source resume is a resume transition: " + describe(resume_outcome));

        const auto [stop_outcome, stopped] = classify("Stop",
            object_of({{"session_id", addon::Json_value("session-a")}, {"turn_id", addon::Json_value("turn-1")}}), 1, current);
        ok &= check(stopped.has_value() && stopped->classification == addon::Classification::EVENT &&
            stopped->observations.size() == 1 &&
            stopped->observations[0].phase_transition == addon::Phase_transition::AWAITING_INPUT &&
            stopped->observations[0].phase_evidence == addon::Evidence::CONFIRMED &&
            stopped->observations[0].provider_turn_id == "turn-1" &&
            stopped->observations[0].session_transition == addon::Session_transition::NONE, id,
            "Stop is an awaiting_input event: " + describe(stop_outcome));

        const auto [prompt_outcome, prompted] = classify("UserPromptSubmit",
            object_of({{"session_id", addon::Json_value("session-a")}}), 1, current);
        ok &= check(prompted.has_value() && prompted->classification == addon::Classification::EVENT &&
            prompted->observations.size() == 1 &&
            prompted->observations[0].phase_transition == addon::Phase_transition::WORKING, id,
            "UserPromptSubmit is a working event: " + describe(prompt_outcome));

        for (const char* ignored_name : {"Ignored", "PreToolUse"}) {
            const auto [ignored_outcome, ignored] = classify(ignored_name, addon::Json_value::object(), 1, current);
            ok &= check(ignored.has_value() && ignored->classification != addon::Classification::TRANSITION &&
                ignored->classification != addon::Classification::EVENT && ignored->observations.empty() &&
                !ignored->transition.has_value() &&
                ignored->helper_response == object_of({{"seen", addon::Json_value(ignored_name)}}), id,
                std::string(ignored_name) + " is ignored with the echo: " + describe(ignored_outcome));
        }

        const auto [nameless_outcome, nameless] = classify("SessionStart",
            object_of({{"source", addon::Json_value("startup")}}), 0, std::nullopt);
        ok &= check(!nameless.has_value() &&
            outcome_is(nameless_outcome, addon::Host_status::FAILED, addon::reason::k_observation_invalid), id,
            "SessionStart without a session_id is observation_invalid: " + describe(nameless_outcome));

        const auto [again_outcome, again] = classify("SessionStart", startup, 0, std::nullopt);
        ok &= check(again.has_value() && again->observations.size() == 1 &&
            again->observations[0].provider_event_id == started.provider_event_id, id,
            "the same event classifies to the same observation identity");
    }

    // lifecycle-events@2 in events mode has no operation.
    {
        addon::Observe_request observe;
        observe.session.working_directory = context.working_directory;
        observe.session.launcher_argv     = context.launcher_argv;
        observe.window_ms                 = addon::limits::k_observe_window_ms;
        ok &= check(outcome_is(invoke_and_wait(session, k_lifecycle_events, addon::k_observe_operation,
                addon::encode_observe_request(observe)),
                addon::Host_status::UNAVAILABLE, addon::reason::k_unknown_operation), id,
            "observe in events mode is unavailable/unknown_operation");
    }

    int exit_code = -1;
    ok &= check(session.shutdown(addon::limits::k_shutdown_grace_ms, &exit_code) && exit_code == 0, id,
        "provider-sessions exits 0 within the shutdown grace");
    std::error_code ignored;
    fs::remove_all(workspace, ignored);
    return ok;
}

// The packaged helper's host-call and MCP modes against the scripted fake
// wrapper: exit codes 0/1/2/4, the minted key and nonce shapes, the printed
// outcome, and stdio handed to a server byte for byte.
bool check_provider_session_helper(const char* id)
{
    bool ok = true;
    const fs::path helper  = helper_executable(k_sessions_suffix);
    const fs::path scratch = make_temp_directory("sessions-helper");
    SDK_CHECK(!scratch.empty(), "scratch directory created");
    const std::vector<std::pair<std::string, std::string>> bound = bound_helper_environment();
    const auto run = [&](const std::vector<std::string>& arguments, std::string_view stdin_bytes = {}) {
        return run_child(helper, arguments, bound, stdin_bytes);
    };

    {
        const Child_run committed = run({"event", "Committed"});
        ok &= check(exit_of(committed) == addon::k_helper_exit_committed, id,
            "event Committed exits 0: " + committed.stderr_bytes);
        const std::optional<addon::Json_value> outcome  = stdout_json(committed);
        const addon::Json_value* accepted = outcome ? outcome->find("accepted") : nullptr;
        const addon::Json_value* epoch    = outcome ? outcome->find("epoch") : nullptr;
        const addon::Json_value* response = outcome ? outcome->find("helper_response") : nullptr;
        const addon::Json_value* received = response ? response->find("received") : nullptr;
        const addon::Json_value* key      = received ? received->find("idempotency_key") : nullptr;
        const addon::Json_value* nonce    = received ? received->find("transition_nonce") : nullptr;
        const addon::Json_value* event    = received ? received->find("event") : nullptr;
        ok &= check(accepted != nullptr && accepted->is_boolean() && accepted->as_boolean() &&
            epoch != nullptr && epoch->is_integer() && epoch->as_integer() == 3, id,
            "the committed outcome is printed with accepted and epoch: " + committed.stdout_bytes);
        ok &= check(key != nullptr && key->is_string() && addon::is_valid_transition_nonce_hex(key->as_string()) &&
            nonce != nullptr && nonce->is_string() && addon::is_valid_transition_nonce_hex(nonce->as_string()), id,
            "a minted key and nonce are 32 lowercase hex");
        ok &= check(event != nullptr && event->find("name") != nullptr && event->find("name")->is_string() &&
            event->find("name")->as_string() == "Committed" &&
            event->find("body") != nullptr && *event->find("body") == addon::Json_value::object(), id,
            "an event without --body-file carries an empty body");
    }
    {
        const fs::path body_file = scratch / "body.json";
        SDK_CHECK(write_file(body_file, R"({"session_id":"session-a","source":"startup"})"), "body file written");
        const std::string nonce = addon::random_nonce_hex();
        const Child_run committed = run({
            "event", "Committed", "--body-file", addon::path_to_utf8(body_file), "--key", "delivery-1", "--nonce", nonce});
        const std::optional<addon::Json_value> outcome  = stdout_json(committed);
        const addon::Json_value* response = outcome ? outcome->find("helper_response") : nullptr;
        const addon::Json_value* received = response ? response->find("received") : nullptr;
        const addon::Json_value* event    = received ? received->find("event") : nullptr;
        const addon::Json_value* body     = event ? event->find("body") : nullptr;
        ok &= check(exit_of(committed) == addon::k_helper_exit_committed && received != nullptr &&
            *received->find("idempotency_key") == addon::Json_value("delivery-1") &&
            *received->find("transition_nonce") == addon::Json_value(nonce) &&
            body != nullptr && body->find("session_id") != nullptr &&
            *body->find("session_id") == addon::Json_value("session-a"), id,
            "--body-file, --key and --nonce reach the wrapper as given: " + committed.stdout_bytes);
    }
    {
        const Child_run rejected = run({"event", "Rejected"});
        const std::optional<addon::Json_value> outcome = stdout_json(rejected);
        ok &= check(exit_of(rejected) == addon::k_helper_exit_committed && outcome.has_value() &&
            *outcome->find("accepted") == addon::Json_value(false) &&
            *outcome->find("reason") == addon::Json_value("stale_epoch") &&
            *outcome->find("epoch") == addon::Json_value(2), id,
            "a committed rejection exits 0 and prints the reason: " + rejected.stdout_bytes);
    }
    ok &= check(exit_of(run({"event", "Transport"})) == addon::k_helper_exit_transport_failure, id,
        "a transport failure of the wrapper exits 1");
    ok &= check(exit_of(run({"event", "Pending"})) == addon::k_helper_exit_pending, id,
        "a delivery still pending at the deadline exits 4");
    ok &= check(exit_of(run({"event", "Garbage"})) == addon::k_helper_exit_transport_failure, id,
        "an outcome the client cannot parse is a transport failure");
    ok &= check(exit_of(run({"set-title", "Unknown fixture"})) == addon::k_helper_exit_committed, id,
        "set-title exits 0");
    ok &= check(exit_of(run({"set-title", "unreachable"})) == addon::k_helper_exit_transport_failure, id,
        "set-title reports the wrapper's transport failure");
    ok &= check(exit_of(run({"changed-files", "src/a.cpp", "src/b.cpp"})) == addon::k_helper_exit_committed, id,
        "changed-files exits 0");
    ok &= check(exit_of(run({"changed-files"})) == addon::k_helper_exit_invalid_arguments, id,
        "changed-files without a path is a usage error");
    {
        const fs::path manifest = scratch / "manifest.json";
        const fs::path garbage  = scratch / "garbage.txt";
        SDK_CHECK(write_file(manifest, R"({"files":[]})") && write_file(garbage, "not a manifest"),
            "manifest files written");
        const int shown   = exit_of(run({"show-diff", "--manifest-file", addon::path_to_utf8(manifest)}));
        const int refused = exit_of(run({"show-diff", "--manifest-file", addon::path_to_utf8(garbage)}));
        ok &= check(shown == addon::k_helper_exit_committed, id, "show-diff exits 0");
        ok &= check(refused == addon::k_helper_exit_invalid_arguments, id,
            "show-diff with a manifest that is not an object is refused by the client with 2");
    }
    const struct
    {
        std::vector<std::string> arguments;
        const char*              omission;
    }
    usage_errors[] = {
        {{},                                           "no mode"},
        {{"no-such-mode"},                             "an unknown mode"},
        {{"event"},                                    "event without a name"},
        {{"show-diff"},                                "show-diff without the manifest"},
        {{"structured"},                               "structured without --purpose"},
        {{"delegated", "--purpose", "implementation"}, "delegated without --artifact-root"},
    };
    for (const auto& entry : usage_errors) {
        ok &= check(exit_of(run(entry.arguments)) == addon::k_helper_exit_invalid_arguments, id,
            std::string(entry.omission) + " is a usage error");
    }
    {
        std::vector<std::pair<std::string, std::string>> unbound = bound;
        for (auto& [name, value] : unbound) {
            if (name == addon::k_helper_env_session_capability) {
                // Override an ambient host capability as well as the fixture's.
                value.clear();
            }
        }
        const Child_run run_unbound = run_child(helper, {"set-title", "Unknown fixture"}, unbound, {});
        ok &= check(exit_of(run_unbound) == addon::k_helper_exit_invalid_arguments &&
            run_unbound.stderr_bytes.find(addon::reason::k_invalid_helper_environment) != std::string::npos, id,
            "an incomplete bound environment exits 2 naming invalid_helper_environment");
    }

    // The MCP modes hand the helper's streams to the server: its stdin is
    // the server's, its exit code is the server's, and the server's stdout
    // is the helper's.
    const std::string wrapper = addon::path_to_utf8(test_executable());
    ok &= check(exit_of(run({"coordination-mcp", wrapper, "call", "logonomic.diff", "show", "--manifest-stdin"},
            R"({"files":[]})")) == addon::k_helper_exit_committed, id,
        "coordination-mcp hands stdin to the server and returns its exit code");
    ok &= check(exit_of(run({"delegation-mcp", wrapper, "call", "logonomic.diff", "show", "--manifest-stdin"},
            "not a manifest")) == addon::k_helper_exit_invalid_arguments, id,
        "delegation-mcp returns the server's failure exit code");
    {
        const addon::Json_value arguments = object_of({
            {"idempotency_key",  addon::Json_value("delivery-2")},
            {"transition_nonce", addon::Json_value(k_transition_nonce)},
            {"event",            object_of({{"name", addon::Json_value("Committed")}, {"body", addon::Json_value::object()}})},
        });
        const Child_run proxied = run({"coordination-mcp", wrapper, "provider-event", "deliver", "--args-stdin"},
            addon::serialize_json(arguments));
        const std::optional<addon::Json_value> outcome = stdout_json(proxied);
        ok &= check(exit_of(proxied) == addon::k_helper_exit_committed && outcome.has_value() &&
            *outcome->find("accepted") == addon::Json_value(true), id,
            "the server's stdout is the helper's stdout byte for byte: " + proxied.stdout_bytes);
    }
    ok &= check(exit_of(run({"coordination-mcp", addon::path_to_utf8(scratch / "missing-server")})) ==
            addon::k_helper_exit_transport_failure, id, "a server that cannot start exits 1");
    ok &= check(exit_of(run({"coordination-mcp"})) == addon::k_helper_exit_invalid_arguments, id,
        "an MCP mode without the server program is a usage error");

    std::error_code ignored;
    fs::remove_all(scratch, ignored);
    return ok;
}

// lifecycle-events@2 in continuous mode: a window's progress wakes, the
// poll's pull with truthful `more`, the cursor grammar and cancellation.
bool check_provider_session_continuous(const char* id)
{
    bool ok = true;
    addon::Contract_failure failure;
    const fs::path workspace = make_temp_directory("sessions-continuous");
    SDK_CHECK(!workspace.empty(), "workspace directory created");
    const fs::path events = workspace / k_events_file_name;
    SDK_CHECK(write_file(events, ""), "events file created empty");

    addon::Host_simulator session(options_for(k_sessions_continuous_suffix), nullptr);
    SDK_CHECK(session.start(&failure),
        "provider-sessions-continuous starts: " + failure.reason_code + " " + failure.detail);

    addon::Observe_request observe;
    observe.session.working_directory = addon::path_to_utf8(workspace);
    observe.session.launcher_argv     = {"unknown-fixture-cli"};
    observe.window_ms                 = addon::limits::k_observe_window_ms;
    const std::string window = session.invoke(k_lifecycle_events, addon::k_observe_operation,
        addon::encode_observe_request(observe), addon::limits::k_observe_window_ms + addon::limits::k_observe_window_grace_ms);
    session.pump(300);
    ok &= check(session.progress(window).empty() && session.state() == addon::Simulator_state::READY, id,
        "no progress before the first observation");

    const auto observation_line = [](const char* name, const char* event_id) {
        addon::Lifecycle_observation observation;
        observation.provider_event_name = name;
        observation.provider_session_id = "session-1";
        observation.provider_event_id   = event_id;
        if (std::string_view(name) == "SessionStart") {
            observation.session_transition = addon::Session_transition::STARTED;
        }
        else {
            observation.phase_transition = addon::Phase_transition::WORKING;
            observation.phase_evidence   = addon::Evidence::CONFIRMED;
        }
        return addon::serialize_json(addon::encode_lifecycle_observation(observation));
    };
    // Pumps until the window has announced `available` observations; two
    // lines appended together may arrive as one frame or two.
    const auto wait_for_announcement = [&session, &window](std::uint64_t available) {
        for (int waited = 0; waited < 3000; waited += 50) {
            const std::vector<addon::Progress_message> progress = session.progress(window);
            if (!progress.empty() && progress.back().completed >= available) {
                break;
            }
            session.pump(50);
        }
        return session.progress(window);
    };

    SDK_CHECK(append_line(events, observation_line("SessionStart", "event-1")), "first observation appended");
    const std::vector<addon::Progress_message> first_progress = wait_for_announcement(1);
    ok &= check(first_progress.size() == 1 && first_progress[0].completed == 1 && !first_progress[0].total.has_value() &&
        first_progress[0].note == addon::observe_progress_note(1), id,
        "one new line wakes the host with one progress frame naming one available observation");

    addon::Poll_observations_request poll;
    poll.limit = addon::limits::k_max_observations_per_result;
    std::optional<addon::Host_outcome> pulled = invoke_and_wait(session, k_lifecycle_events,
        addon::k_poll_observations_operation, addon::encode_poll_observations_request(poll));
    SDK_CHECK(outcome_is(pulled, addon::Host_status::SUCCESS), "poll from the empty cursor: " + describe(pulled));
    std::optional<addon::Poll_observations_result> observations =
        addon::decode_poll_observations_result(pulled->payload, poll.limit, &failure);
    SDK_CHECK(observations.has_value(), "poll result decodes: " + failure.reason_code + " " + failure.detail);
    ok &= check(observations->observations.size() == 1 && observations->observations[0].provider_event_id == "event-1" &&
        observations->observations[0].session_transition == addon::Session_transition::STARTED &&
        !observations->more && !observations->resume_cursor.empty(), id,
        "the empty cursor polls the observed session from its first line");
    const std::string first_cursor = observations->resume_cursor;

    SDK_CHECK(append_line(events, observation_line("UserPromptSubmit", "event-2")) &&
        append_line(events, observation_line("Stop", "event-3")), "two more observations appended");
    const std::vector<addon::Progress_message> more_progress = wait_for_announcement(3);
    ok &= check(more_progress.size() >= 2 && more_progress.size() <= 3 && more_progress.back().completed == 3 &&
        more_progress.back().note == addon::observe_progress_note(3), id,
        "growth is announced as the count available since the window began");

    poll.resume_cursor = first_cursor;
    poll.limit         = 1;
    pulled       = invoke_and_wait(session, k_lifecycle_events, addon::k_poll_observations_operation,
        addon::encode_poll_observations_request(poll));
    observations = outcome_is(pulled, addon::Host_status::SUCCESS)
        ? addon::decode_poll_observations_result(pulled->payload, poll.limit, &failure)
        : std::nullopt;
    ok &= check(observations.has_value() && observations->observations.size() == 1 &&
        observations->observations[0].provider_event_id == "event-2" && observations->more, id,
        "a poll with limit 1 returns the next observation and says more: " + describe(pulled));
    if (observations.has_value()) {
        poll.resume_cursor = observations->resume_cursor;
    }
    poll.limit   = addon::limits::k_max_observations_per_result;
    pulled       = invoke_and_wait(session, k_lifecycle_events, addon::k_poll_observations_operation,
        addon::encode_poll_observations_request(poll));
    observations = outcome_is(pulled, addon::Host_status::SUCCESS)
        ? addon::decode_poll_observations_result(pulled->payload, poll.limit, &failure)
        : std::nullopt;
    ok &= check(observations.has_value() && observations->observations.size() == 1 &&
        observations->observations[0].provider_event_id == "event-3" && !observations->more, id,
        "the following poll returns the last observation and no more: " + describe(pulled));
    const std::string last_cursor = observations ? observations->resume_cursor : first_cursor;

    SDK_CHECK(append_line(events, "not an observation"), "an invalid line appended");
    poll.resume_cursor = last_cursor;
    pulled = invoke_and_wait(session, k_lifecycle_events, addon::k_poll_observations_operation,
        addon::encode_poll_observations_request(poll));
    ok &= check(outcome_is(pulled, addon::Host_status::FAILED, addon::reason::k_observation_invalid) &&
        pulled->message.find("line 4") != std::string::npos, id,
        "a line that is not an observation fails the poll naming the line: " + describe(pulled));
    poll.resume_cursor = "not-a-cursor";
    ok &= check(outcome_is(invoke_and_wait(session, k_lifecycle_events, addon::k_poll_observations_operation,
            addon::encode_poll_observations_request(poll)), addon::Host_status::FAILED, addon::reason::k_frame_violation), id,
        "a cursor the fixture did not issue is frame_violation");

    session.cancel(window);
    const std::optional<addon::Host_outcome> cancelled =
        session.wait_outcome(window, addon::limits::k_cancel_grace_ms + 500);
    ok &= check(outcome_is(cancelled, addon::Host_status::CANCELLED) && session.state() == addon::Simulator_state::READY, id,
        "a cancelled window answers cancelled and leaves the generation ready: " + describe(cancelled));

    // A short window from the last cursor elapses on its own: it announces
    // the one line beyond the cursor and returns the cursor moved past it.
    observe.resume_cursor = last_cursor;
    observe.window_ms     = 500;
    const std::optional<addon::Host_outcome> elapsed = invoke_and_wait(session, k_lifecycle_events, addon::k_observe_operation,
        addon::encode_observe_request(observe), observe.window_ms + addon::limits::k_observe_window_grace_ms);
    const std::optional<addon::Observe_result> window_result = outcome_is(elapsed, addon::Host_status::SUCCESS)
        ? addon::decode_observe_result(elapsed->payload, &failure)
        : std::nullopt;
    ok &= check(window_result.has_value() &&
        window_result->resume_cursor == last_cursor.substr(0, last_cursor.find(':')) + ":4", id,
        "an elapsed window succeeds with the cursor past the announced lines: " + describe(elapsed));

    int exit_code = -1;
    ok &= check(session.shutdown(addon::limits::k_shutdown_grace_ms, &exit_code) && exit_code == 0, id,
        "provider-sessions-continuous exits 0 within the shutdown grace");
    std::error_code ignored;
    fs::remove_all(workspace, ignored);
    return ok;
}

// The --classify-hang variant: classify_event answers only the cancel, and
// nothing else of the behaviour changes.
bool check_classify_hang_variant(const char* id)
{
    bool ok = true;
    addon::Contract_failure failure;
    addon::Host_simulator session(options_for(k_classify_hang_suffix), nullptr);
    SDK_CHECK(session.start(&failure),
        "provider-sessions-classify-hang starts: " + failure.reason_code + " " + failure.detail);

    addon::Classify_event_request request;
    request.event.name       = "SessionStart";
    request.event.body       = object_of({{"session_id", addon::Json_value("session-a")}});
    request.transition_nonce = k_transition_nonce;
    const std::string hanging = session.invoke(k_helper_ingress, addon::k_classify_event_operation,
        addon::encode_classify_event_request(request), addon::limits::k_observe_window_ms);
    session.pump(200);
    ok &= check(!session.outcome(hanging).has_value(), id, "classify_event has not answered");
    session.cancel(hanging);
    const std::optional<addon::Host_outcome> cancelled =
        session.wait_outcome(hanging, addon::limits::k_cancel_grace_ms + 500);
    ok &= check(outcome_is(cancelled, addon::Host_status::CANCELLED) && session.state() == addon::Simulator_state::READY, id,
        "the hanging classification answers the cancel within the grace: " + describe(cancelled));

    addon::Resume_prepare_request resume;
    resume.context       = session_context("/unknown-fixture-session");
    resume.resume_record = object_of({{"provider_session_id", addon::Json_value("session-a")}});
    ok &= check(outcome_is(invoke_and_wait(session, k_interactive_resume, addon::k_prepare_resume_operation,
            addon::encode_resume_prepare_request(resume)), addon::Host_status::SUCCESS), id,
        "the variant flag leaves the other capabilities answering");
    int exit_code = -1;
    ok &= check(session.shutdown(addon::limits::k_shutdown_grace_ms, &exit_code) && exit_code == 0, id,
        "the variant exits 0 within the shutdown grace");
    return ok;
}

// The --resume-foreign-digest variant: prepare_resume acknowledges the digest
// of another session's record, and nothing else of the behaviour changes.
bool check_resume_foreign_digest_variant(const char* id)
{
    bool ok = true;
    addon::Contract_failure failure;
    addon::Host_simulator session(options_for(k_foreign_digest_suffix), nullptr);
    SDK_CHECK(session.start(&failure),
        "provider-sessions-foreign-digest starts: " + failure.reason_code + " " + failure.detail);

    addon::Resume_prepare_request resume;
    resume.context       = session_context("/unknown-fixture-session");
    resume.resume_record = object_of({{"provider_session_id", addon::Json_value("session-a")}});
    const std::optional<addon::Host_outcome> outcome = invoke_and_wait(session, k_interactive_resume,
        addon::k_prepare_resume_operation, addon::encode_resume_prepare_request(resume));
    SDK_CHECK(outcome_is(outcome, addon::Host_status::SUCCESS), "prepare_resume: " + describe(outcome));
    const std::optional<addon::Resume_prepare_result> result =
        addon::decode_resume_prepare_result(outcome->payload, &failure);
    SDK_CHECK(result.has_value(), "prepare_resume result decodes: " + failure.reason_code + " " + failure.detail);
    ok &= check(!addon::validate_resume_acknowledgment(resume.resume_record, result->acknowledgment, &failure) &&
        failure.reason_code == addon::reason::k_acknowledgment_mismatch, id,
        "the acknowledgment does not name the record the host sent");
    const addon::Json_value foreign_record = object_of({{"provider_session_id", addon::Json_value("session-a-other")}});
    ok &= check(addon::validate_resume_acknowledgment(foreign_record, result->acknowledgment, &failure), id,
        "the acknowledgment names the record of <id>-other");
    ok &= check(result->process.executable == addon::Process_executable::launcher_program() &&
        result->process.argv == std::vector<std::string>{"--resume", "session-a"} &&
        result->patch.set == std::vector<std::pair<std::string, std::string>>{
                {"UNKNOWN_FIXTURE_RESUME", "1"},
                {"UNKNOWN_FIXTURE_RESUME_RECORD_DIGEST", addon::resume_record_digest_hex(resume.resume_record)},
            }, id,
        "the proposal and the patch are the provider-sessions answer");

    addon::Classify_event_request request;
    request.event.name       = "SessionStart";
    request.event.body       = object_of({{"session_id", addon::Json_value("session-a")}});
    request.transition_nonce = k_transition_nonce;
    ok &= check(outcome_is(invoke_and_wait(session, k_helper_ingress, addon::k_classify_event_operation,
            addon::encode_classify_event_request(request), addon::limits::k_classify_event_deadline_ms),
            addon::Host_status::SUCCESS), id,
        "the variant flag leaves the other capabilities answering");
    int exit_code = -1;
    ok &= check(session.shutdown(addon::limits::k_shutdown_grace_ms, &exit_code) && exit_code == 0, id,
        "the variant exits 0 within the shutdown grace");
    return ok;
}

} // namespace

bool test_fixture_positive_package()
{
    bool ok = true;
    const char* id = __func__;
    if (!fixtures_available(id)) {
        return false;
    }

    // Validation and declared capabilities.
    addon::Directory_package_source source(package_directory(""));
    addon::Package_validator validator(addon::Validation_options{});
    addon::Contract_failure failure;
    const std::optional<addon::Validated_package> package = validator.validate(source, &failure);
    SDK_CHECK(package.has_value(), "positive package validates: " + failure.reason_code + " " + failure.detail);
    const std::vector<addon::Capability_pair> declared = package->declared_capabilities();
    ok &= check(declared.size() == 3 &&
        declared[0] == addon::Capability_pair{"conformance-probe", 1} &&
        declared[1] == addon::Capability_pair{"org.example.unadopted", 7} &&
        declared[2] == k_profile_storage, id, "three sidecars declared in index order");
    const addon::Index_entry* mark = package->index.find("assets/mark.svg");
    ok &= check(mark != nullptr && mark->origin == addon::Resource_origin::THIRD_PARTY, id, "third-party asset classified");
    const addon::Index_entry* banner = package->index.find("assets/fixture-banner.txt");
    ok &= check(banner != nullptr && banner->origin == addon::Resource_origin::PACKAGE, id, "package asset carries its explicit disposition");

    // Handshake and effective capability set: the simulator adopts every
    // declared sidecar unless told otherwise, so all three pairs are live.
    addon::Transcript_recorder recorder;
    addon::Host_simulator session(options_for(""), &recorder);
    SDK_CHECK(session.start(&failure), "positive package starts: " + failure.reason_code + " " + failure.detail);
    const std::vector<addon::Capability_pair> live = session.live_capabilities();
    ok &= check(live.size() == 3 &&
        std::find(live.begin(), live.end(), k_probe) != live.end() &&
        std::find(live.begin(), live.end(), k_profile_storage) != live.end() &&
        std::find(live.begin(), live.end(), k_unadopted) != live.end(), id,
        "every pair that sidecar and handshake both declare is live for a host adopting everything");
    ok &= check(session.session()->adapter_hello()->adapter_name == "unknown-fixture-adapter", id, "adapter identity in the hello");

    // The environment the adapter received.
    std::size_t reserved = 0;
    bool has_package_id = false;
    for (const std::string& entry : session.adapter_environment()) {
        if (addon::is_reserved_environment_name(entry)) {
            ++reserved;
        }
        has_package_id = has_package_id || entry == std::string(addon::k_env_package_id) + "=" + k_positive_id;
    }
    ok &= check(reserved == 7 && has_package_id, id, "adapter environment carries exactly the reserved variables");

    // Echo round trip.
    addon::Json_value payload = addon::Json_value::object();
    payload.set("marker", addon::Json_value("positive"));
    const std::optional<addon::Host_outcome> echo = probe(session, "echo", payload);
    ok &= check(outcome_is(echo, addon::Host_status::SUCCESS) && echo->payload == payload, id, "echo: " + describe(echo));

    // Host-side dispositions that never reach the adapter.
    const auto check_refusal = [&](addon::Json_value value) {
        const auto request = session.invoke(k_probe, "echo", std::move(value), k_probe_deadline_ms);
        ok &= check(outcome_is(session.outcome(request), addon::Host_status::FAILED, addon::reason::k_payload_too_large),
            id, "the host refuses an unrepresentable request before transmission");
    };
    {
        auto value = addon::Json_value::object();
        std::string text(addon::limits::k_max_json_string_bytes, 'x');
        value.set("text", addon::Json_value(text));
        ok &= check(outcome_is(probe(session, "echo", value), addon::Host_status::SUCCESS),
            id, "a string at the protocol limit makes the round trip");
        text.push_back('x');
        value.set("text", addon::Json_value(std::move(text)));
        check_refusal(std::move(value));

        auto members = addon::Json_value::object();
        for (std::size_t i = 0; i < addon::limits::k_max_json_members; ++i) {
            members.set(std::to_string(i), addon::Json_value(true));
        }
        ok &= check(outcome_is(probe(session, "echo", members), addon::Host_status::SUCCESS),
            id, "an object at the protocol member limit makes the round trip");
        members.set("extra", addon::Json_value(true));
        check_refusal(std::move(members));

        auto invalid_text = addon::Json_value::object();
        invalid_text.set("text", addon::Json_value(std::string(1, (char)0xff)));
        check_refusal(std::move(invalid_text));
        ok &= check(outcome_is(probe(session, "echo", payload), addon::Host_status::SUCCESS),
            id, "the adapter still answers after the local refusals");
    }
    {
        const std::string unknown = session.invoke({"org.example.absent", 1}, "noop", addon::Json_value::object(), 1000);
        ok &= check(outcome_is(session.outcome(unknown), addon::Host_status::UNKNOWN_CAPABILITY, addon::reason::k_unknown_capability), id,
            "unknown capability is decided locally");
        const std::string version = session.invoke({"conformance-probe", 3}, "echo", addon::Json_value::object(), 1000);
        ok &= check(outcome_is(session.outcome(version), addon::Host_status::UNSUPPORTED_VERSION, addon::reason::k_unsupported_version), id,
            "unsupported version is decided locally");
        const std::string unimplemented =
            session.invoke(k_unadopted, "noop", addon::Json_value::object(), k_probe_deadline_ms);
        const std::optional<addon::Host_outcome> answer = session.wait_outcome(unimplemented, k_probe_deadline_ms);
        ok &= check(outcome_is(answer, addon::Host_status::UNAVAILABLE, addon::reason::k_unknown_operation), id,
            "a live pair the adapter implements no operation for answers unavailable/unknown_operation");
    }

    // The host-adopted leg of the intersection: a host that adopts only the
    // two capabilities it consumes never sees org.example.unadopted@7.
    {
        addon::Simulator_options options = options_for("");
        options.adopted_capabilities = std::vector<addon::Capability_pair>{k_profile_storage, k_probe};
        addon::Host_simulator adopting(options, nullptr);
        SDK_CHECK(adopting.start(&failure), "adopting host starts: " + failure.reason_code);
        const std::vector<addon::Capability_pair> adopted_live = adopting.live_capabilities();
        const bool unadopted_live =
            std::find(adopted_live.begin(), adopted_live.end(), k_unadopted) != adopted_live.end();
        ok &= check(adopted_live.size() == 2 && !unadopted_live, id,
            "an unadopted pair is not live although sidecar and handshake declare it");
        const std::string request = adopting.invoke(k_unadopted, "noop", addon::Json_value::object(), 1000);
        const std::optional<addon::Host_outcome> refusal = adopting.outcome(request);
        ok &= check(
                outcome_is(refusal, addon::Host_status::UNKNOWN_CAPABILITY, addon::reason::k_unknown_capability),
            id, "an unadopted pair is unknown_capability, not a sidecar/handshake disagreement");
        adopting.shutdown(addon::limits::k_shutdown_grace_ms, nullptr);
    }

    // profile-storage@1 exact acknowledgment through the host-side helpers.
    {
        std::string error;
        const std::optional<std::string> root = addon::allocate_profile_storage_root(session.profile_storage_root(), k_positive_id, &error);
        SDK_CHECK(root.has_value(), "profile root allocated: " + error);
        ok &= check(root->find(k_positive_id) != std::string::npos && root->find("default") != std::string::npos, id,
            "root is <profile_storage_root>/<package_id>/default");
        addon::Simulator_invocation_options invocation;
        invocation.profile_storage = addon::Profile_storage_context{1, *root};
        const std::string ack = session.invoke(k_profile_storage, addon::k_profile_storage_acknowledge, addon::Json_value::object(),
            k_probe_deadline_ms, invocation);
        const std::optional<addon::Host_outcome> outcome = session.wait_outcome(ack, k_probe_deadline_ms);
        ok &= check(outcome_is(outcome, addon::Host_status::SUCCESS), id, "acknowledge: " + describe(outcome));
        ok &= check(has_line_containing(recorder.lines(), "! profile_storage acknowledged"), id, "acknowledgment recorded");

        // The same echo against another package's root is a mismatch by construction.
        const addon::Profile_storage_context other{1, *root + "-other-package"};
        ok &= check(outcome.has_value() && addon::profile_storage_acknowledgment_failure(other, outcome->payload) == addon::reason::k_profile_storage_mismatch, id,
            "a foreign root is a mismatch");
        const addon::Profile_storage_context other_version{2, *root};
        ok &= check(outcome.has_value() && addon::profile_storage_acknowledgment_failure(other_version, outcome->payload) == addon::reason::k_profile_storage_version_mismatch, id,
            "a different version is a version mismatch");
        ok &= check(addon::profile_storage_acknowledgment_failure(other, addon::Json_value::object()) == addon::reason::k_profile_storage_unacknowledged, id,
            "absence is unacknowledged");
    }

    // The detached proposal the probe returns is rejected by the validator.
    {
        const std::optional<addon::Host_outcome> outcome = probe(session, "propose_detached", addon::Json_value::object());
        SDK_CHECK(outcome_is(outcome, addon::Host_status::SUCCESS), "propose_detached: " + describe(outcome));
        const addon::Json_value* proposal_value = outcome->payload.find("process_proposal");
        SDK_CHECK(proposal_value != nullptr, "payload carries process_proposal");
        const std::optional<addon::Process_proposal> proposal = addon::decode_process_proposal(*proposal_value, &failure);
        SDK_CHECK(proposal.has_value(), "proposal decodes: " + failure.reason_code);
        ok &= check(!addon::validate_process_proposal(*proposal, addon::Process_proposal_policy{}, &failure) && failure.reason_code == addon::reason::k_detached_helper_proposal, id,
            "detached helper proposal is rejected as nonconforming");
    }

    // Graceful shutdown.
    int exit_code = -1;
    ok &= check(session.shutdown(addon::limits::k_shutdown_grace_ms, &exit_code) && exit_code == 0, id,
        "adapter exits 0 within the shutdown grace");
    ok &= check(session.state() == addon::Simulator_state::RETIRED && session.retirement().reason_code == addon::reason::k_shutdown, id,
        "retired by shutdown");
    return ok;
}

bool test_adapter_runtime_worker_lifetime()
{
    bool ok = true;
    const char* id = __func__;
    if (!fixtures_available(id)) {
        return false;
    }

    // core_protocol.md requires one terminal result and exit within the
    // shutdown grace. Neither a result nor handler return completes TLS
    // cleanup. Keep a control worker running before that cleanup starts:
    // Windows may hold its loader lock inside a TLS destructor, so this
    // reader check must not depend on starting another thread afterwards.
    const struct
    {
        const char* kind;
        bool        failure;
    }
    cases[] = {
        {"handler", false},
        {"handler", true },
        {"thread",  false},
        {"thread",  true },
    };
    for (const auto& cleanup : cases) {
        addon::Host_simulator session(options_for(""), nullptr);
        addon::Contract_failure failure;
        SDK_CHECK(session.start(&failure), "cleanup fixture starts: " + failure.reason_code);

        addon::Json_value progress = addon::Json_value::object();
        progress.set("count", addon::Json_value((std::int64_t)1));
        progress.set("interval_ms", addon::Json_value((std::int64_t)0));
        const std::string control = session.invoke(k_probe, "progress_then_wait", std::move(progress), 30000);
        const auto control_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(k_probe_deadline_ms);
        while (session.progress_count(control) == 0 && std::chrono::steady_clock::now() < control_deadline) {
            session.pump(10);
        }
        SDK_CHECK(session.progress_count(control) == 1, "control worker is running before cleanup starts");

        addon::Json_value payload = addon::Json_value::object();
        payload.set("kind", addon::Json_value(cleanup.kind));
        payload.set("failure", addon::Json_value(cleanup.failure));
        const std::optional<addon::Host_outcome> outcome = probe(session, "result_then_cleanup", std::move(payload));
        SDK_CHECK(outcome_is(outcome, cleanup.failure ? addon::Host_status::FAILED : addon::Host_status::SUCCESS,
            cleanup.failure ? "cleanup_failure" : nullptr), "terminal result arrives before cleanup: " + describe(outcome));

        const std::string marker = std::string("fixture ") + cleanup.kind + " cleanup entered";
        const auto cleanup_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(k_probe_deadline_ms);
        while (session.stderr_text().find(marker) == std::string::npos &&
            std::chrono::steady_clock::now() < cleanup_deadline)
        {
            session.pump(10);
        }
        SDK_CHECK(session.stderr_text().find(marker) != std::string::npos, "cleanup has entered before reader and shutdown checks");

        // This input is consumed before shutdown is sent. The reader must
        // pass its normal reaping step again to read the shutdown frame.
        session.cancel(control);
        ok &= check(outcome_is(session.wait_outcome(control, k_probe_deadline_ms), addon::Host_status::CANCELLED), id,
            "the existing worker receives cancel while cleanup remains active");
        // These rows consume the adapter's grace. A 2000 ms host grace
        // leaves its existing 10% observation margin at 200 ms, allowing
        // scheduling headroom while still requiring exit before recovery.
        int exit_code = -1;
        ok &= check(session.shutdown(addon::limits::k_cancel_grace_ms, &exit_code) && exit_code == 0, id,
            std::string(cleanup.kind) + " cleanup does not extend shutdown grace after " +
                (cleanup.failure ? "error" : "success") + "; stderr=" + session.stderr_text());
        ok &= check(session.stderr_text().find("logonomic-cli-addon runtime: Abandoning ") != std::string::npos, id,
            std::string(cleanup.kind) + " shutdown diagnostic identifies the SDK runtime: " + session.stderr_text());
    }

    // A terminal result frees the host's admission slot while the runtime
    // retains the context, including shutdown cancellation, until exit.
    addon::Simulator_options options = options_for("");
    options.limits.max_in_flight_requests = 1;
    addon::Host_simulator session(options, nullptr);
    addon::Contract_failure failure;
    SDK_CHECK(session.start(&failure), "cancellable cleanup fixture starts: " + failure.reason_code);
    addon::Json_value payload = addon::Json_value::object();
    payload.set("kind", addon::Json_value("cancel"));
    payload.set("failure", addon::Json_value(false));
    SDK_CHECK(outcome_is(probe(session, "result_then_cleanup", std::move(payload)), addon::Host_status::SUCCESS),
        "terminal result arrives while the handler is waiting for cancellation");
    for (int i = 0; i < 2; ++i) {
        const std::optional<addon::Host_outcome> echo = probe(session, "echo", addon::Json_value::object());
        ok &= check(outcome_is(echo, addon::Host_status::SUCCESS), id,
            "the next request is admitted and answered during cleanup: " + describe(echo));
    }
    int exit_code = -1;
    ok &= check(session.shutdown(500, &exit_code) && exit_code == 0, id,
        "shutdown cancels and reaps cleanup within grace");
    ok &= check(session.stderr_text().find("fixture cleanup cancelled") != std::string::npos, id,
        "the handler receives shutdown cancellation after its terminal result");
    return ok;
}

bool test_adapter_runtime_payload_lifetime()
{
    const char* id = __func__;
    if (!fixtures_available(id)) {
        return false;
    }

    for (const char* mode : {"success", "failure", "incomplete"}) {
        addon::Host_simulator session(options_for(""), nullptr);
        addon::Contract_failure failure;
        SDK_CHECK(session.start(&failure), "payload fixture starts: " + failure.reason_code);

        addon::Json_value payload = addon::Json_value::object();
        payload.set("mode", addon::Json_value(mode));
        payload.set("blob", addon::Json_value(std::string(768 * 1024, 'p')));
        const auto outcome = probe(session, "payload_lifetime", std::move(payload));
        const bool success = std::string_view(mode) == "success";
        const char* reason = success ? nullptr : std::string_view(mode) == "failure"
            ? "payload_probe_failure" : "handler_incomplete";
        SDK_CHECK(outcome_is(outcome, success ? addon::Host_status::SUCCESS : addon::Host_status::FAILED, reason),
            "terminal result preserves its request identity: " + describe(outcome));

        // Keep stdin open and send no subsequent frame. A completed invocation
        // must release its input even while the runtime waits for host input.
        const std::string marker = "fixture payload released with identity retained";
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(k_probe_deadline_ms);
        while (session.stderr_text().find(marker) == std::string::npos &&
            std::chrono::steady_clock::now() < deadline)
        {
            session.pump(10);
        }
        SDK_CHECK(session.stderr_text().find("fixture payload available through handler cleanup") != std::string::npos,
            "a terminal result does not invalidate input still used by the handler");
        SDK_CHECK(session.stderr_text().find(marker) != std::string::npos,
            "completed input is released before TLS completion without another host frame; stderr=" + session.stderr_text());

        int exit_code = -1;
        SDK_CHECK(session.shutdown(addon::limits::k_shutdown_grace_ms, &exit_code) && exit_code == 0,
            "the payload probe still retires normally");
    }
    return true;
}

bool test_fixture_package_file_variants()
{
    bool ok = true;
    const char* id = __func__;
    if (!fixtures_available(id)) {
        return false;
    }

    const struct
    {
        const char* suffix;
        const char* reason;
    }
    variants[] = {
        {"-invalid-bootstrap",             addon::reason::k_invalid_bootstrap},
        {"-reserved-environment",          addon::reason::k_reserved_environment_variable},
        {"-escaped-path",                  addon::reason::k_path_escape},
        {"-invalid-utf8-bootstrap",        addon::reason::k_invalid_utf8},
        {"-digest-mismatch",               addon::reason::k_digest_mismatch},
        {"-unsupported-core-generation",   addon::reason::k_unsupported_core_generation},
        {"-third-party-ok",                "ok"},
        {"-third-party-missing-notice",    addon::reason::k_third_party_record_invalid},
        {"-third-party-empty-notice",      addon::reason::k_third_party_record_invalid},
        {"-third-party-escaped-notice",    addon::reason::k_third_party_record_invalid},
        {"-third-party-unindexed-notice",  addon::reason::k_third_party_record_invalid},
        {"-third-party-digest-mismatch",   addon::reason::k_third_party_record_invalid},
    };
    for (const auto& variant : variants) {
        const std::string reason = validate_reason(variant.suffix);
        ok &= check(reason == variant.reason, id, std::string(variant.suffix) + ": expected " + variant.reason + ", got " + reason);
    }

    // An invalid package never starts and every invocation answers locally.
    addon::Host_simulator session(options_for("-invalid-bootstrap"), nullptr);
    addon::Contract_failure failure;
    ok &= check(!session.start(&failure) && failure.reason_code == addon::reason::k_invalid_bootstrap, id, "invalid package does not start");
    ok &= check(session.state() == addon::Simulator_state::RETIRED, id, "invalid package is retired");
    const std::string request = session.invoke(k_probe, "echo", addon::Json_value::object(), 1000);
    ok &= check(outcome_is(session.outcome(request), addon::Host_status::PACKAGE_UNAVAILABLE, addon::reason::k_invalid_bootstrap), id,
        "invocation on an invalid package is package_unavailable with the validation reason");
    return ok;
}

bool test_fixture_paced_progress_lifetime()
{
    bool ok = true;
    const char* id = __func__;
    if (!fixtures_available(id)) {
        return false;
    }
    addon::Contract_failure failure;

    // The adapter runtime must keep delivering beyond the former lifetime
    // limit; unpaced raw frames exercise the host tracker separately.
    addon::Host_simulator session(options_for(""), nullptr);
    SDK_CHECK(session.start(&failure), "long paced progress fixture starts");
    addon::Json_value payload = addon::Json_value::object();
    payload.set("count", addon::Json_value(4608));
    payload.set("interval_ms", addon::Json_value(12));
    const std::string request = session.invoke(k_probe, "progress_then_wait", std::move(payload), 110000);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(100);
    while (std::chrono::steady_clock::now() < deadline) {
        session.pump(20);
        const auto recent = session.progress(request);
        if ((!recent.empty() && recent.back().completed == 4608) ||
            session.state() != addon::Simulator_state::READY)
        {
            break;
        }
    }
    const auto recent = session.progress(request);
    ok &= check(session.progress_count(request) > 4096, id,
        "runtime delivers beyond the former lifetime limit: " + std::to_string(session.progress_count(request)));
    ok &= check(recent.size() == addon::Host_simulator::k_retained_progress_frames &&
        recent.back().completed == 4608, id, "paced delivery retains only the latest observation window");
    session.cancel(request);
    ok &= check(outcome_is(session.wait_outcome(request, 5000), addon::Host_status::CANCELLED), id,
        "runtime remains cancellable after more than 4096 delivered frames");
    const auto settled_count = session.progress_count(request);
    session.pump(50);
    ok &= check(session.progress_count(request) == settled_count && session.state() == addon::Simulator_state::READY,
        id, "terminal result excludes later progress and leaves the generation usable");
    return ok;
}

bool test_fixture_behavior_variants()
{
    bool ok = true;
    const char* id = __func__;
    if (!fixtures_available(id)) {
        return false;
    }
    addon::Contract_failure failure;

    // Handshake failures.
    for (const char* suffix : {"-wrong-nonce-hello", "-wrong-core-generation"}) {
        addon::Host_simulator session(options_for(suffix), nullptr);
        ok &= check(!session.start(&failure) && failure.reason_code == addon::reason::k_identity_mismatch, id,
            std::string(suffix) + ": identity_mismatch, got " + failure.reason_code);
        ok &= check(session.state() == addon::Simulator_state::RETIRED, id, std::string(suffix) + ": retired");
    }
    {
        addon::Simulator_options options = options_for("-hang-on-hello");
        options.hello_deadline_ms = 800;
        addon::Host_simulator session(options, nullptr);
        ok &= check(!session.start(&failure) && failure.reason_code == addon::reason::k_hello_timeout, id,
            "hang-on-hello: hello_timeout, got " + failure.reason_code);
    }

    // Sidecar and handshake disagreements are capability-local.
    {
        addon::Host_simulator session(options_for("-sidecar-without-handshake"), nullptr);
        SDK_CHECK(session.start(&failure), "sidecar-without-handshake starts: " + failure.reason_code);
        const std::vector<addon::Capability_pair> live = session.live_capabilities();
        ok &= check(std::find(live.begin(), live.end(), k_profile_storage) == live.end() &&
            std::find(live.begin(), live.end(), k_probe) != live.end(), id, "only the probe is live");
        const std::string request = session.invoke(k_profile_storage, "acknowledge", addon::Json_value::object(), 1000);
        ok &= check(outcome_is(session.outcome(request), addon::Host_status::UNKNOWN_CAPABILITY, addon::reason::k_sidecar_handshake_disagreement), id,
            "sidecar without handshake: sidecar_handshake_disagreement");
        ok &= check(outcome_is(probe(session, "echo", addon::Json_value::object()), addon::Host_status::SUCCESS), id, "the package stays usable");
    }
    {
        addon::Host_simulator session(options_for("-handshake-without-sidecar"), nullptr);
        SDK_CHECK(session.start(&failure), "handshake-without-sidecar starts: " + failure.reason_code);
        ok &= check(session.session()->handshake_declares({"org.example.extra", 1}), id, "handshake declares the extra pair");
        const std::vector<addon::Capability_pair> live = session.live_capabilities();
        ok &= check(std::find(live.begin(), live.end(), addon::Capability_pair{"org.example.extra", 1}) == live.end(), id,
            "an adapter-only capability is ignored");
        const std::string request = session.invoke({"org.example.extra", 1}, "noop", addon::Json_value::object(), 1000);
        ok &= check(outcome_is(session.outcome(request), addon::Host_status::UNKNOWN_CAPABILITY, addon::reason::k_unknown_capability), id,
            "adapter-only capability: unknown_capability");
    }

    // Protocol violations retire the generation atomically.
    const struct
    {
        const char* suffix;
        const char* reason;
    }
    violations[] = {
        {"-stdout-garbage",     addon::reason::k_frame_violation},
        {"-oversize-frame",     addon::reason::k_oversize_frame},
        {"-invalid-utf8-frame", addon::reason::k_invalid_utf8},
    };
    for (const auto& violation : violations) {
        addon::Host_simulator session(options_for(violation.suffix), nullptr);
        SDK_CHECK(session.start(&failure), std::string(violation.suffix) + " starts: " + failure.reason_code);
        const std::optional<addon::Host_outcome> outcome = probe(session, "echo", addon::Json_value::object());
        ok &= check(outcome_is(outcome, addon::Host_status::GENERATION_RETIRED, violation.reason), id,
            std::string(violation.suffix) + ": expected generation_retired/" + violation.reason + ", got " + describe(outcome));
        ok &= check(session.state() == addon::Simulator_state::RETIRED && session.retirement().reason_code == violation.reason, id,
            std::string(violation.suffix) + ": retired with the violation");
        const std::string after = session.invoke(k_probe, "echo", addon::Json_value::object(), 1000);
        ok &= check(outcome_is(session.outcome(after), addon::Host_status::PACKAGE_UNAVAILABLE, violation.reason), id,
            std::string(violation.suffix) + ": quarantined afterwards");
    }
    {
        addon::Host_simulator session(options_for("-double-result"), nullptr);
        SDK_CHECK(session.start(&failure), "double-result starts");
        const std::optional<addon::Host_outcome> outcome = probe(session, "echo", addon::Json_value::object());
        ok &= check(outcome_is(outcome, addon::Host_status::SUCCESS), id, "first result is delivered: " + describe(outcome));
        session.pump(300);
        ok &= check(session.state() == addon::Simulator_state::RETIRED && session.retirement().reason_code == addon::reason::k_duplicate_terminal_result, id,
            "second result retires the generation");
    }

    const auto start_waiting_request = [](addon::Host_simulator& session) {
        addon::Json_value payload = addon::Json_value::object();
        payload.set("count", addon::Json_value(1));
        payload.set("interval_ms", addon::Json_value(0));
        const auto request = session.invoke(k_probe, "progress_then_wait", std::move(payload), 30000);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(k_probe_deadline_ms);
        while (session.progress_count(request) == 0 && std::chrono::steady_clock::now() < deadline) {
            session.pump(10);
        }
        return request;
    };
    {
        addon::Host_simulator session(options_for(""), nullptr);
        SDK_CHECK(session.start(&failure), "handler exception fixture starts");
        const auto other = start_waiting_request(session);
        SDK_CHECK(session.progress_count(other) == 1, "a concurrent request is active");
        const auto fatal = probe(session, "handler_runtime_error", addon::Json_value::object());
        ok &= check(outcome_is(fatal, addon::Host_status::GENERATION_RETIRED, addon::reason::k_adapter_exited) &&
            outcome_is(session.wait_outcome(other, k_probe_deadline_ms), addon::Host_status::GENERATION_RETIRED,
                addon::reason::k_adapter_exited), id, "unexpected handler exception retires both outstanding operations");
        session.shutdown(1000, nullptr);   // drain remaining stderr
        ok &= check(session.exit_code() == EXIT_FAILURE &&
            session.stderr_text().find("Unhandled exception in the invocation handler") != std::string::npos &&
            session.stderr_text().find("fixture private exception text") == std::string::npos,
            id, "unexpected exception produces a fixed diagnostic and a failing process exit");
    }

    // The runtime turns an unsendable terminal payload into this request's
    // bounded error result; it never emits a malformed frame that retires the
    // generation. Diagnostic clipping preserves valid UTF-8 by scalar.
    {
        addon::Host_simulator session(options_for(""), nullptr);
        SDK_CHECK(session.start(&failure), "egress checks fixture starts: " + failure.reason_code);
        const auto other = start_waiting_request(session);
        SDK_CHECK(session.progress_count(other) == 1, "a concurrent request is active");
        const struct { const char* operation; const char* reason; } invalid_calls[] = {
            {"scalar_success",          "handler_invalid_result"},
            {"invalid_utf8_success",    addon::reason::k_invalid_utf8},
            {"progress_above_total",    "handler_invalid_progress"},
            {"progress_count_overflow", "handler_invalid_progress"},
            {"progress_total_overflow", "handler_invalid_progress"},
            {"invalid_launch_patch",    "handler_invalid_argument"},
        };
        for (const auto& call : invalid_calls) {
            const auto request = session.invoke(k_probe, call.operation, addon::Json_value::object(), k_probe_deadline_ms);
            const auto rejected = session.wait_outcome(request, k_probe_deadline_ms);
            ok &= check(outcome_is(rejected, addon::Host_status::FAILED, call.reason) &&
                session.progress_count(request) == 0, id, std::string(call.operation) + ": " + describe(rejected));
            ok &= check(outcome_is(probe(session, "echo", addon::Json_value::object()), addon::Host_status::SUCCESS),
                id, std::string("subsequent invoke after ") + call.operation);
        }
        const auto boundary = session.invoke(k_probe, "progress_maximum", addon::Json_value::object(), k_probe_deadline_ms);
        const auto accepted = session.wait_outcome(boundary, k_probe_deadline_ms);
        const auto progress_at_limit = session.progress(boundary);
        constexpr auto maximum = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
        ok &= check(outcome_is(accepted, addon::Host_status::SUCCESS) && progress_at_limit.size() == 1 &&
            progress_at_limit.front().completed == maximum && progress_at_limit.front().total == maximum,
            id, "largest representable progress count and total cross the wire");
        ok &= check(outcome_is(probe(session, "invalid_patch_after_success", addon::Json_value::object()),
            addon::Host_status::SUCCESS), id, "a validation exception after success preserves the first result");
        session.cancel(other);
        ok &= check(outcome_is(session.wait_outcome(other, k_probe_deadline_ms), addon::Host_status::CANCELLED), id,
            "a concurrent request remains usable across handler validation failures");
        const std::optional<addon::Host_outcome> oversized =
            probe(session, "oversize_success", addon::Json_value::object());
        ok &= check(outcome_is(oversized, addon::Host_status::FAILED, addon::reason::k_payload_too_large), id,
            "oversized success becomes a request-local payload_too_large result: " + describe(oversized));
        ok &= check(session.state() == addon::Simulator_state::READY, id,
            "oversized success leaves the generation ready");
        ok &= check(outcome_is(probe(session, "echo", addon::Json_value::object()), addon::Host_status::SUCCESS), id,
            "a request after oversized success remains usable");

        const std::optional<addon::Host_outcome> unicode_failure =
            probe(session, "unicode_failure", addon::Json_value::object());
        ok &= check(outcome_is(unicode_failure, addon::Host_status::FAILED, "diagnostic") &&
                addon::is_valid_utf8(unicode_failure->message) &&
                unicode_failure->message.size() <= addon::limits::k_max_result_message_bytes,
            id, "result diagnostic clipping preserves valid bounded UTF-8");
        const std::optional<addon::Host_outcome> invalid_diagnostic =
            probe(session, "invalid_diagnostic", addon::Json_value::object());
        ok &= check(outcome_is(invalid_diagnostic, addon::Host_status::FAILED, "diagnostic") &&
                invalid_diagnostic->message == "invalid UTF-8 diagnostic", id,
            "invalid result diagnostic becomes fixed safe text");

        const std::string progress_request =
            session.invoke(k_probe, "unicode_progress", addon::Json_value::object(), k_probe_deadline_ms);
        const std::optional<addon::Host_outcome> progress_outcome =
            session.wait_outcome(progress_request, k_probe_deadline_ms);
        const std::vector<addon::Progress_message> progress = session.progress(progress_request);
        ok &= check(outcome_is(progress_outcome, addon::Host_status::SUCCESS) && progress.size() == 1 &&
                addon::is_valid_utf8(progress.front().note) &&
                progress.front().note.size() <= addon::limits::k_max_progress_note_bytes,
            id, "progress diagnostic clipping preserves valid bounded UTF-8");
    }

    // The host observes the complete stream while its diagnostic history
    // stays bounded. The adapter still settles the same request on cancel.
    {
        addon::Host_simulator session(options_for(""), nullptr);
        SDK_CHECK(session.start(&failure), "continuous progress fixture starts");
        const std::string request = session.invoke(k_probe, "raw_progress_then_wait",
            addon::Json_value::object(), 30000);
        for (int waited = 0; waited < 20000 && session.progress_count(request) < 8192; waited += 20) {
            session.pump(20);
        }
        ok &= check(session.progress_count(request) == 8192, id, "all continuous progress frames reach the host");
        const auto recent = session.progress(request);
        ok &= check(recent.size() == addon::Host_simulator::k_retained_progress_frames &&
            recent.back().completed == 8192, id, "only the latest progress window is retained");
        session.cancel(request);
        ok &= check(outcome_is(session.wait_outcome(request, 5000), addon::Host_status::CANCELLED), id,
            "a long progress stream remains cancellable");
        session.pump(50);
        ok &= check(session.progress_count(request) == 8192 && session.state() == addon::Simulator_state::READY,
            id, "cancellation settles without late progress or retirement");
    }
    // A fast producer's final update is flushed after coalescing, without
    // retaining its preceding reports or delaying terminal cancellation.
    {
        addon::Host_simulator session(options_for(""), nullptr);
        SDK_CHECK(session.start(&failure), "coalesced progress fixture starts");
        addon::Json_value payload = addon::Json_value::object();
        payload.set("count", addon::Json_value(8192));
        payload.set("interval_ms", addon::Json_value(0));
        const std::string request = session.invoke(k_probe, "progress_then_wait", std::move(payload), 10000);
        for (int waited = 0; waited < 5000; waited += 20) {
            session.pump(20);
            const auto recent = session.progress(request);
            if (!recent.empty() && recent.back().completed == 8192) {
                break;
            }
        }
        const auto recent = session.progress(request);
        ok &= check(!recent.empty() && recent.back().completed == 8192, id,
            "the latest coalesced update reaches the host even after the producer becomes quiet");
        ok &= check(recent.size() <= addon::Host_simulator::k_retained_progress_frames &&
            session.progress_count(request) < 8192, id, "fast progress coalesces within bounded retention");
        session.cancel(request);
        ok &= check(outcome_is(session.wait_outcome(request, 5000), addon::Host_status::CANCELLED), id,
            "coalesced progress request cancels");
    }

    // Progress is paced, never dropped silently. With the bound lowered to
    // 128, a handler that reports 300 times over a few seconds is still heard
    // as it goes, never past the bound, and its last count reaches the host
    // although nothing follows it until the cancel.
    {
        addon::Simulator_options options = options_for("");
        options.limits.max_progress_frames_per_request = 128;
        addon::Host_simulator session(options, nullptr);
        SDK_CHECK(session.start(&failure), "paced progress fixture starts: " + failure.reason_code);
        addon::Json_value payload = addon::Json_value::object();
        payload.set("count",       addon::Json_value(300));
        payload.set("interval_ms", addon::Json_value(10));
        const std::string request = session.invoke(k_probe, "progress_then_wait", std::move(payload), 60000);
        for (int waited = 0; waited < 20000; waited += 50) {
            const std::vector<addon::Progress_message> progress = session.progress(request);
            if (!progress.empty() && progress.back().completed == 300) {
                break;
            }
            session.pump(50);
        }
        const std::vector<addon::Progress_message> progress = session.progress(request);
        ok &= check(!progress.empty() && progress.back().completed == 300, id,
            "the last reported count reaches the host: " +
                (progress.empty() ? std::string("no progress") : std::to_string(progress.back().completed)));
        ok &= check(progress.size() > 16 && progress.size() <= 128, id,
            "paced progress stays within the bound: " + std::to_string(progress.size()) + " frames");
        ok &= check(session.state() == addon::Simulator_state::READY, id, "paced progress violates nothing");
        session.cancel(request);
        ok &= check(outcome_is(session.wait_outcome(request, 5000), addon::Host_status::CANCELLED), id,
            "the waiting request answers its cancel");
    }

    // A request that does spend its whole bound says so on stderr.
    {
        addon::Simulator_options options = options_for("");
        options.limits.max_progress_frames_per_request = 1;
        addon::Host_simulator session(options, nullptr);
        SDK_CHECK(session.start(&failure), "exhausted progress fixture starts: " + failure.reason_code);
        addon::Json_value payload = addon::Json_value::object();
        payload.set("count", addon::Json_value(3));
        const std::string request = session.invoke(k_probe, "progress", std::move(payload), k_probe_deadline_ms);
        ok &= check(outcome_is(session.wait_outcome(request, k_probe_deadline_ms), addon::Host_status::SUCCESS), id,
            "a request past its progress bound still succeeds");
        session.pump(200);
        ok &= check(session.progress(request).size() == 1, id, "the bound holds");
        ok &= check(session.stderr_text().find("progress frames it may send") != std::string::npos, id,
            "the spent bound is reported on stderr: " + session.stderr_text());
    }

    // stderr overflow is a diagnostic, never a failure.
    {
        addon::Host_simulator session(options_for("-stderr-flood"), nullptr);
        SDK_CHECK(session.start(&failure), "stderr-flood starts");
        const std::optional<addon::Host_outcome> outcome = probe(session, "echo", addon::Json_value::object());
        session.pump(200);
        ok &= check(outcome_is(outcome, addon::Host_status::SUCCESS), id, "stderr flood: request succeeds: " + describe(outcome));
        ok &= check(session.stderr_truncated() && session.stderr_text().rfind(addon::limits::k_stderr_truncation_marker, 0) == 0, id,
            "stderr ring truncated with the marker");
        ok &= check(session.state() == addon::Simulator_state::READY, id, "stderr flood leaves the generation ready");
    }

    // Both directions may exceed a kernel pipe at once. The host must keep
    // draining progress while a large invoke is back-pressured, then enforce
    // the ordinary deadline/cancel-grace retirement without wedging.
    {
        addon::Host_simulator session(options_for("-bidirectional-flood"), nullptr);
        SDK_CHECK(session.start(&failure), "bidirectional-flood starts");
        const std::string flood = session.invoke(k_probe, "flood", addon::Json_value::object(), 300);
        addon::Json_value large = addon::Json_value::object();
        large.set("blob", addon::Json_value(std::string(512u * 1024u, 'i')));
        (void)session.invoke(k_probe, "echo", std::move(large), 5000);
        const std::optional<addon::Host_outcome> outcome =
            session.wait_outcome(flood, 300 + addon::limits::k_cancel_grace_ms + 3000);
        ok &= check(outcome_is(outcome, addon::Host_status::TIMED_OUT, addon::reason::k_cancel_grace_expired), id,
            "simultaneous large invoke/output flood retires on its deadline: " + describe(outcome) +
            "; retirement=" + session.retirement().reason_code + "/" + session.retirement().detail +
            "; stderr=" + session.stderr_text());
        ok &= check(!session.progress(flood).empty(), id, "the opposing output flood was observed");
    }

    // stdin_queue_overflow names only exhaustion of the actual byte-bounded
    // queue. A ready adapter that deliberately stops reading deterministically
    // reaches it; an ordinary slow write does not synthesize the reason.
    {
        addon::Host_simulator session(options_for("-stdin-stall"), nullptr);
        SDK_CHECK(session.start(&failure), "stdin-stall starts");
        std::string first;
        for (int i = 0; i < 32 && session.state() == addon::Simulator_state::READY; ++i) {
            addon::Json_value payload = addon::Json_value::object();
            payload.set("blob", addon::Json_value(std::string(768u * 1024u, 'q')));
            const std::string request = session.invoke(k_probe, "echo", std::move(payload), 60000);
            if (first.empty()) {
                first = request;
            }
        }
        ok &= check(session.state() == addon::Simulator_state::RETIRED &&
            session.retirement().reason_code == addon::reason::k_stdin_queue_overflow,
            id, "the actual queued-byte bound retires stdin-stall with stdin_queue_overflow");
        ok &= check(outcome_is(session.outcome(first), addon::Host_status::GENERATION_RETIRED,
            addon::reason::k_stdin_queue_overflow), id, "queued requests fail with the same canonical reason");
    }

    // Crash and hang isolation.
    {
        addon::Host_simulator session(options_for("-crash-on-invoke"), nullptr);
        SDK_CHECK(session.start(&failure), "crash-on-invoke starts");
        const std::optional<addon::Host_outcome> outcome = probe(session, "echo", addon::Json_value::object());
        ok &= check(outcome_is(outcome, addon::Host_status::GENERATION_RETIRED, addon::reason::k_adapter_exited), id,
            "crash: generation_retired/adapter_exited, got " + describe(outcome));
        ok &= check(session.adapter_exited(), id, "crash: exit observed");
    }
    {
        addon::Host_simulator session(options_for("-hang-on-invoke"), nullptr);
        SDK_CHECK(session.start(&failure), "hang-on-invoke starts");
        const std::string request = session.invoke(k_probe, "echo", addon::Json_value::object(), 300);
        const std::optional<addon::Host_outcome> outcome = session.wait_outcome(request, 300 + addon::limits::k_cancel_grace_ms + 1000);
        ok &= check(outcome_is(outcome, addon::Host_status::TIMED_OUT, addon::reason::k_cancel_grace_expired), id,
            "hang: timed_out/cancel_grace_expired, got " + describe(outcome));
        ok &= check(session.state() == addon::Simulator_state::RETIRED && session.retirement().reason_code == addon::reason::k_cancel_grace_expired, id,
            "hang: generation retired after the cancel grace");
    }
    {
        addon::Host_simulator session(options_for("-ignores-cancel"), nullptr);
        SDK_CHECK(session.start(&failure), "ignores-cancel starts");
        addon::Json_value payload = addon::Json_value::object();
        payload.set("ms", addon::Json_value(60000));
        const std::string request = session.invoke(k_probe, "sleep", payload, 60000);
        session.pump(100);
        session.cancel(request);
        const std::optional<addon::Host_outcome> outcome = session.wait_outcome(request, addon::limits::k_cancel_grace_ms + 1000);
        ok &= check(outcome_is(outcome, addon::Host_status::TIMED_OUT, addon::reason::k_cancel_grace_expired), id,
            "ignores-cancel: timed_out/cancel_grace_expired, got " + describe(outcome));
    }
    {
        addon::Host_simulator session(options_for("-slow-result"), nullptr);
        SDK_CHECK(session.start(&failure), "slow-result starts");
        const std::optional<addon::Host_outcome> outcome = probe(session, "echo", addon::Json_value::object(), 4000);
        ok &= check(outcome_is(outcome, addon::Host_status::SUCCESS), id, "slow-result answers within a long deadline: " + describe(outcome));
    }
    {
        addon::Host_simulator session(options_for("-slow-shutdown"), nullptr);
        SDK_CHECK(session.start(&failure), "slow-shutdown starts");
        int exit_code = -1;
        ok &= check(!session.shutdown(500, &exit_code), id, "slow-shutdown does not exit within the grace");
        ok &= check(session.adapter_exited(), id, "slow-shutdown was terminated");
    }

    // profile-storage@1 hard failures.
    const struct
    {
        const char* suffix;
        const char* reason;
    }
    profile_variants[] = {
        {"-profile-omit-echo",     addon::reason::k_profile_storage_unacknowledged},
        {"-profile-wrong-root",    addon::reason::k_profile_storage_mismatch},
        {"-profile-default-dir",   addon::reason::k_profile_storage_mismatch},
        {"-profile-wrong-version", addon::reason::k_profile_storage_version_mismatch},
    };
    for (const auto& variant : profile_variants) {
        addon::Host_simulator session(options_for(variant.suffix), nullptr);
        SDK_CHECK(session.start(&failure), std::string(variant.suffix) + " starts");
        std::string error;
        const std::optional<std::string> root = addon::allocate_profile_storage_root(
            session.profile_storage_root(), session.package()->bootstrap.package_id, &error);
        SDK_CHECK(root.has_value(), "root allocated: " + error);
        addon::Simulator_invocation_options invocation;
        invocation.profile_storage = addon::Profile_storage_context{1, *root};
        const std::string request = session.invoke(k_profile_storage, "acknowledge", addon::Json_value::object(), k_probe_deadline_ms, invocation);
        const std::optional<addon::Host_outcome> outcome = session.wait_outcome(request, k_probe_deadline_ms);
        ok &= check(outcome_is(outcome, addon::Host_status::FAILED, variant.reason), id,
            std::string(variant.suffix) + ": expected error/" + variant.reason + ", got " + describe(outcome));
        ok &= check(session.state() == addon::Simulator_state::READY, id, std::string(variant.suffix) + ": a hard operation failure keeps the generation");
    }

    // Descendants: owned grandchildren stay in the adapter's group; a
    // detached one does not.
    {
        addon::Host_simulator session(options_for("-orphan-grandchild"), nullptr);
        SDK_CHECK(session.start(&failure), "orphan-grandchild starts");
        addon::Json_value payload = addon::Json_value::object();
        payload.set("ms", addon::Json_value(5000));
        const std::optional<addon::Host_outcome> outcome = probe(session, "spawn_child", payload);
        SDK_CHECK(outcome_is(outcome, addon::Host_status::SUCCESS), "spawn_child: " + describe(outcome));
        const std::int64_t child = outcome->payload.find("child_pid")->as_integer();
        ok &= check(session.owns_process(child), id, "orphan grandchild is in the adapter's process group");
        ok &= check(!outcome->payload.find("detached")->as_boolean(), id, "orphan grandchild did not detach");
        session.shutdown(addon::limits::k_shutdown_grace_ms, nullptr);
        addon::kill_process(child);
    }
    {
        addon::Host_simulator session(options_for("-detach-grandchild"), nullptr);
        SDK_CHECK(session.start(&failure), "detach-grandchild starts");
        addon::Json_value payload = addon::Json_value::object();
        payload.set("ms", addon::Json_value(5000));
        const std::optional<addon::Host_outcome> outcome = probe(session, "spawn_child", payload);
        SDK_CHECK(outcome_is(outcome, addon::Host_status::SUCCESS), "spawn_child (detach): " + describe(outcome));
        const std::int64_t child = outcome->payload.find("child_pid")->as_integer();
        ok &= check(outcome->payload.find("detached")->as_boolean(), id, "detach fixture reports its intent");
#if !defined(_WIN32)
        ok &= check(!session.owns_process(child), id, "detached grandchild left the adapter's process group");
#endif
        session.shutdown(addon::limits::k_shutdown_grace_ms, nullptr);
        addon::kill_process(child);
    }

    // The provider-session behaviours, their helper and their variants.
    ok &= check_provider_session_surfaces(id);
    ok &= check_provider_session_helper(id);
    ok &= check_provider_session_continuous(id);
    ok &= check_classify_hang_variant(id);
    ok &= check_resume_foreign_digest_variant(id);
    return ok;
}

bool test_fixture_transcripts()
{
    bool ok = true;
    const char* id = __func__;
    if (!fixtures_available(id)) {
        return false;
    }
    SDK_CHECK(!scripts_directory().empty(), "transcript scripts are required (--scripts <dir>)");

    const auto run = [](const std::string& suffix, const std::string& script_name, addon::Transcript_recorder& recorder) {
        const std::optional<addon::Json_value> script = load_script(script_name);
        if (!script.has_value()) {
            return false;
        }
        addon::Host_simulator session(options_for(suffix), &recorder);
        addon::Contract_failure failure;
        session.start(&failure);
        std::string error;
        return session.run_script(*script, &error);
    };

    // acknowledge.json on the positive package, twice: byte-equal after
    // normalization although nonce and root differ between the runs.
    {
        addon::Transcript_recorder first;
        addon::Transcript_recorder second;
        ok &= check(run("", "acknowledge.json", first) && run("", "acknowledge.json", second), id, "acknowledge script runs");
        ok &= check(has_line_containing(first.lines(), "! profile_storage acknowledged"), id, "acknowledge: acknowledgment event");
        ok &= check(has_line_containing(first.lines(), R"("status":"success")"), id, "acknowledge: success result");
        ok &= check(has_line_containing(first.lines(), R"("root":"<root>")"), id, "acknowledge: root normalized");
        ok &= check(has_line_containing(first.lines(), R"("generation_nonce":"<nonce>")"), id, "acknowledge: nonce normalized");
        ok &= check(!first.lines().empty() && first.lines().back() == "! adapter_exited exit_code=0", id, "acknowledge: clean exit last");
        addon::Transcript_difference difference;
        ok &= check(addon::compare_transcripts(first.lines(), second.lines(), &difference), id,
            "two runs agree after normalization (line " + std::to_string(difference.line) + ": " + difference.expected + " vs " + difference.actual + ")");
    }
    // crash.json: the positive package succeeds, the crashing one exits
    // during the request.
    {
        addon::Transcript_recorder positive;
        addon::Transcript_recorder crash;
        ok &= check(run("", "crash.json", positive) && run("-crash-on-invoke", "crash.json", crash), id, "crash script runs");
        ok &= check(has_line_containing(positive.lines(), R"("marker":"crash-script")") && has_line_containing(positive.lines(), R"("status":"success")"), id,
            "crash script on the positive package echoes");
        ok &= check(!has_line_containing(crash.lines(), "< {") || !has_line_containing(crash.lines(), R"("type":"result")"), id,
            "crashing adapter sends no result");
#if defined(_WIN32)
        ok &= check(has_line_containing(crash.lines(), "! adapter_exited exit_code=3"), id, "crash exit recorded");
#else
        ok &= check(has_line_containing(crash.lines(), "! adapter_exited signal=6"), id, "crash signal recorded");
#endif
        ok &= check(has_line_containing(crash.lines(), "! retired adapter_exited"), id, "crash retires the generation");
#if !defined(_WIN32)
        ok &= check(ends_with_lines(crash.lines(), {"! adapter_exited signal=6", "! retired adapter_exited"}), id,
            "an unrequested exit records the exit and then the retirement");
#endif
    }
    // cancel_during_hang.json: a conforming adapter answers cancelled, a
    // hanging one exhausts the cancel grace.
    {
        addon::Transcript_recorder positive;
        addon::Transcript_recorder hang;
        ok &= check(run("", "cancel_during_hang.json", positive) && run("-hang-on-invoke", "cancel_during_hang.json", hang), id,
            "cancel script runs");
        ok &= check(has_line_containing(positive.lines(), R"(> {"request_id":"<r#1>","type":"cancel"})"), id, "cancel frame recorded");
        ok &= check(has_line_containing(positive.lines(), R"("status":"cancelled")"), id, "conforming adapter answers cancelled");
        ok &= check(has_line_containing(hang.lines(), "! cancel_grace_expired <r#1>") && has_line_containing(hang.lines(), "! retired cancel_grace_expired"), id,
            "hanging adapter exhausts the cancel grace");
        ok &= check(has_line_containing(hang.lines(), "! killed"), id, "hanging adapter is killed");
#if !defined(_WIN32)
        ok &= check(ends_with_lines(hang.lines(),
                {"! cancel_grace_expired <r#1>", "! retired cancel_grace_expired", "! killed", "! adapter_exited signal=9"}), id,
            "grace expiry records expiry, retirement, kill and the killed exit in that order");
#endif
    }
    // double_result.json: the duplicate is a violation.
    {
        addon::Transcript_recorder duplicate;
        ok &= check(run("-double-result", "double_result.json", duplicate), id, "double result script runs");
        ok &= check(has_line_containing(duplicate.lines(), "! violation duplicate_terminal_result") &&
            has_line_containing(duplicate.lines(), "! retired duplicate_terminal_result"), id, "duplicate terminal result retires the generation");
#if !defined(_WIN32)
        ok &= check(ends_with_lines(duplicate.lines(),
                {"! violation duplicate_terminal_result", "! retired duplicate_terminal_result", "! killed", "! adapter_exited signal=9"}), id,
            "a violation records violation, retirement, kill and the killed exit in that order");
#endif
    }
    // shutdown.json: hello, shutdown, exit.
    {
        addon::Transcript_recorder shutdown;
        ok &= check(run("", "shutdown.json", shutdown), id, "shutdown script runs");
        const std::vector<std::string>& lines = shutdown.lines();
        ok &= check(lines.size() == 5 &&
            lines[0].rfind("> {", 0) == 0 && lines[0].find(R"("type":"hello")") != std::string::npos &&
            lines[1].rfind("< {", 0) == 0 && lines[1].find(R"("type":"hello")") != std::string::npos &&
            lines[2].find(R"("type":"capabilities")") != std::string::npos &&
            lines[3] == R"(> {"grace_ms":5000,"type":"shutdown"})" &&
            lines[4] == "! adapter_exited exit_code=0", id, "shutdown transcript has exactly hello, hello, capabilities, shutdown, exit");
    }
    // The provider-session scripts, twice each: byte-equal after
    // normalization, a success result, the answer the script provokes, and
    // a clean exit last.
    {
        const struct
        {
            const char* script;
            const char* suffix;
            const char* marker;
        }
        session_scripts[] = {
            {"structured_prepare_inspect.json", k_sessions_suffix,            R"("complete":true)"},
            {"delegated_prepare.json",          k_sessions_suffix,            R"("server_ids":["coordination"])"},
            {"resume_prepare.json",             k_sessions_suffix,            R"("reason":"resume_record_invalid")"},
            {"classify_events.json",            k_sessions_suffix,            R"("classification":"transition")"},
            {"observe_poll.json",               k_sessions_continuous_suffix, R"("more":false)"},
        };
        for (const auto& entry : session_scripts) {
            const std::string script = entry.script;
            addon::Transcript_recorder first;
            addon::Transcript_recorder second;
            ok &= check(run(entry.suffix, script, first) && run(entry.suffix, script, second), id, script + " runs");
            ok &= check(has_line_containing(first.lines(), R"("status":"success")"), id, script + ": a success result");
            ok &= check(has_line_containing(first.lines(), entry.marker), id, script + ": " + entry.marker);
            ok &= check(!first.lines().empty() && first.lines().back() == "! adapter_exited exit_code=0", id,
                script + ": clean exit last");
            addon::Transcript_difference difference;
            ok &= check(addon::compare_transcripts(first.lines(), second.lines(), &difference), id,
                script + ": two runs agree after normalization (line " + std::to_string(difference.line) + ": " +
                difference.expected + " vs " + difference.actual + ")");
        }
    }
    return ok;
}

} // namespace sdk_tests
