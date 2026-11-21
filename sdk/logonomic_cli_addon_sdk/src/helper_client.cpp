#include "logonomic_cli_addon/helper_client.h"
#include "logonomic_cli_addon/capability_ids.h"
#include "logonomic_cli_addon/host_simulator.h"
#include "logonomic_cli_addon/limits.h"
#include "simulator_process.h"
#include "strict_json.h"

#include <algorithm>
#include <chrono>
#include <memory>
#include <utility>

namespace logonomic::cli_addon {

namespace {

using Clock = std::chrono::steady_clock;

// Bytes the wrapper may still hand over after its exit is observed.
constexpr int k_post_exit_drain_ms = 250;

bool report(Contract_failure* out_failure, const char* reason_code, std::string detail)
{
    if (out_failure != nullptr) {
        out_failure->reason_code = reason_code;
        out_failure->detail      = std::move(detail);
    }
    return false;
}

bool has_nul(std::string_view text) { return text.find('\0') != std::string_view::npos; }

/// One wrapper attempt. Only a completed invocation supplies its own exit code.
struct Wrapper_run
{
    int         exit_code = k_helper_exit_transport_failure;
    std::string stdout_bytes;
    std::optional<Helper_local_diagnostic> local_diagnostic;
};

void reap(process::Child_process& child)
{
    // A deadline requests termination; it cannot prove that termination has
    // completed. Keep ownership until the OS reports and reaps the child, even
    // if scheduling or an uninterruptible kernel operation delays that report.
    while (!child.exited()) {
        (void)child.wait_activity(100, false);
    }
}

Wrapper_run run_wrapper(
    const std::string&              vnmctl_path,
    const std::vector<std::string>& arguments,
    std::string_view                stdin_bytes,
    std::uint64_t                   max_stdout_bytes,
    int                             deadline_ms,
    const std::function<bool()>&     cancellation_requested)
{
    Wrapper_run run;
    if (cancellation_requested && cancellation_requested()) {
        run.local_diagnostic = Helper_local_diagnostic{Helper_local_failure::CANCELLED, "Cancelled before starting the wrapper."};
        return run;
    }
    process::Spawn_request request;
    request.executable = vnmctl_path;
    request.argv.push_back(vnmctl_path);
    request.argv.insert(request.argv.end(), arguments.begin(), arguments.end());
    for (const auto& [name, value] : current_process_environment()) {
        request.environment.push_back(name + "=" + value);
    }
    // A helper's descendants stay in the custodian-owned process group or
    // Job (the no-detach rule): the wrapper is not isolated.
    request.isolate = false;

    std::uint32_t native_error = 0;
    std::unique_ptr<process::Child_process> child = process::Child_process::spawn(request, nullptr, &native_error);
    if (!child) {
        run.local_diagnostic = Helper_local_diagnostic{Helper_local_failure::SPAWN_FAILED,
            native_error ? "Could not start the wrapper, native error " + std::to_string(native_error) + "."
                         : "The wrapper launch arguments or environment could not be prepared."};
        return run;
    }

    const Clock::time_point deadline = Clock::now() + std::chrono::milliseconds(deadline_ms);
    std::size_t written      = 0;
    bool        stdin_closed = false;
    bool        stdout_eof   = false;
    std::optional<process::Exit_status>  exit;
    std::optional<Clock::time_point>     drain_deadline;
    if (stdin_bytes.empty()) {
        child->close_stdin();
        stdin_closed = true;
    }
    while (true) {
        if (!stdin_closed) {
            const process::Write_result result = child->write_stdin(stdin_bytes.substr(written));
            if (result.status == process::Write_status::TRANSFERRED) {
                written += result.bytes;
            }
            // The wrapper stopped reading (CLOSED or FAILED): its exit code
            // says what became of the request.
            if (written == stdin_bytes.size() || result.status == process::Write_status::CLOSED ||
                result.status == process::Write_status::FAILED)
            {
                child->close_stdin();
                stdin_closed = true;
            }
        }
        const Clock::time_point now = Clock::now();
        if (exit.has_value() && (stdout_eof || (drain_deadline.has_value() && now >= *drain_deadline))) {
            break;
        }
        const bool timed_out = now >= deadline;
        if (timed_out || (cancellation_requested && cancellation_requested())) {
            child->kill();
            reap(*child);
            run.local_diagnostic = timed_out
                ? Helper_local_diagnostic{Helper_local_failure::TIMED_OUT,
                      "Wrapper exceeded " + std::to_string(deadline_ms) + " ms."}
                : Helper_local_diagnostic{Helper_local_failure::CANCELLED, "Cancelled while waiting for the wrapper."};
            return run;
        }
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
        const int  timeout   = (int)std::min<long long>(remaining, exit.has_value() ? k_post_exit_drain_ms : 100);
        const process::Activity activity = child->wait_activity(timeout, !stdin_closed);
        if (activity.stdout_bytes.size() > max_stdout_bytes - run.stdout_bytes.size()) {
            child->kill();
            reap(*child);
            run.local_diagnostic = Helper_local_diagnostic{Helper_local_failure::OUTPUT_TOO_LARGE,
                "Wrapper stdout exceeded " + std::to_string(max_stdout_bytes) + " bytes."};
            return run;
        }
        run.stdout_bytes += activity.stdout_bytes;
        if (activity.stdout_eof) {
            stdout_eof = true;
        }
        if (activity.exit.has_value()) {
            exit           = activity.exit;
            drain_deadline = Clock::now() + std::chrono::milliseconds(k_post_exit_drain_ms);
        }
    }
    if (exit->signaled) {
        run.local_diagnostic = Helper_local_diagnostic{Helper_local_failure::ABNORMAL_EXIT,
            "Wrapper terminated with native status " + std::to_string(exit->code) + "."};
    }
    else {
        run.exit_code = exit->code;
        if (run.exit_code != k_helper_exit_committed && run.exit_code != k_helper_exit_pending) {
            run.local_diagnostic = Helper_local_diagnostic{Helper_local_failure::WRAPPER_FAILED,
                "Wrapper returned exit code " + std::to_string(run.exit_code) + "."};
        }
    }
    return run;
}

Provider_event_outcome refused(const char* reason_code, Provider_event_identity identity)
{
    Provider_event_outcome outcome;
    outcome.reason    = reason_code;
    outcome.exit_code = k_helper_exit_invalid_arguments;
    outcome.identity  = std::move(identity);
    outcome.local_diagnostic = Helper_local_diagnostic{Helper_local_failure::INVALID_ARGUMENTS, reason_code};
    return outcome;
}

Title_instruction_result invalid_title_instruction_arguments()
{
    Title_instruction_result result;
    result.exit_code = k_helper_exit_invalid_arguments;
    result.local_diagnostic = Helper_local_diagnostic{Helper_local_failure::INVALID_ARGUMENTS, "Invalid title instruction arguments."};
    return result;
}

Helper_local_diagnostic invalid_wrapper_response(std::size_t bytes)
{
    return {Helper_local_failure::INVALID_RESPONSE,
        "Wrapper returned " + std::to_string(bytes) + " bytes inconsistent with its response contract."};
}

std::optional<Title_instruction_result> parse_title_instruction_result(
    std::string_view text)
{
    Json_parse_limits parse_limits;
    parse_limits.max_bytes = limits::k_max_helper_client_stdout_bytes;
    Json_parse_error parse_error;
    const std::optional<Json_value> document = parse_json(
        text,
        parse_limits,
        &parse_error);
    if (!document.has_value() || !document->is_object() ||
        strict::unexpected_member(
            *document,
            {"accepted", "instruction_required", "instruction", "reason"})
                .has_value())
    {
        return std::nullopt;
    }
    const Json_value* accepted = strict::member_of_type(
        *document,
        "accepted",
        Json_type::BOOLEAN);
    if (accepted == nullptr) {
        return std::nullopt;
    }
    Title_instruction_result result;
    result.completed = true;
    result.accepted = accepted->as_boolean();
    result.exit_code = k_helper_exit_committed;
    if (!result.accepted) {
        const std::string* reason = strict::string_member(*document, "reason");
        if (reason == nullptr || document->find("instruction_required") != nullptr ||
            document->find("instruction") != nullptr)
        {
            return std::nullopt;
        }
        result.reason = *reason;
        return result;
    }
    const Json_value* instruction_required = strict::member_of_type(
        *document,
        "instruction_required",
        Json_type::BOOLEAN);
    const std::string* instruction = strict::string_member(*document, "instruction");
    if (instruction_required == nullptr || instruction == nullptr) {
        return std::nullopt;
    }
    result.instruction_required = instruction_required->as_boolean();
    result.instruction = *instruction;
    return result;
}

// The committed outcome object the wrapper prints:
// {"accepted": bool, "reason"?: string, "epoch"?: integer, "helper_response"?: any}.
std::optional<Provider_event_outcome> parse_committed_outcome(std::string_view text)
{
    Json_parse_limits parse_limits;
    parse_limits.max_bytes = limits::k_max_helper_client_stdout_bytes;
    Json_parse_error parse_error;
    const std::optional<Json_value> document = parse_json(text, parse_limits, &parse_error);
    if (!document.has_value() || !document->is_object() ||
        strict::unexpected_member(*document, {"accepted", "reason", "epoch", "helper_response"}).has_value())
    {
        return std::nullopt;
    }
    const Json_value* accepted = strict::member_of_type(*document, "accepted", Json_type::BOOLEAN);
    if (accepted == nullptr) {
        return std::nullopt;
    }
    Provider_event_outcome outcome;
    outcome.committed = true;
    outcome.accepted  = accepted->as_boolean();
    outcome.exit_code = k_helper_exit_committed;
    const Json_value* reason_value = document->find("reason");
    if (outcome.accepted) {
        if (reason_value) {
            return std::nullopt;
        }
    }
    else {
        if (!reason_value || !reason_value->is_string() || reason_value->as_string().empty()) {
            return std::nullopt;
        }
        outcome.reason = reason_value->as_string();
    }
    if (document->find("epoch") != nullptr) {
        const std::optional<std::int64_t> epoch = strict::integer_member(*document, "epoch", 0, INT64_MAX);
        if (!epoch.has_value()) {
            return std::nullopt;
        }
        outcome.epoch = (std::uint64_t)*epoch;
    }
    if (const Json_value* response = document->find("helper_response"); response != nullptr) {
        outcome.helper_response = *response;
    }
    return outcome;
}

bool is_plain_argument(std::string_view text)
{
    return !text.empty() && strict::has_no_control_characters(text);
}

} // namespace

Json_value encode_model_context_delivery(const Model_context_delivery& delivery)
{
    Json_value record = Json_value::object();
    record.set("version", Json_value(delivery.activation
        ? k_model_context_delivery_activation_version : k_model_context_delivery_version));
    record.set("kind", Json_value("model-context"));
    if (delivery.activation) {
        Json_value activation = Json_value::object();
        activation.set("provider_session_id", Json_value(delivery.activation->provider_session_id));
        activation.set("transition_nonce", Json_value(delivery.activation->transition_nonce));
        record.set("activation", std::move(activation));
    }
    return record;
}

std::optional<Model_context_delivery> decode_model_context_delivery(
    const Json_value& value, Contract_failure* out_failure)
{
    const auto version = strict::integer_member(value, "version",
        k_model_context_delivery_version, k_model_context_delivery_activation_version);
    const auto* kind = strict::string_member(value, "kind");
    if (!value.is_object() || !version || !kind || *kind != "model-context" ||
        strict::unexpected_member(value, {"version", "kind", "activation"}))
    {
        report(out_failure, reason::k_frame_violation, "Invalid model-context delivery record");
        return std::nullopt;
    }
    const auto* activation = value.find("activation");
    if (*version == k_model_context_delivery_version) {
        if (!activation) return Model_context_delivery{};
    }
    else if (activation && activation->is_object() &&
        !strict::unexpected_member(*activation, {"provider_session_id", "transition_nonce"}))
    {
        const auto* id = strict::string_member(*activation, "provider_session_id");
        const auto* nonce = strict::string_member(*activation, "transition_nonce");
        if (id && nonce && !id->empty() && id->size() <= limits::k_max_lifecycle_identity_bytes &&
            is_valid_utf8(*id) && strict::has_no_control_characters(*id) &&
            is_valid_transition_nonce_hex(*nonce))
        {
            return Model_context_delivery{Model_context_activation{*id, *nonce}};
        }
    }
    report(out_failure, reason::k_frame_violation, "Invalid model-context activation");
    return std::nullopt;
}

std::optional<Helper_environment> parse_helper_environment(
    const std::vector<std::pair<std::string, std::string>>& environment,
    Contract_failure*                                       out_failure)
{
    const auto value_of = [&environment](const char* name) -> const std::string* {
        const auto entry = std::find_if(environment.begin(), environment.end(),
            [name](const std::pair<std::string, std::string>& candidate) { return candidate.first == name; });
        return entry == environment.end() || entry->second.empty() ? nullptr : &entry->second;
    };
    const auto missing = [out_failure](const char* name) {
        report(out_failure, reason::k_invalid_helper_environment, std::string(name) + " is absent or empty");
        return std::optional<Helper_environment>();
    };
    const auto out_of_grammar = [out_failure](const char* name) {
        report(out_failure, reason::k_invalid_helper_environment, std::string(name) + " is outside its grammar");
        return std::optional<Helper_environment>();
    };

    const std::string* vnmctl_path        = value_of(k_helper_env_vnmctl_path);
    const std::string* session_capability = value_of(k_helper_env_session_capability);
    const std::string* package_id         = value_of(k_helper_env_package_id);
    const std::string* generation_digest  = value_of(k_helper_env_generation_digest);
    const std::string* generation_nonce   = value_of(k_helper_env_generation_nonce);
    const std::string* helpers_dir        = value_of(k_helper_env_helpers_dir);
    if (vnmctl_path == nullptr)        { return missing(k_helper_env_vnmctl_path); }
    if (session_capability == nullptr) { return missing(k_helper_env_session_capability); }
    if (package_id == nullptr)         { return missing(k_helper_env_package_id); }
    if (generation_digest == nullptr)  { return missing(k_helper_env_generation_digest); }
    if (generation_nonce == nullptr)   { return missing(k_helper_env_generation_nonce); }
    if (helpers_dir == nullptr)        { return missing(k_helper_env_helpers_dir); }

    const bool capability_valid = session_capability->size() == limits::k_session_capability_hex_chars &&
        std::all_of(session_capability->begin(), session_capability->end(), [](char c) {
            return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        });
    if (has_nul(*vnmctl_path) || vnmctl_path->size() > limits::k_max_start_absolute_path_bytes) {
        return out_of_grammar(k_helper_env_vnmctl_path);
    }
    if (!capability_valid)                                { return out_of_grammar(k_helper_env_session_capability); }
    if (!is_valid_package_id(*package_id))                { return out_of_grammar(k_helper_env_package_id); }
    if (!is_valid_sha256_hex(*generation_digest))         { return out_of_grammar(k_helper_env_generation_digest); }
    if (!is_valid_generation_nonce_hex(*generation_nonce)) { return out_of_grammar(k_helper_env_generation_nonce); }
    if (has_nul(*helpers_dir) || helpers_dir->size() > limits::k_max_start_absolute_path_bytes) {
        return out_of_grammar(k_helper_env_helpers_dir);
    }

    Helper_environment helper_environment;
    helper_environment.vnmctl_path        = *vnmctl_path;
    helper_environment.session_capability = *session_capability;
    helper_environment.package_id         = *package_id;
    helper_environment.generation_digest  = *generation_digest;
    helper_environment.generation_nonce   = *generation_nonce;
    helper_environment.helpers_dir        = *helpers_dir;
    return helper_environment;
}

std::optional<Helper_environment> read_helper_environment(Contract_failure* out_failure)
{
    return parse_helper_environment(current_process_environment(), out_failure);
}

Helper_client::Helper_client(Helper_environment environment, std::function<bool()> cancellation_requested)
:
    m_environment(std::move(environment)),
    m_cancellation_requested(std::move(cancellation_requested))
{}

Provider_event_outcome Helper_client::deliver_provider_event(
    std::string_view  event_name,
    const Json_value& body,
    std::string_view  idempotency_key,
    std::string_view  transition_nonce)
{
    return deliver_provider_event({std::string(event_name), body,
        {idempotency_key.empty() ? random_nonce_hex() : std::string(idempotency_key),
         transition_nonce.empty() ? random_nonce_hex() : std::string(transition_nonce)}});
}

Provider_event_outcome Helper_client::deliver_provider_event(Prepared_provider_event delivery)
{
    const auto& event_name = delivery.event_name;
    if (event_name.empty() || event_name.size() > limits::k_max_provider_event_name_bytes || has_nul(event_name)) {
        return refused(reason::k_payload_too_large, std::move(delivery.identity));
    }
    if (!delivery.body.is_object()) {
        return refused(reason::k_frame_violation, std::move(delivery.identity));
    }
    const auto& key = delivery.identity.idempotency_key;
    if (key.size() > limits::k_max_idempotency_key_bytes || !is_plain_argument(key)) {
        return refused(reason::k_frame_violation, std::move(delivery.identity));
    }
    const auto& nonce = delivery.identity.transition_nonce;
    if (!is_valid_transition_nonce_hex(nonce)) {
        return refused(reason::k_frame_violation, std::move(delivery.identity));
    }

    Json_value event = Json_value::object();
    event.set("name", Json_value(std::move(delivery.event_name)));
    event.set("body", std::move(delivery.body));
    Json_value arguments = Json_value::object();
    arguments.set("idempotency_key",  Json_value(key));
    arguments.set("transition_nonce", Json_value(nonce));
    arguments.set("event",            std::move(event));

    const Wrapper_run run = run_wrapper(
        m_environment.vnmctl_path,
        {"provider-event", "deliver", "--args-stdin"},
        serialize_json(arguments),
        limits::k_max_helper_client_stdout_bytes,
        limits::k_helper_client_wrapper_deadline_ms,
        m_cancellation_requested);
    Provider_event_outcome outcome;
    outcome.exit_code = run.exit_code;
    outcome.local_diagnostic = run.local_diagnostic;
    if (run.exit_code == k_helper_exit_committed) {
        if (auto parsed = parse_committed_outcome(run.stdout_bytes)) {
            parsed->identity = std::move(delivery.identity);
            return std::move(*parsed);
        }
    }
    if (run.exit_code == k_helper_exit_committed ||
        (run.exit_code == k_helper_exit_pending && !run.stdout_bytes.empty()))
    {
        outcome.exit_code = k_helper_exit_transport_failure;
        outcome.local_diagnostic = invalid_wrapper_response(run.stdout_bytes.size());
    }
    outcome.identity = std::move(delivery.identity);
    return outcome;
}

int Helper_client::acknowledge_provider_event(
    std::string_view idempotency_key)
{
    if (idempotency_key.empty() ||
        idempotency_key.size() > limits::k_max_idempotency_key_bytes ||
        !is_plain_argument(idempotency_key))
    {
        return k_helper_exit_invalid_arguments;
    }
    const Wrapper_run run = run_wrapper(
        m_environment.vnmctl_path,
        {"provider-event", "acknowledge", std::string(idempotency_key)},
        {},
        limits::k_max_helper_client_stdout_bytes,
        limits::k_helper_client_wrapper_deadline_ms,
        m_cancellation_requested);
    if ((run.exit_code == k_helper_exit_committed || run.exit_code == k_helper_exit_pending) &&
        !run.stdout_bytes.empty())
    {
        return k_helper_exit_transport_failure;
    }
    return run.exit_code;
}

int Helper_client::set_title(std::string_view title)
{
    if (!is_plain_argument(title)) {
        return k_helper_exit_invalid_arguments;
    }
    const Wrapper_run run = run_wrapper(
        m_environment.vnmctl_path,
        {"set-title", std::string(title)},
        {},
        limits::k_max_helper_client_stdout_bytes,
        limits::k_helper_client_wrapper_deadline_ms,
        m_cancellation_requested);
    return run.exit_code;
}

Title_instruction_result Helper_client::request_title_instruction(
    std::string_view source,
    std::string_view hook_event_name,
    bool             changed_files_requested,
    std::optional<Model_context_delivery> delivery)
{
    if (!is_plain_argument(source) || !is_plain_argument(hook_event_name)) {
        return invalid_title_instruction_arguments();
    }
    Json_value request = Json_value::object();
    request.set("source", Json_value(source));
    request.set("hook_event_name", Json_value(hook_event_name));
    request.set("changed_files_requested", Json_value(changed_files_requested));
    if (delivery) {
        Json_value record = encode_model_context_delivery(*delivery);
        if (!decode_model_context_delivery(record, nullptr)) {
            return invalid_title_instruction_arguments();
        }
        request.set("delivery", std::move(record));
    }
    const Wrapper_run run = run_wrapper(
        m_environment.vnmctl_path,
        {"request-title-instruction", "--args-stdin"},
        serialize_json(request),
        limits::k_max_helper_client_stdout_bytes,
        limits::k_helper_client_wrapper_deadline_ms,
        m_cancellation_requested);
    Title_instruction_result result;
    result.exit_code = run.exit_code;
    result.local_diagnostic = run.local_diagnostic;
    if (run.exit_code == k_helper_exit_committed) {
        if (auto parsed = parse_title_instruction_result(run.stdout_bytes)) {
            return std::move(*parsed);
        }
        result.exit_code = k_helper_exit_transport_failure;
        result.local_diagnostic = invalid_wrapper_response(run.stdout_bytes.size());
    }
    return result;
}

int Helper_client::show_changed_files(const std::vector<std::string>& paths)
{
    const bool plain_paths = std::all_of(paths.begin(), paths.end(), [](const std::string& path) {
        return is_plain_argument(path);
    });
    if (paths.empty() || !plain_paths) {
        return k_helper_exit_invalid_arguments;
    }
    std::vector<std::string> arguments = {"call", "logonomic.diff", "show_changed_files", "--"};
    arguments.insert(arguments.end(), paths.begin(), paths.end());
    const Wrapper_run run = run_wrapper(
        m_environment.vnmctl_path,
        arguments,
        {},
        limits::k_max_helper_client_stdout_bytes,
        limits::k_helper_client_wrapper_deadline_ms,
        m_cancellation_requested);
    return run.exit_code;
}

int Helper_client::show_diff(std::string_view manifest_json)
{
    Json_parse_error parse_error;
    const std::optional<Json_value> manifest = parse_json(manifest_json, Json_parse_limits{}, &parse_error);
    if (!manifest.has_value() || !manifest->is_object()) {
        return k_helper_exit_invalid_arguments;
    }
    const Wrapper_run run = run_wrapper(
        m_environment.vnmctl_path,
        {"call", "logonomic.diff", "show", "--manifest-stdin"},
        manifest_json,
        limits::k_max_helper_client_stdout_bytes,
        limits::k_helper_client_wrapper_deadline_ms,
        m_cancellation_requested);
    return run.exit_code;
}

} // namespace logonomic::cli_addon
