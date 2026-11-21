#include "sdk_test_support.h"

#include "logonomic_cli_addon/auth_status.h"
#include "logonomic_cli_addon/account_refresh.h"
#include "logonomic_cli_addon/host_simulator.h"
#include "logonomic_cli_addon/quota.h"
#include "logonomic_cli_addon/package_source.h"
#include "logonomic_cli_addon/usage_scan.h"
#include "logonomic_cli_addon/stored_usage_accounting.h"
#include "logonomic_cli_addon/usage_incremental.h"
#include "logonomic_cli_addon/usage_temporal.h"
#include "logonomic_cli_addon/sha256.h"

#include <algorithm>
#include <ctime>
#include <limits>
#include <filesystem>
#include <stdexcept>

#ifdef _WIN32
#include <windows.h>
#include <winioctl.h>
#endif

namespace sdk_tests {

namespace {

namespace addon = logonomic::cli_addon;

std::filesystem::path fixture_io_path(const std::filesystem::path& path)
{
#ifdef _WIN32
    const auto native = std::filesystem::path(path).make_preferred().native();
    return native.starts_with(L"\\\\")
        ? L"\\\\?\\UNC\\" + native.substr(2) : L"\\\\?\\" + native;
#else
    return path;
#endif
}

addon::Session_launch_context launch_context()
{
    addon::Session_launch_context context;
    context.launcher      = {"unknown-fixture-cli", addon::k_runner_direct, {}};
    context.launcher_argv = {"unknown-fixture-cli"};
    context.launch_environment = addon::Launch_environment{addon::host_platform_id(),
        addon::Json_value::object(), std::nullopt};
    return context;
}

addon::Usage_scan_page usage_page()
{
    addon::Usage_record record;
    record.record_id      = "event-01";
    record.completed_date = "2024-02-29";
    record.total_tokens   = 9007199254740991ULL;
    record.input          = {true, 0};
    record.output         = {true, 9007199254740991ULL};
    record.aggregate_evidence = "complete";
    for (auto* amount : {&record.input, &record.output, &record.cache_read, &record.cache_creation}) {
        amount->coverage = addon::Usage_field_coverage{amount->applicable ? 1U : 0U, 1, false,
            amount->applicable ? addon::Usage_support::SUPPORTED : addon::Usage_support::UNSUPPORTED};
    }
    addon::Usage_scan_page page;
    page.scope       = {std::nullopt, 1700000000000ULL, 60000};
    page.snapshot_id = "snapshot-1";
    page.complete    = true;
    page.records     = {record};
    const auto profile = std::filesystem::temp_directory_path() / "usage-accounting-contract";
    page.profile_storage = addon::Profile_storage_context{1, addon::path_to_utf8(profile)};
    page.cache_candidate = addon::Usage_cache_reference{addon::path_to_utf8(profile / "candidate"),
        page.snapshot_id, std::string(64, 'a')};
    return page;
}

} // namespace

bool test_auth_quota_contracts()
{
    // Oracle: current quota preserves the native interactive stdin exchange,
    // exact native percentages and textual reset description.
    addon::Contract_failure exchange_failure;
    addon::Probe_prepare_request frozen{launch_context()};
    frozen.context.launch_environment = addon::Launch_environment{"linux-x86_64", addon::Json_value::object(), std::nullopt};
    frozen.context.launch_environment->effective_environment.set("PROFILE_HOME", addon::Json_value("/profiles/work"));
    const auto frozen_payload = addon::encode_probe_prepare_request(frozen, 2);
    const auto frozen_decoded = addon::decode_probe_prepare_request(frozen_payload, &exchange_failure, 2);
    SDK_CHECK(frozen_decoded && frozen_decoded->context.launch_environment,
        "probe2 receives the exact selected-profile environment");
    SDK_CHECK(frozen_decoded->context.launch_environment->effective_environment ==
        frozen.context.launch_environment->effective_environment,
        "probe2 preserves the frozen environment values");
    auto invalid_environment = frozen_payload;
    invalid_environment.erase("launch_environment");
    SDK_CHECK(!addon::decode_probe_prepare_request(invalid_environment, &exchange_failure, 2),
        "probe2 requires the frozen environment");
    invalid_environment.set("launch_environment", addon::Json_value(true));
    SDK_CHECK(!addon::decode_probe_prepare_request(invalid_environment, &exchange_failure, 2),
        "probe2 rejects a malformed environment");
    addon::Quota_inspect_result continuation;
    continuation.stdin_bytes = "{\"method\":\"initialized\"}\n";
    const auto continuation_payload = addon::encode_quota_inspect_result(continuation, 2);
    const auto continued = addon::decode_quota_inspect_result(continuation_payload, false, &exchange_failure, 2);
    SDK_CHECK(continued && continued->stdin_bytes == continuation.stdin_bytes,
        "quota2 can send the next protocol message without closing stdin");
    SDK_CHECK(!addon::decode_quota_inspect_result(continuation_payload, false, &exchange_failure, 65535),
        "unknown quota versions are not admitted");
    continuation.complete = true;
    continuation.observation = addon::Quota_observation{};
    SDK_CHECK(!addon::decode_quota_inspect_result(
        addon::encode_quota_inspect_result(continuation, 2), true, &exchange_failure, 2),
        "completed quota cannot leave a pending stdin write");
    continuation.stdin_bytes.reset();
    continuation.observation->state = addon::Quota_status::READY;
    continuation.observation->primary = addon::Quota_window{};
    continuation.observation->primary->reset_text = "Resets at 4 pm";
    const auto reset = addon::decode_quota_inspect_result(
        addon::encode_quota_inspect_result(continuation, 2), true, &exchange_failure, 2);
    SDK_CHECK(reset && reset->observation->primary->reset_text == "Resets at 4 pm",
        "provider reset text survives without inventing an absolute timestamp");
    continuation.observation->primary->reset_text.reset();
    continuation.observation->primary->used_percent = 123.5;
    SDK_CHECK(addon::decode_quota_inspect_result(
        addon::encode_quota_inspect_result(continuation, 2), true, &exchange_failure, 2).has_value(),
        "quota2 preserves native finite overage without clamping");

    // Collection observations are authoritative even when all limits disappear.
    addon::Quota_observation collection;
    collection.scope = {"Work", 1700000000000ULL, 60000};
    collection.state = addon::Quota_status::READY;
    collection.windows = std::vector<addon::Quota_window>{
        {12.5, std::nullopt, std::nullopt, "Week (all models)", std::nullopt, false},
        {123.5, std::nullopt, std::nullopt, "Week (Fable)", "Tomorrow", true},
        {0, 90, std::nullopt, std::nullopt},
    };
    addon::Quota_inspect_result collection_result{true, {}, collection};
    auto collection_payload = addon::encode_quota_inspect_result(collection_result, 3);
    auto collection_decoded = addon::decode_quota_inspect_result(collection_payload, true, &exchange_failure, 3);
    SDK_CHECK(collection_decoded && collection_decoded->observation->windows->size() == 3,
        "quota3 retains every observed scope without fixed slots");
    const auto& windows = *collection_decoded->observation->windows;
    SDK_CHECK(windows[0].headline == false && !windows[0].window_minutes &&
        windows[1].headline == true && windows[1].used_percent == 123.5 &&
        windows[1].label == "Week (Fable)" && windows[1].reset_text == "Tomorrow" &&
        windows[2].window_minutes == 90 && !windows[2].label && !windows[2].headline,
        "quota3 retains source order, scope, absent metadata and headline evidence");
    SDK_CHECK(!addon::decode_quota_inspect_result(collection_payload, true, &exchange_failure, 2),
        "quota2 cannot misread a collection as absent slots");
    auto malformed_collection = collection_payload;
    malformed_collection.find("observation")->erase("windows");
    SDK_CHECK(!addon::decode_quota_inspect_result(malformed_collection, true, &exchange_failure, 3),
        "ready quota3 must distinguish an empty collection from no observation");
    malformed_collection = collection_payload;
    malformed_collection.find("observation")->set("primary", addon::Json_value::object());
    SDK_CHECK(!addon::decode_quota_inspect_result(malformed_collection, true, &exchange_failure, 3),
        "quota3 rejects simultaneous legacy slot authority");
    malformed_collection = collection_payload;
    malformed_collection.find("observation")->find("windows")->as_array()[0].set("headline", addon::Json_value("yes"));
    SDK_CHECK(!addon::decode_quota_inspect_result(malformed_collection, true, &exchange_failure, 3),
        "headline selection must be an observed boolean");
    collection_result.observation->windows->clear();
    collection_decoded = addon::decode_quota_inspect_result(
        addon::encode_quota_inspect_result(collection_result, 3), true, &exchange_failure, 3);
    SDK_CHECK(collection_decoded && collection_decoded->observation->state == addon::Quota_status::READY &&
        collection_decoded->observation->windows && collection_decoded->observation->windows->empty(),
        "successful empty quota3 reports no current meters");
    bool rejected_collection_as_legacy = false;
    try {
        (void)addon::encode_quota_inspect_result(collection_result, 2);
    }
    catch (const std::invalid_argument&) {
        rejected_collection_as_legacy = true;
    }
    SDK_CHECK(rejected_collection_as_legacy, "an accidental quota2 encode cannot silently discard the collection");

    // Oracle: current frozen probes and exact independently owned account scope acknowledgment.
    addon::Contract_failure failure;
    addon::Probe_prepare_request prepare{launch_context()};
    prepare.context.account_context = addon::Account_context{1, "Primary"};
    const auto prepared = addon::decode_probe_prepare_request(addon::encode_probe_prepare_request(prepare), &failure);
    SDK_CHECK(prepared && prepared->context.account_context->adapter_account_key == "Primary",
        "probe preparation preserves selected account bytes without a structured purpose");
    auto invalid = addon::encode_probe_prepare_request(prepare);
    invalid.set("password", addon::Json_value("private-fixture-value"));
    SDK_CHECK(!addon::decode_probe_prepare_request(invalid, &failure), "prepare rejects undeclared secret members");
    SDK_CHECK(failure.detail.find("private-fixture-value") == std::string::npos, "failure does not repeat secret values");
    prepare.context.account_context.reset();
    SDK_CHECK(addon::decode_probe_prepare_request(addon::encode_probe_prepare_request(prepare), &failure).has_value(),
        "default scope requires no fabricated accounts record");

    addon::Probe_inspect_request inspection;
    inspection.inspection.stdout_bytes = std::string("a\0b", 3);
    inspection.inspection.final        = true;
    inspection.account_context         = addon::Account_context{1, "Primary"};
    const auto inspected = addon::decode_probe_inspect_request(addon::encode_probe_inspect_request(inspection), &failure);
    SDK_CHECK(inspected && inspected->inspection.stdout_bytes == inspection.inspection.stdout_bytes &&
        inspected->account_context->adapter_account_key == "Primary", "inspection reuses bounded binary stream grammar");

    addon::Auth_inspect_result auth{true, {}, addon::Auth_observation{{"Primary", 1700000000000ULL, 60000},
        addon::Auth_status::AUTHENTICATED}};
    for (const auto state : {addon::Auth_status::AUTHENTICATED, addon::Auth_status::NOT_LOGGED_IN,
            addon::Auth_status::UNKNOWN, addon::Auth_status::UNAVAILABLE, addon::Auth_status::FAILED})
    {
        auth.observation->state = state;
        const auto result = addon::decode_auth_inspect_result(addon::encode_auth_inspect_result(auth), true, &failure);
        SDK_CHECK(result && result->observation->state == state, "auth distinguishes every normalized state");
    }
    for (const char* member : {"token", "credentials", "error", "stdout_base64"}) {
        invalid = addon::encode_auth_inspect_result(auth);
        invalid.find("observation")->set(member, addon::Json_value("private-fixture-value"));
        SDK_CHECK(!addon::decode_auth_inspect_result(invalid, true, &failure), "auth projection rejects raw/private data");
        SDK_CHECK(failure.detail.find("private-fixture-value") == std::string::npos, "diagnostics remain host-authored");
    }
    invalid = addon::encode_auth_inspect_result(auth);
    invalid.find("observation")->find("scope")->erase("source_account_key");
    SDK_CHECK(!addon::decode_auth_inspect_result(invalid, true, &failure), "scope source key is mandatory even at default");
    for (const auto& timestamp : {addon::Json_value(-1), addon::Json_value(1.5), addon::Json_value(static_cast<std::uint64_t>(9007199254740992ULL))}) {
        invalid = addon::encode_auth_inspect_result(auth);
        invalid.find("observation")->find("scope")->set("observed_at_ms", timestamp);
        SDK_CHECK(!addon::decode_auth_inspect_result(invalid, true, &failure), "observation time is exact and bounded");
    }
    addon::Auth_inspect_result pending;
    SDK_CHECK(addon::decode_auth_inspect_result(addon::encode_auth_inspect_result(pending), false, &failure).has_value() &&
        !addon::decode_auth_inspect_result(addon::encode_auth_inspect_result(pending), true, &failure),
        "only nonfinal inspections may remain pending");
    pending.observation = auth.observation;
    SDK_CHECK(!addon::decode_auth_inspect_result(addon::encode_auth_inspect_result(pending), false, &failure),
        "an incomplete inspection cannot publish an observation");
    auth.observation.reset();
    SDK_CHECK(!addon::decode_auth_inspect_result(addon::encode_auth_inspect_result(auth), true, &failure),
        "a completed inspection requires its normalized observation");

    addon::Quota_observation observation;
    observation.scope           = {std::nullopt, 1700000000000ULL, 0};
    observation.state           = addon::Quota_status::READY;
    observation.primary         = addon::Quota_window{0, 300, 1700000060000ULL, "Five hours"};
    observation.secondary       = addon::Quota_window{100, std::nullopt, std::nullopt, std::nullopt};
    observation.credits_balance = "12.3400";
    observation.reset_credits   = 9007199254740991LL;
    const addon::Quota_inspect_result quota{true, {}, observation};
    const auto result = addon::decode_quota_inspect_result(addon::encode_quota_inspect_result(quota), true, &failure);
    SDK_CHECK(result && !result->observation->scope.source_account_key && result->observation->scope.fresh_for_ms == 0 &&
        result->observation->primary->used_percent == 0 && result->observation->secondary->used_percent == 100 &&
        result->observation->credits_balance == "12.3400", "default scope, observed zeros and exact credits are retained");
    for (const double percent : {std::numeric_limits<double>::infinity(),
            std::numeric_limits<double>::quiet_NaN()}) {
        invalid = addon::encode_quota_inspect_result(quota);
        invalid.find("observation")->find("primary")->set("used_percent", addon::Json_value(percent));
        SDK_CHECK(!addon::decode_quota_inspect_result(invalid, true, &failure), "quota percentages must be finite");
    }
    for (const char* balance : {"1e4", " 1", "01.2", "USD 12", "1.", "private fixture diagnostic"}) {
        invalid = addon::encode_quota_inspect_result(quota);
        invalid.find("observation")->set("credits_balance", addon::Json_value(balance));
        SDK_CHECK(!addon::decode_quota_inspect_result(invalid, true, &failure), "credits use exact bounded decimal grammar");
    }
    invalid = addon::encode_quota_inspect_result(quota);
    invalid.find("observation")->set("state", addon::Json_value("error"));
    SDK_CHECK(!addon::decode_quota_inspect_result(invalid, true, &failure), "failed quota cannot publish stale quota fields");

    for (const auto reason : {
            addon::Quota_failure::AUTHENTICATION_REQUIRED, addon::Quota_failure::CREDENTIALS_EXPIRED,
            addon::Quota_failure::REQUEST_REJECTED, addon::Quota_failure::REQUEST_FAILED,
            addon::Quota_failure::INVALID_RESPONSE, addon::Quota_failure::TIMED_OUT,
            addon::Quota_failure::PROCESS_FAILED, addon::Quota_failure::REGISTRY_FAILED})
    {
        const auto failed = addon::make_quota_failure(reason, observation.scope);
        const auto decoded = addon::decode_quota_inspect_result(
            addon::encode_quota_inspect_result(failed, 2, true), true, &failure);
        SDK_CHECK(decoded && decoded->complete && decoded->observation && decoded->failure_reason &&
            decoded->observation->state == addon::Quota_status::FAILED &&
            decoded->observation->scope.source_account_key == observation.scope.source_account_key &&
            addon::parse_quota_failure(*decoded->failure_reason) == reason,
            "a completed negative quota observation preserves its classified cause and account scope");
        SDK_CHECK(std::string_view(addon::quota_failure_description(reason)).size() > 0,
            "every classified quota cause has a safe presentation description");
    }
    const auto failed = addon::make_quota_failure(addon::Quota_failure::REQUEST_REJECTED,
        observation.scope, addon::Quota_diagnostic{addon::Quota_failure_stage::HTTP_RESPONSE, 503});
    const auto diagnostic_payload = addon::encode_quota_inspect_result(failed, 2, true);
    const auto diagnosed = addon::decode_quota_inspect_result(diagnostic_payload, true, &failure);
    SDK_CHECK(diagnosed && diagnosed->diagnostic && diagnosed->diagnostic->code == 503 &&
        diagnosed->diagnostic->stage == addon::Quota_failure_stage::HTTP_RESPONSE,
        "HTTP failure evidence reaches the diagnostic consumer without response text");
    for (const auto stage : {addon::Quota_failure_stage::PROCESS_START,
            addon::Quota_failure_stage::PROCESS_EXIT, addon::Quota_failure_stage::REGISTRY_READ})
    {
        const auto native = addon::make_quota_failure(addon::Quota_failure::PROCESS_FAILED,
            observation.scope, addon::Quota_diagnostic{stage, 3221225477LL});
        const auto decoded = addon::decode_quota_inspect_result(
            addon::encode_quota_inspect_result(native, 2, true), true, &failure);
        SDK_CHECK(decoded && decoded->diagnostic && decoded->diagnostic->stage == stage &&
            decoded->diagnostic->code == 3221225477LL,
            "native diagnostic codes retain the unsigned Windows exit range");
    }
    // Unreviewed explanation values are dropped whole and never echoed: the
    // observation stays a neutral failure.
    const auto neutral = [&](const addon::Json_value& payload) {
        const auto decoded = addon::decode_quota_inspect_result(payload, true, &failure);
        return decoded && decoded->observation && decoded->observation->state == addon::Quota_status::FAILED &&
            !decoded->failure_reason && !decoded->diagnostic && decoded->explanation_ignored &&
            failure.detail.find("private") == std::string::npos;
    };
    for (const char* private_value : {"private-output", "C:/private/profile", "quota_request_failed: token"}) {
        invalid = diagnostic_payload;
        invalid.find("quota_failure")->set("reason", addon::Json_value(private_value));
        SDK_CHECK(!addon::parse_quota_failure(private_value) && neutral(invalid),
            "unreviewed quota reasons cannot cross the privacy boundary");
        invalid = diagnostic_payload;
        invalid.find("quota_failure")->find("diagnostic")->set("stage", addon::Json_value(private_value));
        SDK_CHECK(neutral(invalid), "diagnostic stages cannot carry native text");
    }
    for (const auto code : {-2147483649LL, 4294967296LL, 99LL, 600LL}) {
        invalid = diagnostic_payload;
        invalid.find("quota_failure")->find("diagnostic")->set("code", addon::Json_value((std::int64_t)code));
        SDK_CHECK(neutral(invalid), "HTTP diagnostic codes must be actual bounded status codes");
    }
    invalid = diagnostic_payload;
    invalid.find("quota_failure")->find("diagnostic")->set("output", addon::Json_value("private-output"));
    SDK_CHECK(neutral(invalid), "quota diagnostic objects cannot append raw output");
    invalid = diagnostic_payload;
    invalid.find("quota_failure")->set("version", addon::Json_value(2));
    SDK_CHECK(neutral(invalid), "an explanation of another version is not used");
    invalid = addon::encode_quota_inspect_result(quota);
    invalid.set("failure_reason", addon::Json_value("quota_invalid_response"));
    SDK_CHECK(!addon::decode_quota_inspect_result(invalid, true, &failure),
        "successful quota cannot simultaneously report a failure reason");
    invalid = diagnostic_payload;
    invalid.set("failure_reason", addon::Json_value("quota_invalid_response"));
    SDK_CHECK(!addon::decode_quota_inspect_result(invalid, true, &failure),
        "the quota@2 failure reason names only lost authentication");

    // Optional current-login evidence has its own capability boundary. Its
    // absence or rejection must preserve an otherwise valid authentication answer.
    SDK_CHECK(addon::decode_current_account_declaration(addon::Json_value::object(), &failure),
        "current-account declares no private configuration");
    prepare.observe_current_account = true;
    const auto current_prepare = addon::decode_probe_prepare_request(
        addon::encode_probe_prepare_request(prepare), &failure);
    SDK_CHECK(current_prepare && current_prepare->observe_current_account,
        "current-account is an explicit independent opt-in");
    invalid = addon::encode_probe_prepare_request(prepare);
    invalid.find("current_account")->set("version", addon::Json_value(2));
    const auto unknown_current = addon::decode_probe_prepare_request(invalid, &failure);
    SDK_CHECK(unknown_current && !unknown_current->observe_current_account,
        "an unknown optional version leaves the original probe usable");

    addon::Auth_inspect_result identified_auth{true, {}, addon::Auth_observation{{"profile-source", 1700000000000ULL, 0}, addon::Auth_status::AUTHENTICATED}};
    identified_auth.observation->scope.source_account_key = "profile-source";
    identified_auth.current_account = addon::Current_account_observation{
        identified_auth.observation->scope, "opaque-current-account", true};
    const auto identified_payload = addon::encode_auth_inspect_result(identified_auth);
    const auto identified = addon::decode_auth_inspect_result(identified_payload, true, &failure);
    SDK_CHECK(identified && identified->current_account &&
        identified->current_account->identity_key == "opaque-current-account" &&
        identified->current_account->is_default_profile &&
        identified->observation->scope.source_account_key == "profile-source",
        "current identity preserves the independent historical source key");
    for (const char* member : {"source_account_key", "observed_at_ms", "fresh_for_ms"}) {
        invalid = identified_payload;
        invalid.find("current_account")->find("scope")->erase(member);
        const auto rejected = addon::decode_auth_inspect_result(invalid, true, &failure);
        SDK_CHECK(rejected && rejected->observation && !rejected->current_account,
            "incomplete identity scope cannot invalidate authentication");
    }
    invalid = identified_payload;
    invalid.find("current_account")->find("scope")->set("source_account_key", addon::Json_value("other-source"));
    const auto other_source = addon::decode_auth_inspect_result(invalid, true, &failure);
    SDK_CHECK(other_source && !other_source->current_account,
        "identity from another source cannot group the selected profile");
    for (const auto& malformed : {addon::Json_value(true), addon::Json_value("private-fixture-value")}) {
        invalid = identified_payload;
        invalid.set("current_account", malformed);
        const auto rejected = addon::decode_auth_inspect_result(invalid, true, &failure);
        SDK_CHECK(rejected && rejected->observation && !rejected->current_account,
            "malformed optional identity leaves valid authentication intact");
        SDK_CHECK(failure.detail.find("private-fixture-value") == std::string::npos,
            "optional identity rejection never repeats private response bytes");
    }
    return true;
}

bool test_account_refresh_contracts()
{
    // account-refresh@1 separates a completed quota observation from native
    // completion; authenticated identity is admitted only in final inspection.
    addon::Contract_failure failure;
    addon::Probe_prepare_request prepare{launch_context()};
    prepare.context.account_context = addon::Account_context{1, "Work profile"};
    prepare.context.launch_environment = addon::Launch_environment{
        "linux-x86_64", addon::Json_value::object(), std::nullopt};
    prepare.context.launch_environment->effective_environment.set(
        "PROFILE_HOME", addon::Json_value("/profiles/work"));
    prepare.observe_current_account = true;
    const auto prepared = addon::decode_account_refresh_prepare_request(
        addon::encode_account_refresh_prepare_request(prepare), &failure);
    SDK_CHECK(prepared && prepared->observe_current_account &&
        prepared->context.account_context->adapter_account_key == "Work profile" &&
        prepared->context.launch_environment &&
        prepared->context.launch_environment->effective_environment ==
            prepare.context.launch_environment->effective_environment,
        "one refresh preserves its selected source, identity opt-in and frozen environment");
    auto missing_environment = addon::encode_account_refresh_prepare_request(prepare);
    missing_environment.erase("launch_environment");
    SDK_CHECK(!addon::decode_account_refresh_prepare_request(missing_environment, &failure),
        "the current account-refresh wrapper requires the frozen probe environment");

    addon::Probe_inspect_request inspect;
    inspect.account_context = prepare.context.account_context;
    inspect.observe_current_account = true;
    inspect.inspection.stdout_bytes = std::string("partial\0record", 14);
    const auto inspected = addon::decode_account_refresh_inspect_request(
        addon::encode_account_refresh_inspect_request(inspect), &failure);
    SDK_CHECK(inspected && inspected->observe_current_account &&
        inspected->inspection.stdout_bytes == inspect.inspection.stdout_bytes,
        "shared inspection preserves bounded binary fragments and independent identity opt-in");

    addon::Quota_observation quota;
    quota.scope = {"Work profile", 1700000000000ULL, 60000};
    quota.state = addon::Quota_status::READY;
    quota.primary = addon::Quota_window{123.5, 300, std::nullopt, "Primary"};
    addon::Account_refresh_inspect_result progress;
    progress.protocol_state = "waiting-for-auth";
    progress.quota = addon::Quota_inspect_result{true, {}, quota};
    const auto progress_payload = addon::encode_account_refresh_inspect_result(progress);
    const auto decoded_progress = addon::decode_account_refresh_inspect_result(progress_payload, false, &failure);
    SDK_CHECK(decoded_progress && !decoded_progress->complete && decoded_progress->quota &&
        decoded_progress->quota->observation->primary->used_percent == 123.5 && !decoded_progress->auth,
        "quota2 progress publishes native overage while authentication and process completion remain pending");
    SDK_CHECK(!addon::decode_account_refresh_inspect_result(progress_payload, true, &failure),
        "pending progress cannot masquerade as a final native result");

    addon::Account_refresh_inspect_result pending;
    SDK_CHECK(addon::decode_account_refresh_inspect_result(
        addon::encode_account_refresh_inspect_result(pending), false, &failure).has_value(),
        "an output fragment need not complete either observation");
    addon::Auth_inspect_result auth{true, {}, addon::Auth_observation{
        quota.scope, addon::Auth_status::AUTHENTICATED}};
    auth.current_account = addon::Current_account_observation{
        quota.scope, "workspace-identity", false};
    progress.auth = auth;
    SDK_CHECK(!addon::decode_account_refresh_inspect_result(
        addon::encode_account_refresh_inspect_result(progress), false, &failure),
        "authentication cannot publish before native final inspection");

    addon::Account_refresh_inspect_result final;
    final.complete = true;
    final.auth = auth;
    const auto final_payload = addon::encode_account_refresh_inspect_result(final);
    const auto decoded_final = addon::decode_account_refresh_inspect_result(final_payload, true, &failure);
    SDK_CHECK(decoded_final && !decoded_final->quota && decoded_final->auth->current_account &&
        decoded_final->auth->current_account->identity_key == "workspace-identity",
        "final authentication may follow quota already delivered in an earlier inspection");
    final.quota = addon::Quota_inspect_result{true, {}, quota};
    SDK_CHECK(addon::decode_account_refresh_inspect_result(
        addon::encode_account_refresh_inspect_result(final), true, &failure).has_value(),
        "a short native process can complete both observations in final inspection");
    for (const auto state : {addon::Auth_status::NOT_LOGGED_IN, addon::Auth_status::UNKNOWN,
             addon::Auth_status::UNAVAILABLE, addon::Auth_status::FAILED})
    {
        final.auth->current_account.reset();
        final.auth->observation->state = state;
        const auto decoded = addon::decode_account_refresh_inspect_result(
            addon::encode_account_refresh_inspect_result(final), true, &failure);
        SDK_CHECK(decoded && decoded->quota && decoded->auth->observation->state == state,
            "authentication failure leaves an independent completed quota disposition intact");
    }
    final.auth->observation->state = addon::Auth_status::AUTHENTICATED;
    final.quota = addon::make_quota_failure(addon::Quota_failure::INVALID_RESPONSE, quota.scope);
    const auto failed_quota = addon::decode_account_refresh_inspect_result(
        addon::encode_account_refresh_inspect_result(final, true), true, &failure);
    SDK_CHECK(failed_quota && failed_quota->quota && failed_quota->quota->failure_reason &&
        addon::parse_quota_failure(*failed_quota->quota->failure_reason) == addon::Quota_failure::INVALID_RESPONSE &&
        failed_quota->auth->observation->state == addon::Auth_status::AUTHENTICATED,
        "a malformed quota response does not erase a separately authenticated account");
    final.auth.reset();
    SDK_CHECK(!addon::decode_account_refresh_inspect_result(
        addon::encode_account_refresh_inspect_result(final), true, &failure),
        "final inspection must state the authentication disposition");

    for (const char* member : {"token", "credentials", "stdout_base64"}) {
        auto invalid = progress_payload;
        invalid.set(member, addon::Json_value("private-fixture-value"));
        SDK_CHECK(!addon::decode_account_refresh_inspect_result(invalid, false, &failure),
            "shared result rejects undeclared private payloads");
        SDK_CHECK(failure.detail.find("private-fixture-value") == std::string::npos,
            "shared result diagnostics never echo rejected secrets");
    }
    for (const bool final_inspection : {false, true}) {
        auto invalid = final_inspection ? final_payload : progress_payload;
        invalid.set("complete", addon::Json_value(!final_inspection));
        SDK_CHECK(!addon::decode_account_refresh_inspect_result(invalid, final_inspection, &failure),
            "outer completion agrees exactly with native final inspection");
    }
    auto invalid = progress_payload;
    invalid.find("quota")->set("complete", addon::Json_value(false));
    SDK_CHECK(!addon::decode_account_refresh_inspect_result(invalid, false, &failure),
        "a quota slot cannot carry unfinished quota protocol work");
    progress.auth.reset();
    progress.quota->stdin_bytes = "private-native-request";
    SDK_CHECK(!addon::decode_account_refresh_inspect_result(
        addon::encode_account_refresh_inspect_result(progress), false, &failure),
        "completed quota progress never transfers native stdin ownership");
    invalid = final_payload;
    invalid.find("auth")->set("current_account", addon::Json_value("private-fixture-value"));
    const auto without_identity = addon::decode_account_refresh_inspect_result(invalid, true, &failure);
    SDK_CHECK(without_identity && without_identity->auth->observation && !without_identity->auth->current_account,
        "malformed optional identity preserves the valid authentication result");
    return true;
}

bool test_usage_scan_contracts()
{
    {
        // These are the bytes of the current stored accounting contract. A
        // proof bound to them must survive the public scan codec cleanup.
        const std::string stored_bytes =
            "{\"record_id\":\"stored-accounting-proof\",\"completed_date\":\"2026-09-12\",\"total_tokens\":25,\"semantics\":\""
            "cumulative-final\",\"input\":{\"applicable\":true,\"coverage\":{\"covered_records\":2,\"total_records\":3,\"over"
            "flow\":false,\"support\":\"supported\"},\"value\":12},\"output\":{\"applicable\":true,\"coverage\":{\"covered_reco"
            "rds\":3,\"total_records\":3,\"overflow\":false,\"support\":\"supported\"},\"value\":13},\"cache_read\":{\"applicab"
            "le\":true,\"coverage\":{\"covered_records\":0,\"total_records\":3,\"overflow\":false,\"support\":\"unknown\"},\"va"
            "lue\":0},\"cache_creation\":{\"applicable\":false,\"coverage\":{\"covered_records\":0,\"total_records\":3,\"over"
            "flow\":false,\"support\":\"unsupported\"}},\"session_key\":\"session-proof\",\"coverage_unit\":\"records\",\"sourc"
            "e_counter_semantics\":\"per_event_sum\"}";
        addon::Contract_failure failure;
        const auto value = addon::parse_json(stored_bytes, {}, nullptr);
        const auto record = value ? addon::decode_stored_usage_accounting(*value, &failure) : std::nullopt;
        SDK_CHECK(record && !record->aggregate_evidence && record->capabilities.as_object().empty() &&
            record->input.coverage->covered_records == 2 && record->input.coverage->total_records == 3 &&
            record->cache_read.value == 0 && record->cache_read.coverage->support == addon::Usage_support::UNKNOWN &&
            !record->cache_creation.value && !record->cache_creation.applicable,
            "stored accounting preserves partial coverage, observed zero and absence without observation evidence");
        const auto encoded = addon::encode_stored_usage_accounting(*record);
        SDK_CHECK(addon::serialize_json(encoded) == stored_bytes &&
            addon::to_hex(addon::Sha256::digest(addon::serialize_json(encoded))) ==
                "9168dd8f8e7ee359b6e229a2bdce6a887a2daf63953f924ebba84147d2c0685c",
            "the current stored accounting bytes and their proof digest are stable");
        auto invalid = encoded;
        invalid.set("aggregate_evidence", addon::Json_value("complete"));
        SDK_CHECK(!addon::decode_stored_usage_accounting(invalid, &failure),
            "stored accounting cannot acquire synthesized public aggregate evidence");
        invalid = encoded;
        invalid.set("capabilities", addon::Json_value::object());
        SDK_CHECK(!addon::decode_stored_usage_accounting(invalid, &failure),
            "independent observation proofs are not stored accounting members");
        invalid = encoded;
        invalid.set("source_counter_semantics", addon::Json_value("cumulative_turn_delta"));
        SDK_CHECK(!addon::decode_stored_usage_accounting(invalid, &failure),
            "public observation counter semantics do not alter the current stored grammar");
    }
    {
        addon::Contract_failure failure;
        addon::Json_value declaration = addon::Json_value::object();
        addon::Json_value names = addon::Json_value::array();
        std::vector<std::pair<std::string, std::string>> ambient;
        for (const char* name : {
                "FIXTURE_HOME_A", "FIXTURE_HOME_B", "FIXTURE_HOME_C", "FIXTURE_HOME_D", "FIXTURE_HOME_E"})
        {
            names.push_back(addon::Json_value(name));
            ambient.emplace_back(name, std::string("/ambient/") + name);
        }
        declaration.set("environment_names", names);
        const auto decoded = addon::decode_usage_scan_declaration(declaration, &failure);
        SDK_CHECK(decoded && decoded->environment_names.size() == ambient.size(),
            "the current usage declaration retains every declared environment selector");
        ambient.emplace_back("UNDECLARED_PROFILE", "/private/not-projected");
        const auto view = addon::ambient_view(ambient, *decoded, addon::k_platform_linux_x86_64);
        for (std::size_t index = 0; index + 1 < ambient.size(); ++index) {
            SDK_CHECK(view.find(ambient[index].first) &&
                view.find(ambient[index].first)->as_string() == ambient[index].second,
                "usage declaration preserves the exact ambient selector without an accounts dependency");
        }
        SDK_CHECK(!view.find("UNDECLARED_PROFILE"), "usage projection excludes undeclared ambient variables");
        const auto empty = addon::decode_usage_scan_declaration(addon::Json_value::object(), &failure);
        SDK_CHECK(empty && empty->environment_names.empty(), "an empty current usage declaration remains valid");
        declaration.find("environment_names")->push_back(addon::Json_value("FIXTURE_HOME_B"));
        SDK_CHECK(!addon::decode_usage_scan_declaration(declaration, &failure),
            "current usage names retain the real duplicate-name validator");
    }
    {
        SDK_CHECK(addon::parse_native_usage_timestamp("1970-01-01T00:00:00.123456Z") == 123,
            "native usage timestamps discard sub-millisecond precision");
        SDK_CHECK(addon::parse_native_usage_timestamp("1970-01-01T00:00:00.1Z") == 100 &&
                addon::parse_native_usage_timestamp("1970-01-01T00:00:00.12Z") == 120,
            "short fractions preserve their decimal place value");
        SDK_CHECK(addon::parse_native_usage_timestamp("1970-01-01T02:30:00+02:30") == 0 &&
                addon::parse_native_usage_timestamp("1969-12-31T21:30:00-02:30") == 0 &&
                addon::parse_native_usage_timestamp("1970-01-01T00:00:00-00:00") == 0,
            "native usage offset conversion retains its existing zero-offset behavior");
        std::tm local{};
        local.tm_year = 124;
        local.tm_mon = 6;
        local.tm_mday = 1;
        local.tm_isdst = -1;
        const auto local_seconds = std::mktime(&local);
        SDK_CHECK(local_seconds != static_cast<std::time_t>(-1) &&
                addon::parse_native_usage_timestamp("2024-07-01T00:00:00.1234") ==
                    static_cast<std::int64_t>(local_seconds) * 1000 + 123,
            "zoneless native records use the platform local-time and DST rules");
        for (const char* iso : {"2026-02-29T00:00:00Z", "0000-01-01T00:00:00Z", "2024-01-01T00:00:60Z",
                "2024-01-01T00:00:00.Z", "2024-01-01T00:00:00+24:00", "2024-01-01T00:00:00z"})
        {
            SDK_CHECK(!addon::parse_native_usage_timestamp(iso), "invalid native usage timestamps are rejected");
        }
    }
    {
        auto page = usage_page();
        auto record = page.records.front();
        record.model = "model-\"quoted\"-\\-\xCE\xB1";
        record.capabilities.set("future-metadata", addon::Json_value("retained\nopaque\tdata"));
        page.records.clear();
        std::size_t encoded_records_bytes = 0;
        SDK_CHECK(!addon::append_usage_page_record(page, record, encoded_records_bytes, 3,
                addon::limits::k_max_exchange_payload_bytes + 1) &&
            page.records.empty() && encoded_records_bytes == 0,
            "an oversized wrapper reservation does not consume a record");
        for (std::size_t index = 0; index < addon::k_max_usage_page_records; ++index) {
            page.complete = index % 2 == 0;
            page.next_cursor = page.complete ? std::nullopt : std::optional("next-" + std::to_string(index + 1));
            page.incomplete_historical_coverage = index % 3 == 0;
            if (index == 1) {
                const auto account_root = std::filesystem::temp_directory_path() / "usage-page-account";
                page.account_context = addon::Account_context{2, "page-account", addon::path_to_utf8(account_root)};
                page.scope.source_account_key = page.account_context->adapter_account_key;
            }
            record.record_id = "event-" + std::to_string(1000 + index);
            if (index <= 1 || index + 1 == addon::k_max_usage_page_records) {
                // The serialized transport page is the size oracle, including
                // commas, escaping and envelope changes between appends.
                auto expected = page;
                expected.records.push_back(record);
                const auto expected_payload = addon::encode_usage_scan_result(expected);
                const auto bytes = addon::serialize_json(expected_payload).size();
                const auto reservation = addon::limits::k_max_exchange_payload_bytes - bytes;
                const auto previous_payload = addon::encode_usage_scan_result(page);
                const auto previous_bytes = encoded_records_bytes;
                SDK_CHECK(!addon::append_usage_page_record(page, record, encoded_records_bytes, 3, reservation + 1) &&
                    addon::encode_usage_scan_result(page) == previous_payload && encoded_records_bytes == previous_bytes,
                    "one byte over the exchange budget leaves the existing page and next record unconsumed");
                SDK_CHECK(addon::append_usage_page_record(page, record, encoded_records_bytes, 3, reservation) &&
                    addon::encode_usage_scan_result(page) == expected_payload,
                    "an exact-fit append preserves the complete encoded page after envelope changes");
            }
            else {
                SDK_CHECK(addon::append_usage_page_record(page, record, encoded_records_bytes),
                    "records fitting the count and byte budgets are appended");
            }
        }
        const auto payload = addon::encode_usage_scan_result(page);
        const auto previous_bytes = encoded_records_bytes;
        SDK_CHECK(!addon::append_usage_page_record(page, record, encoded_records_bytes) &&
            addon::encode_usage_scan_result(page) == payload && encoded_records_bytes == previous_bytes,
            "the record count limit leaves a full page and its byte counter unchanged");
        addon::Contract_failure failure;
        const auto wire = addon::parse_json(addon::serialize_json(payload), {}, nullptr);
        SDK_CHECK(wire && addon::decode_usage_scan_result(*wire, &failure).has_value(),
            "byte-packed pages retain the current transport codec contract");
    }
    {
        // Oracle: adopted record-linked temporal contract conserves accepted
        // total-token accounting and retains explicit temporal meaning.
        addon::Contract_failure failure;
        addon::Usage_record record;
        record.record_id = "conversation-temporal";
        record.completed_date = "2026-10-09";
        record.total_tokens = 100;
        record.aggregate_evidence = "complete";
        for (auto* amount : {&record.input, &record.output, &record.cache_read, &record.cache_creation}) {
            *amount = {false, std::nullopt, addon::Usage_field_coverage{0, 1, false, addon::Usage_support::UNSUPPORTED}};
        }
        addon::Usage_temporal evidence;
        evidence.parent = {record.record_id, record.total_tokens, "snapshot-temporal", std::nullopt};
        evidence.portions = {{"instant", "2026-10-08T23:59:30.123+02:00", "", "", 25},
            {"source-calendar", "", "2026-10-09", "provider-local-calendar", 35}};
        evidence.unavailable_tokens = 40;
        evidence.coverage_reason = "missing-evidence";
        record.temporal = evidence;
        record.capabilities.set("future-usage-metadata", addon::Json_value("retained-opaque"));
        const auto encoded = addon::encode_usage_record(record, 3);
        const auto decoded = addon::decode_usage_record(encoded, &failure, 3);
        SDK_CHECK(decoded && decoded->temporal && decoded->total_tokens == 100 &&
                decoded->temporal->unavailable_tokens == 40 &&
                decoded->temporal->portions.front().instant == evidence.portions.front().instant &&
                decoded->temporal->portions.back().calendar == "provider-local-calendar" &&
                addon::encode_usage_record(*decoded, 3) == encoded,
            "typed replay retains offsets, explicit date-only meaning, remainder and unknown capabilities");
        for (const char* instant : {"2026-10-08T23:59:30", "2026-02-30T12:00:00Z", "2026-10-08T23:59:30-00:00",
                "2026-10-08T23:59:30.Z", "2016-12-31T23:58:60Z", "2016-12-30T23:59:60Z",
                "2017-01-01T00:59:60+02:00", "2016-12-31T23:59:61Z"})
        {
            record.temporal->portions.front().instant = instant;
            SDK_CHECK(!addon::decode_usage_record(addon::encode_usage_record(record, 3), &failure, 3),
                "an offsetless, invalid-calendar or unknown-offset value cannot assert an instant");
        }
        record.temporal = evidence;
        record.temporal->unavailable_tokens = 41;
        SDK_CHECK(!addon::decode_usage_record(addon::encode_usage_record(record, 3), &failure, 3),
            "temporal portions and remainder must conserve the parent amount");
        record.temporal = evidence;
        record.temporal->parent.record_id = "other-conversation";
        SDK_CHECK(!addon::decode_usage_record(addon::encode_usage_record(record, 3), &failure, 3),
            "a temporal claim cannot attach to another accounting identity");
        record.temporal.reset();
        auto future = addon::encode_usage_temporal(evidence);
        future.set("version", addon::Json_value(19));
        future.set("future-format", addon::Json_value("opaque-evidence"));
        record.capabilities.set(addon::k_usage_temporal_capability_id, future);
        const auto future_encoded = addon::encode_usage_record(record, 3);
        const auto future_decoded = addon::decode_usage_record(future_encoded, &failure, 3);
        SDK_CHECK(future_decoded && !future_decoded->temporal && future_decoded->total_tokens == 100 &&
                addon::encode_usage_record(*future_decoded, 3) == future_encoded,
            "an unsupported temporal version preserves accounting and its complete opaque envelope");
        addon::Usage_scan_page page;
        page.snapshot_id = "other-snapshot";
        page.complete = true;
        page.scope = {std::nullopt, 1700000000000ULL, 60000};
        const auto profile_root = std::filesystem::temp_directory_path() / "usage-temporal-contract";
        page.profile_storage = addon::Profile_storage_context{1, addon::path_to_utf8(profile_root)};
        page.cache_candidate = addon::Usage_cache_reference{addon::path_to_utf8(profile_root / "candidate"),
            page.snapshot_id, std::string(64, 'a')};
        record.temporal = evidence;
        record.capabilities.erase(addon::k_usage_temporal_capability_id);
        page.records = {record};
        SDK_CHECK(!addon::decode_usage_scan_result(addon::encode_usage_scan_result(page, 3), &failure, 3),
            "temporal evidence must bind the acknowledged scan snapshot");
        page.snapshot_id = evidence.parent.snapshot_id;
        page.cache_candidate->snapshot_id = page.snapshot_id;
        SDK_CHECK(addon::decode_usage_scan_result(addon::encode_usage_scan_result(page, 3), &failure, 3).has_value(),
            "a valid temporal parent binds the same sealed snapshot and account as its accounting page");
        // RFC 3339 sections 5.6-5.8: case-insensitive T/Z, unrestricted
        // fractional digits, and offset-shifted leap-second representations.
        for (const std::string& instant : {std::string("2026-10-08T23:59:30.1234567891Z"),
                std::string("2026-10-08t23:59:30.12345678901234567890z"),
                std::string("0000-01-01T00:00:00Z"),
                std::string("2026-10-08T23:59:30.") + std::string(4096, '1') + "+02:00",
                std::string("2016-12-31T23:59:60.1234567891Z"),
                std::string("2017-01-01t13:59:60.1234567891+14:00"),
                std::string("1990-12-31T15:59:60-08:00")})
        {
            record.temporal->portions.front().instant = instant;
            const auto decoded_temporal = addon::decode_usage_temporal(addon::encode_usage_temporal(*record.temporal), &failure);
            page.records = {record};
            const auto encoded_page = addon::encode_usage_scan_result(page, 3);
            const auto wire = addon::parse_json(addon::serialize_json(encoded_page), {}, nullptr);
            const auto decoded_page = wire ? addon::decode_usage_scan_result(*wire, &failure, 3) : std::nullopt;
            SDK_CHECK(decoded_temporal && decoded_temporal->portions.front().instant == instant &&
                    decoded_page && decoded_page->records.front().temporal &&
                    decoded_page->records.front().total_tokens == record.total_tokens &&
                    decoded_page->records.front().temporal->portions.front().instant == instant &&
                    addon::encode_usage_scan_result(*decoded_page, 3) == encoded_page,
                "RFC 3339 precision, case and leap seconds survive the codec and accounting page without conversion");
        }
        record.temporal->portions.front().instant = "2026-10-08T23:59:30." + std::string(addon::k_max_usage_temporal_bytes, '1') + 'Z';
        SDK_CHECK(!addon::decode_usage_temporal(addon::encode_usage_temporal(*record.temporal), &failure),
            "fractional precision remains subject to the existing optional temporal envelope budget");
        page.records.clear();
        std::size_t bytes = 0;
        record.temporal.reset();
        record.capabilities.set("bounded-future-data", addon::Json_value(std::string(768u * 1024u, 'x')));
        int count = 0;
        while (addon::append_usage_page_record(page, record, bytes, 3)) {
            ++count;
            record.record_id += 'a';
        }
        SDK_CHECK(count > 0 && count < static_cast<int>(addon::k_max_usage_page_records) &&
                addon::serialize_json(addon::encode_usage_scan_result(page, 3)).size() <= addon::limits::k_max_exchange_payload_bytes &&
                record.capabilities.find("bounded-future-data"),
            "encoded-byte packing bounds pages before the count limit and preserves the unconsumed record");
        record.capabilities = addon::Json_value::object();
        record.repository_usage = addon::Usage_repository{{}, record.total_tokens};
        for (int index = 0; index < 2000; ++index) {
#ifdef _WIN32
            const std::string path = "C:/" + std::string(768, 'x') + std::to_string(index);
#else
            const std::string path = "/" + std::string(768, 'x') + std::to_string(index);
#endif
            record.repository_usage->allocations.push_back({path, 0});
        }
        const auto encoded_record = addon::encode_usage_record(record, 3);
        SDK_CHECK(addon::serialize_json(encoded_record).size() > 1024u * 1024u &&
                addon::decode_usage_record(encoded_record, &failure, 3).has_value(),
            "optional temporal limits do not narrow established accounting record contracts");
        page.records = {record, record};
        page.records[1].record_id += 'b';
        const auto encoded_page = addon::encode_usage_scan_result(page, 3);
        SDK_CHECK(addon::serialize_json(encoded_page).size() > 3u * 1024u * 1024u &&
                addon::serialize_json(encoded_page).size() < addon::limits::k_max_frame_bytes &&
                addon::decode_usage_scan_result(encoded_page, &failure, 3).has_value(),
            "optional evidence does not add a new global accounting page ceiling");

        const auto portion = evidence.portions.front();
        evidence.portions.assign(addon::k_max_usage_temporal_portions + 1, portion);
        SDK_CHECK(!addon::decode_usage_temporal(addon::encode_usage_temporal(evidence), &failure),
            "inline temporal rows are bounded before typed allocation");
    }
    {
        addon::Contract_failure failure;
        addon::Usage_record unknown_amount;
        unknown_amount.record_id = "event-unknown-cache";
        unknown_amount.completed_date = "2026-09-22";
        unknown_amount.total_tokens = 10;
        unknown_amount.input = {true, 4, addon::Usage_field_coverage{
            1, 1, false, addon::Usage_support::SUPPORTED}};
        unknown_amount.output = {true, 6, addon::Usage_field_coverage{
            1, 1, false, addon::Usage_support::SUPPORTED}};
        unknown_amount.cache_read = {true, std::nullopt, addon::Usage_field_coverage{
            0, 1, false, addon::Usage_support::UNKNOWN}};
        unknown_amount.cache_creation = {true, std::nullopt, addon::Usage_field_coverage{
            0, 1, false, addon::Usage_support::UNKNOWN}};
        unknown_amount.aggregate_evidence = "complete";

        const auto encoded_v3 = addon::encode_usage_record(unknown_amount, 3);
        const auto* cache_read = encoded_v3.find("cache_read");
        const auto* cache_creation = encoded_v3.find("cache_creation");
        SDK_CHECK(cache_read && !cache_read->find("value") && cache_creation && !cache_creation->find("value") &&
            cache_read->find("coverage")->find("support")->as_string() == "unknown" &&
            cache_creation->find("coverage")->find("support")->as_string() == "unknown",
            "usage3 omits numeric values for unknown token dimensions");
        const auto decoded_v3 = addon::decode_usage_record(encoded_v3, &failure, 3);
        SDK_CHECK(decoded_v3 && !decoded_v3->cache_read.value && !decoded_v3->cache_creation.value &&
            decoded_v3->cache_read.applicable && decoded_v3->cache_creation.applicable &&
            decoded_v3->cache_read.coverage->support == addon::Usage_support::UNKNOWN &&
            decoded_v3->cache_creation.coverage->support == addon::Usage_support::UNKNOWN,
            "usage3 preserves unknown support and absent values without synthesizing zero");

        auto inapplicable_unknown = unknown_amount;
        inapplicable_unknown.cache_read.applicable = false;
        SDK_CHECK(!addon::decode_usage_record(
                addon::encode_usage_record(inapplicable_unknown, 3), &failure, 3),
            "usage3 does not treat unknown support as proof that a dimension is inapplicable");

        auto covered_but_absent = unknown_amount;
        covered_but_absent.cache_read.coverage->covered_records = 1;
        SDK_CHECK(!addon::decode_usage_record(
                addon::encode_usage_record(covered_but_absent, 3), &failure, 3),
            "usage3 requires an exact amount when unknown coverage reports observed records");

        auto overflowed_but_covered = unknown_amount;
        overflowed_but_covered.cache_read.coverage->covered_records = 1;
        overflowed_but_covered.cache_read.coverage->overflow = true;
        SDK_CHECK(!addon::decode_usage_record(
                addon::encode_usage_record(overflowed_but_covered, 3), &failure, 3),
            "usage3 rejects an absent unknown amount with covered records even when overflowed");

        auto supported_without_value = unknown_amount;
        supported_without_value.cache_read = {true, std::nullopt, addon::Usage_field_coverage{
            0, 1, false, addon::Usage_support::SUPPORTED}};
        SDK_CHECK(!addon::decode_usage_record(
                addon::encode_usage_record(supported_without_value, 3), &failure, 3),
            "usage3 still rejects a supported dimension with no observed value");

        auto stored_unknown_without_value = unknown_amount;
        stored_unknown_without_value.aggregate_evidence.reset();
        SDK_CHECK(!addon::decode_stored_usage_accounting(
                addon::encode_stored_usage_accounting(stored_unknown_without_value), &failure),
            "current stored accounting retains its explicit unknown amount requirement");
        auto stored_known_zero = stored_unknown_without_value;
        stored_known_zero.cache_read = {true, 0, addon::Usage_field_coverage{
            0, 1, false, addon::Usage_support::UNKNOWN}};
        stored_known_zero.cache_creation = {false, std::nullopt, addon::Usage_field_coverage{
            0, 1, false, addon::Usage_support::UNSUPPORTED}};
        SDK_CHECK(addon::decode_stored_usage_accounting(addon::encode_stored_usage_accounting(stored_known_zero), &failure).has_value(),
            "current stored accounting preserves known zero and unsupported dimensions");


    }
    {
        namespace fs = std::filesystem;
        const auto root = fs::canonical(make_temp_directory("usage-cache-contract"));
        struct Cleanup {
            fs::path root;
            ~Cleanup() { std::error_code error; fs::remove_all(root, error); }
        } cleanup{root};
        fs::create_directory(root / "candidate");
        const auto profile = addon::path_to_utf8(root);
        const auto candidate = addon::path_to_utf8(root / "candidate");
        addon::Contract_failure failure;
        addon::Usage_scan_request request;
        request.context = launch_context();
        request.context.profile_storage = addon::Profile_storage_context{1, profile};
        request.context.launch_environment = addon::Launch_environment{
            addon::host_platform_id(), addon::Json_value::object(), std::nullopt};
        request.cache_selection = addon::Usage_cache_selection{std::nullopt, candidate};
        const auto encoded = addon::encode_usage_scan_request(request, 3);
        SDK_CHECK(addon::decode_usage_scan_request(encoded, &failure, 3).has_value(), "usage3 carries explicit cache authority");
        auto invalid = encoded;
        invalid.erase("cache_selection");
        SDK_CHECK(!addon::decode_usage_scan_request(invalid, &failure, 3), "usage3 cannot omit cache selection");
        invalid = encoded;
        invalid.find("cache_selection")->erase("accepted");
        SDK_CHECK(!addon::decode_usage_scan_request(invalid, &failure, 3), "absence is explicit, not inferred");
        SDK_CHECK(addon::validate_usage_cache_allocation(candidate, profile, &failure), "fresh allocation is empty");
        SDK_CHECK(write_file(root / "candidate" / "records", "retained-event 19\n"), "write candidate evidence");
        const auto cancelled = [] { return false; };
        const auto sealed = addon::seal_usage_cache_candidate(
            candidate, profile, std::nullopt, "snapshot-17", "fixture-usage@1", cancelled, &failure);
        SDK_CHECK(sealed.has_value(), "candidate is durably sealed without promoting a pointer");
        const auto manifest = addon::validate_usage_cache_candidate(*sealed, profile, std::nullopt, cancelled, &failure);
        SDK_CHECK(manifest && manifest->files.size() == 1 && manifest->format == "fixture-usage@1",
            "reopened candidate has exact inventory and provider format");
        SDK_CHECK(addon::validate_usage_cache_manifest(*sealed, profile, std::nullopt, cancelled, &failure).has_value(),
            "incremental readers validate accepted manifest identity separately from payloads");
        SDK_CHECK(addon::validate_usage_cache_file(*sealed, *manifest, "records", cancelled, &failure),
            "a consumed cache payload is verified against its accepted manifest");
        auto incremental_request = request;
        incremental_request.cache_selection = addon::Usage_cache_selection{
            *sealed, addon::path_to_utf8(root / "incremental-candidate")};
        SDK_CHECK(addon::decode_usage_incremental_request(
                addon::encode_usage_incremental_request(incremental_request), &failure).has_value(),
            "incremental requests retain their explicit accepted baseline");
        incremental_request.cache_selection->accepted.reset();
        SDK_CHECK(!addon::decode_usage_incremental_request(
                addon::encode_usage_incremental_request(incremental_request), &failure),
            "a first import cannot claim an unchanged accepted baseline");
        const addon::Usage_incremental_result unchanged{*sealed, std::nullopt, std::nullopt, request.context.profile_storage};
        auto unchanged_wire = addon::encode_usage_incremental_result(unchanged);
        const auto decoded_unchanged = addon::decode_usage_incremental_result(unchanged_wire, &failure);
        SDK_CHECK(decoded_unchanged && decoded_unchanged->baseline == *sealed && !decoded_unchanged->changes,
            "an unchanged result retains exact baseline identity without manufacturing a snapshot");
        SDK_CHECK(decoded_unchanged->profile_storage && decoded_unchanged->profile_storage->root == profile,
            "the unchanged result preserves the acknowledged profile root");
        auto missing_storage = unchanged_wire;
        missing_storage.erase(addon::k_profile_storage_member);
        SDK_CHECK(!addon::decode_usage_incremental_result(missing_storage, nullptr),
            "incremental results require a profile acknowledgment even when no diagnostic is requested");
        auto with_label = addon::profile_storage_member(*unchanged.profile_storage);
        with_label.set("label", addon::Json_value("cache"));
        auto text_version = addon::profile_storage_member(*unchanged.profile_storage);
        text_version.set("version", addon::Json_value("1"));
        for (const auto& storage : {addon::Json_value(), addon::Json_value::object(), with_label, text_version,
                                    addon::profile_storage_member({2, profile})})
        {
            auto invalid_result = unchanged_wire;
            invalid_result.set(addon::k_profile_storage_member, storage);
            failure = {};
            SDK_CHECK(!addon::decode_usage_incremental_result(invalid_result, &failure) &&
                failure.reason_code == "usage_incremental_invalid" && !failure.detail.empty(),
                "incremental results reject an invalid profile acknowledgment with a contract failure");
        }
        unchanged_wire.set("changes", addon::Json_value::object());
        SDK_CHECK(!addon::decode_usage_incremental_result(unchanged_wire, &failure),
            "a result cannot both retain and replace the accepted baseline");
        {
            fs::create_directory(root / "inherited");
            const auto inherited_root = addon::path_to_utf8(root / "inherited");
            SDK_CHECK(addon::inherit_usage_cache_file(*sealed, *manifest, "records",
                inherited_root, profile, cancelled, &failure), "carry one immutable accepted file");
            SDK_CHECK(write_file(root / "inherited" / "metadata", "source checkpoint 2\n"), "write changed metadata");
            const auto inherited = addon::seal_usage_incremental_cache_candidate(inherited_root, profile,
                std::nullopt, "snapshot-18", "fixture-usage@2", *sealed, cancelled, &failure);
            SDK_CHECK(inherited.has_value(), "seal inherited evidence and new metadata together");
            const auto inherited_manifest = addon::validate_usage_incremental_cache_candidate(
                *inherited, *sealed, profile, std::nullopt, cancelled, &failure);
            SDK_CHECK(inherited_manifest && inherited_manifest->files.size() == 2,
                "incremental validation binds exact inventory and accepted file identity");
            SDK_CHECK(!addon::validate_usage_incremental_cache_candidate(*inherited, *sealed, profile,
                std::nullopt, [] { return true; }, &failure) && failure.reason_code == "cancelled",
                "inherited validation preserves cancellation");

            fs::create_directory(root / "self-contained");
            const auto independent_root = addon::path_to_utf8(root / "self-contained");
            SDK_CHECK(addon::inherit_usage_cache_file(*inherited, *inherited_manifest, "records",
                independent_root, profile, cancelled, &failure), "carry evidence into another generation");
            const auto independent = addon::seal_usage_incremental_cache_candidate(independent_root, profile,
                std::nullopt, "snapshot-19", "fixture-usage@2", *inherited, cancelled, &failure);
            SDK_CHECK(independent.has_value(), "seal a self-contained generation");

            // Replace the link with different same-size bytes while leaving its
            // manifest intact. Descriptor equality alone must never trust it.
            SDK_CHECK(fs::remove(root / "inherited" / "records"), "detach inherited evidence before corruption");
            SDK_CHECK(write_file(root / "inherited" / "records", "retained-event 11\n"), "write forged copied evidence");
            SDK_CHECK(!addon::validate_usage_incremental_cache_candidate(
                *inherited, *sealed, profile, std::nullopt, cancelled, &failure) &&
                failure.reason_code == "usage_cache_content_mismatch",
                "a copied payload cannot reuse an accepted hash by descriptor alone");
            fs::remove_all(root / "inherited");
            SDK_CHECK(addon::validate_usage_cache_candidate(
                *independent, profile, std::nullopt, cancelled, &failure).has_value(),
                "reclaiming a prior generation leaves inherited evidence self-contained");
        }
#ifdef _WIN32
        {
            const auto manifest_path = fixture_io_path(root / "candidate" / "cache-manifest.json");
            const HANDLE handle = CreateFileW(manifest_path.c_str(), GENERIC_READ,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
            SDK_CHECK(handle != INVALID_HANDLE_VALUE, "open the accepted manifest for a real read contention");
            OVERLAPPED region{};
            const bool locked = LockFileEx(handle, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY,
                0, MAXDWORD, MAXDWORD, &region) != 0;
            if (!locked) {
                CloseHandle(handle);
            }
            SDK_CHECK(locked, "hold the accepted manifest's actual byte range");
            const auto unreadable = addon::validate_usage_cache_candidate(
                *sealed, profile, std::nullopt, cancelled, &failure);
            const bool unlocked = UnlockFileEx(handle, 0, MAXDWORD, MAXDWORD, &region) != 0;
            const bool closed = CloseHandle(handle) != 0;
            SDK_CHECK(unlocked && closed, "release the manifest's native read contention");
            SDK_CHECK(!unreadable && failure.reason_code == "usage_cache_unavailable",
                "an opened manifest's short read is retryable I/O, not damaged content");
        }
#endif
        {
            addon::Usage_cache_manifest large{std::nullopt, "large-snapshot", "fixture-usage@1", {}};
            const std::string digest(64, 'a');
            for (std::size_t i = 0; i < 65'537; ++i) {
                large.files.push_back({"records-" + std::to_string(100'000 + i), 131'073, digest});
            }
            large.files.front().bytes = 8ull * 1024 * 1024 * 1024 + 1;
            const auto decoded = addon::decode_usage_cache_manifest(addon::encode_usage_cache_manifest(large), &failure);
            SDK_CHECK(decoded && decoded->files.size() == large.files.size() &&
                decoded->files.front().bytes == large.files.front().bytes,
                "cache inventories exceed former file, individual-byte and aggregate-byte ceilings");
            large.files.resize(3);
            for (auto& file : large.files) {
                file.bytes = std::numeric_limits<std::int64_t>::max();
            }
            SDK_CHECK(!addon::decode_usage_cache_manifest(addon::encode_usage_cache_manifest(large), &failure),
                "an unrepresentable aggregate byte count is rejected instead of wrapping");
        }
        {
            const auto streamed_root = root / "streamed-manifest";
            fs::create_directory(streamed_root);
            SDK_CHECK(write_file(streamed_root / "records", "retained-event 19\n"), "write streamed manifest evidence");
            const auto streamed_candidate = addon::path_to_utf8(streamed_root);
            std::string bytes(17 * 1024 * 1024, ' ');
            bytes += addon::serialize_json(addon::encode_usage_cache_manifest(*manifest));
            SDK_CHECK(write_file(streamed_root / "cache-manifest.json", bytes), "write manifest beyond former byte ceiling");
            const addon::Usage_cache_reference reference{
                streamed_candidate, manifest->snapshot_id, addon::to_hex(addon::Sha256::digest(bytes))};
            SDK_CHECK(addon::validate_usage_cache_candidate(reference, profile, std::nullopt, cancelled, &failure).has_value(),
                "manifest size does not limit a fully verified cache");
            int checkpoints = 0;
            SDK_CHECK(!addon::validate_usage_cache_candidate(reference, profile, std::nullopt,
                [&] { return ++checkpoints == 8; }, &failure) && failure.reason_code == "cancelled",
                "streaming a large manifest remains cancellable");
            const std::string canonical = addon::serialize_json(addon::encode_usage_cache_manifest(*manifest));
            std::string escaped = canonical;
            escaped.replace(escaped.find("\"files\""), 7, "\"fi\\u006ces\"");
            std::string duplicate = canonical;
            duplicate.insert(1, "\"version\":1,");
            std::string trailing_comma = canonical;
            trailing_comma.insert(trailing_comma.size() - 1, ",");
            const std::pair<std::string, bool> documents[] = {
                {escaped, true},
                {duplicate, false},
                {trailing_comma, false},
                {canonical + "null", false},
                {canonical.substr(0, canonical.size() - 2), false},
            };
            for (const auto& [document, accepted] : documents) {
                SDK_CHECK(write_file(streamed_root / "cache-manifest.json", document), "write manifest syntax case");
                const addon::Usage_cache_reference syntax_reference{
                    streamed_candidate, manifest->snapshot_id, addon::to_hex(addon::Sha256::digest(document))};
                SDK_CHECK(addon::validate_usage_cache_candidate(
                    syntax_reference, profile, std::nullopt, cancelled, &failure).has_value() == accepted,
                    "streamed framing retains strict JSON validation and accepts escaped member names");
                SDK_CHECK(accepted || failure.reason_code == "usage_cache_content_mismatch",
                    "readable malformed sealed manifests are deterministic content mismatches");
            }
        }
        SDK_CHECK(!addon::seal_usage_cache_candidate(
                candidate, profile, std::nullopt, "snapshot-18", "fixture-usage@1", cancelled, &failure),
            "a sealed allocation cannot be overwritten");
        SDK_CHECK(!addon::validate_usage_cache_candidate(*sealed, profile,
                addon::Account_context{1, "other-account"}, cancelled, &failure),
            "cache integrity is bound to exact account context");
        SDK_CHECK(failure.reason_code == "usage_cache_content_mismatch",
            "a sealed manifest for another account is a deterministic content mismatch");
        auto wrong_snapshot = *sealed;
        wrong_snapshot.snapshot_id = "other-snapshot";
        SDK_CHECK(!addon::validate_usage_cache_candidate(wrong_snapshot, profile,
                std::nullopt, cancelled, &failure) && failure.reason_code == "usage_cache_content_mismatch",
            "a sealed manifest for another snapshot is a deterministic content mismatch");
        request.cache_selection->accepted = sealed;
        SDK_CHECK(!addon::validate_usage_cache_selection(request, &failure), "candidate cannot overwrite accepted root");
        request.cache_selection->candidate_root = addon::path_to_utf8(root / "next");
        SDK_CHECK(addon::validate_usage_cache_selection(request, &failure), "disjoint selected roots are admitted");
        request.mode = "reset";
        SDK_CHECK(!addon::validate_usage_cache_selection(request, &failure), "reset cannot reuse accepted evidence");
        request.cache_selection->accepted.reset();
        SDK_CHECK(addon::validate_usage_cache_selection(request, &failure), "reset selects sources only");
        auto page = usage_page();
        page.snapshot_id = sealed->snapshot_id;
        page.cache_candidate = sealed;
        page.profile_storage = request.context.profile_storage;
        auto& record = page.records.front();
        record.aggregate_evidence = "partial";
        for (auto* amount : {&record.input, &record.output, &record.cache_read, &record.cache_creation}) {
            amount->coverage = addon::Usage_field_coverage{amount->applicable ? 1U : 0U, 1, false,
                amount->applicable ? addon::Usage_support::SUPPORTED : addon::Usage_support::UNSUPPORTED};
        }
        const auto wire_page = addon::encode_usage_scan_result(page, 3);
        const auto decoded = addon::decode_usage_scan_result(wire_page, &failure, 3);
        SDK_CHECK(decoded && decoded->cache_candidate == sealed &&
            decoded->records.front().aggregate_evidence == "partial", "page preserves cache and aggregate evidence");
        SDK_CHECK(decoded->profile_storage && decoded->profile_storage->root == profile &&
            !decoded->account_context && !wire_page.find("account_context"),
            "no-account page acknowledges profile and omits account context");
        const addon::Usage_incremental_result changed{*sealed, page, std::nullopt, request.context.profile_storage};
        const auto decoded_changes = addon::decode_usage_incremental_result(
            addon::encode_usage_incremental_result(changed), &failure);
        SDK_CHECK(decoded_changes && decoded_changes->changes && decoded_changes->changes->records.size() == 1,
            "incremental changes preserve absolute accounting observations");
        auto metadata_only = changed;
        metadata_only.changes->records.clear();
        metadata_only.changes->complete = true;
        metadata_only.changes->next_cursor.reset();
        const auto decoded_metadata = addon::decode_usage_incremental_result(
            addon::encode_usage_incremental_result(metadata_only), &failure);
        SDK_CHECK(decoded_metadata && decoded_metadata->changes && decoded_metadata->changes->records.empty(),
            "a metadata-only replacement is distinct from retaining the exact baseline");
        SDK_CHECK(!addon::profile_storage_acknowledgment_failure(*request.context.profile_storage, wire_page),
            "usage page satisfies the production profile acknowledgment check");
        for (const char* member : {"profile_storage", "account_context"}) {
            invalid = wire_page;
            invalid.set(member, addon::Json_value());
            SDK_CHECK(!addon::decode_usage_scan_result(invalid, &failure, 3),
                "acknowledgments cannot be null");
        }
        invalid = wire_page;
        invalid.erase("profile_storage");
        SDK_CHECK(!addon::decode_usage_scan_result(invalid, &failure, 3), "profile echo is required");
        invalid = wire_page;
        invalid.set("unowned_ack", addon::Json_value(true));
        SDK_CHECK(!addon::decode_usage_scan_result(invalid, &failure, 3), "page members remain strictly owned");
        invalid = wire_page;
        invalid.find("profile_storage")->set("extra", addon::Json_value(true));
        SDK_CHECK(!addon::decode_usage_scan_result(invalid, &failure, 3), "profile record uses its strict codec");
        invalid = wire_page;
        invalid.find("profile_storage")->set("version", addon::Json_value(2));
        SDK_CHECK(!addon::decode_usage_scan_result(invalid, &failure, 3), "profile version remains independently owned");
        invalid = wire_page;
        invalid.find("profile_storage")->set("root", addon::Json_value(profile + "/other"));
        SDK_CHECK(addon::profile_storage_acknowledgment_failure(*request.context.profile_storage, invalid).has_value(),
            "host rejects a different profile even when the record is well formed");
        for (const addon::Account_context& account : {
                 addon::Account_context{1, "selected-account"},
                 addon::Account_context{2, "selected-account", std::nullopt},
                 addon::Account_context{2, "selected-account", profile}})
        {
            auto selected = page;
            selected.account_context = account;
            selected.scope.source_account_key = account.adapter_account_key;
            const auto wire = addon::encode_usage_scan_result(selected, 3);
            const auto parsed = addon::decode_usage_scan_result(wire, &failure, 3);
            SDK_CHECK(parsed && parsed->account_context &&
                addon::encode_account_context(*parsed->account_context) == addon::encode_account_context(account),
                "usage acknowledgment preserves the exact account version and nullable path");
            SDK_CHECK(!addon::account_context_acknowledgment_failure(account, wire),
                "usage page satisfies the production account acknowledgment check");
            invalid = wire;
            invalid.erase("account_context");
            SDK_CHECK(!addon::decode_usage_scan_result(invalid, &failure, 3),
                "selected scope cannot omit its account acknowledgment");
            invalid = wire;
            invalid.find("account_context")->set("adapter_account_key", addon::Json_value("other-account"));
            SDK_CHECK(!addon::decode_usage_scan_result(invalid, &failure, 3) &&
                addon::account_context_acknowledgment_failure(account, invalid),
                "scope and requested account must not be replaced");
            invalid = wire;
            invalid.find("account_context")->set("extra", addon::Json_value(true));
            SDK_CHECK(!addon::decode_usage_scan_result(invalid, &failure, 3),
                "account record uses its strict codec");
            if (account.version == 2) {
                invalid = wire;
                invalid.find("account_context")->erase("profile_path");
                SDK_CHECK(!addon::decode_usage_scan_result(invalid, &failure, 3),
                    "account version two requires an explicit nullable profile path");
                invalid = wire;
                invalid.set("account_context", addon::encode_account_context({1, account.adapter_account_key}));
                SDK_CHECK(addon::account_context_acknowledgment_failure(account, invalid).has_value(),
                    "host rejects a downgraded acknowledgment version");
                invalid = wire;
                invalid.find("account_context")->set("profile_path", addon::Json_value(profile + "/other"));
                SDK_CHECK(addon::account_context_acknowledgment_failure(account, invalid).has_value(),
                    "host rejects a different native profile path");
            }
        }
        invalid = wire_page;
        invalid.find("records")->as_array().front().erase("aggregate_evidence");
        SDK_CHECK(!addon::decode_usage_scan_result(invalid, &failure, 3), "missing evidence cannot assert completeness");
        invalid = wire_page;
        invalid.find("cache_candidate")->set("snapshot_id", addon::Json_value("foreign-snapshot"));
        SDK_CHECK(!addon::decode_usage_scan_result(invalid, &failure, 3), "cache must bind the page snapshot");
        for (const char* name : {"../escape", "nul", "con.txt", "records.", "cache-manifest.json"}) {
            auto bad_manifest = *manifest;
            bad_manifest.files.front().path = name;
            SDK_CHECK(!addon::decode_usage_cache_manifest(addon::encode_usage_cache_manifest(bad_manifest), &failure),
                "cache inventory forbids escape, device and reserved names");
        }
        SDK_CHECK(write_file(root / "candidate" / "records", "retained-event 11\n"), "simulate storage corruption");
        SDK_CHECK(!addon::validate_usage_cache_file(*sealed, *manifest, "records", cancelled, &failure) &&
            failure.reason_code == "usage_cache_content_mismatch",
            "incremental readers reject corruption of a consumed payload");
        SDK_CHECK(!addon::validate_usage_cache_candidate(*sealed, profile, std::nullopt, cancelled, &failure),
            "a selected corrupt cache is unavailable, never replaced by a newest cache");
        SDK_CHECK(failure.reason_code == "usage_cache_content_mismatch",
            "completed content mismatch is distinct from an unavailable cache read");
        SDK_CHECK(!addon::validate_usage_cache_candidate(*sealed, profile, std::nullopt,
            [] { return true; }, &failure) && failure.reason_code == "cancelled",
            "cancellation is not classified as damaged accepted data");
        fs::rename(root / "candidate" / "cache-manifest.json", root / "candidate" / "cache-manifest.saved");
        SDK_CHECK(!addon::validate_usage_cache_candidate(*sealed, profile, std::nullopt, cancelled, &failure) &&
            failure.reason_code == "usage_cache_unavailable",
            "a manifest read failure remains unavailable rather than a content mismatch");
        fs::rename(root / "candidate" / "cache-manifest.saved", root / "candidate" / "cache-manifest.json");
#ifdef _WIN32
        // The capability accepts scoped absolute paths, not only MAX_PATH-sized
        // allocations. Extended Win32 I/O spelling must not alter that identity.
        cleanup.root = fixture_io_path(root);
        auto long_parent = root;
        while (long_parent.native().size() <= MAX_PATH + 32) {
            long_parent /= "retained-account-cache-allocation-component";
        }
        const auto long_path = long_parent / "candidate";
        fs::create_directories(fixture_io_path(long_path));
        const auto long_candidate = addon::path_to_utf8(long_path);
        SDK_CHECK(addon::validate_usage_cache_allocation(long_candidate, profile, &failure),
            "existing long allocation is admitted without changing its wire spelling");
        const auto producer_path = addon::usage_cache_io_path(long_path / "records");
        SDK_CHECK(producer_path == fixture_io_path(long_path / "records") &&
            addon::usage_cache_io_path(producer_path) == producer_path, "cache I/O spelling is idempotent");
        SDK_CHECK(write_file(producer_path, "retained-event 19\n"), "write long candidate evidence");
        const auto long_sealed = addon::seal_usage_cache_candidate(
            long_candidate, profile, std::nullopt, "snapshot-long", "fixture-usage@1", cancelled, &failure);
        SDK_CHECK(long_sealed && long_sealed->root == long_candidate,
            "long cache seals durably and preserves its exact reference identity");
        const auto long_manifest = addon::validate_usage_cache_candidate(
            *long_sealed, profile, std::nullopt, cancelled, &failure);
        SDK_CHECK(long_manifest && long_manifest->files.size() == 1 &&
            long_manifest->files.front().path == "records", "long selected cache reopens with exact inventory");

        // A directory junction needs no symlink privilege and exercises the
        // reparse-point rejection at the same long-path boundary as admission.
        const auto junction = long_parent / "redirect";
        const auto junction_io = fixture_io_path(junction);
        fs::create_directory(junction_io);
        const auto target = fs::path(long_path).make_preferred().native();
        const auto substitute = target.starts_with(L"\\\\")
            ? L"\\??\\UNC\\" + target.substr(2) : L"\\??\\" + target;
        struct junction_data_t {
            DWORD tag;
            WORD data_length;
            WORD reserved;
            WORD substitute_offset;
            WORD substitute_length;
            WORD print_offset;
            WORD print_length;
            wchar_t paths[1024];
        } data{};
        SDK_CHECK(substitute.size() + target.size() + 2 <= std::size(data.paths), "junction fixture fits its buffer");
        data.tag = IO_REPARSE_TAG_MOUNT_POINT;
        data.substitute_length = (WORD)(substitute.size() * sizeof(wchar_t));
        data.print_offset = data.substitute_length + sizeof(wchar_t);
        data.print_length = (WORD)(target.size() * sizeof(wchar_t));
        data.data_length = 8 + data.print_offset + data.print_length + sizeof(wchar_t);
        std::copy(substitute.begin(), substitute.end(), data.paths);
        std::copy(target.begin(), target.end(), data.paths + substitute.size() + 1);
        const HANDLE handle = CreateFileW(junction_io.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        SDK_CHECK(handle != INVALID_HANDLE_VALUE, "open long junction fixture without following it");
        DWORD returned = 0;
        const bool linked = DeviceIoControl(handle, FSCTL_SET_REPARSE_POINT, &data, data.data_length + 8,
            nullptr, 0, &returned, nullptr) != 0;
        const bool closed = CloseHandle(handle) != 0;
        SDK_CHECK(linked && closed, "create real long-path directory reparse point");
        const DWORD attributes = GetFileAttributesW(junction_io.c_str());
        SDK_CHECK(attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0,
            "fixture carries the reparse-point attribute");
        const auto redirected = addon::path_to_utf8(junction);
        SDK_CHECK(!addon::validate_usage_cache_root(redirected, profile, &failure),
            "long-path spelling does not admit a directory junction");
        SDK_CHECK(!addon::seal_usage_cache_candidate(redirected, profile, std::nullopt,
            "snapshot-redirect", "fixture-usage@1", cancelled, &failure), "sealing rejects a redirected root");
        auto redirected_reference = *long_sealed;
        redirected_reference.root = redirected;
        SDK_CHECK(!addon::validate_usage_cache_candidate(redirected_reference, profile, std::nullopt,
            cancelled, &failure), "selected cache validation rejects a redirected root");
        SDK_CHECK(RemoveDirectoryW(junction_io.c_str()) != 0, "remove junction without deleting its target");
#endif
    }
    {
        // Oracle: native session aggregates retain partial per-message category
        // coverage and independent repository allocations under one session ID.
        addon::Contract_failure failure;
        addon::Usage_record aggregate;
        aggregate.record_id = "session-work";
        aggregate.session_key = "session-work";
        aggregate.completed_date = "2026-09-12";
        aggregate.total_tokens = 25;
        aggregate.input = {true, 12, addon::Usage_field_coverage{2, 3, false, addon::Usage_support::SUPPORTED}};
        aggregate.output = {true, 13, addon::Usage_field_coverage{3, 3, false, addon::Usage_support::SUPPORTED}};
        aggregate.cache_read = {true, 0, addon::Usage_field_coverage{0, 3, false, addon::Usage_support::UNKNOWN}};
        aggregate.cache_creation = {false, std::nullopt,
            addon::Usage_field_coverage{0, 3, false, addon::Usage_support::UNSUPPORTED}};
        aggregate.coverage_unit = addon::Usage_coverage_unit::RECORDS;
        const std::string repository = addon::path_to_utf8(std::filesystem::current_path());
        aggregate.repository_usage = addon::Usage_repository{{{repository, 18}}, 7};
        aggregate.model = "multiple";
        aggregate.counter_field = "usage.input_tokens+usage.output_tokens";
        aggregate.source_counter_semantics = "per_event_sum";
        auto turn_delta = aggregate;
        turn_delta.aggregate_evidence = "partial";
        turn_delta.source_counter_semantics = "cumulative_turn_delta";
        const auto delta = addon::decode_usage_record(addon::encode_usage_record(turn_delta, 3), &failure, 3);
        SDK_CHECK(delta && delta->source_counter_semantics == "cumulative_turn_delta" &&
            delta->aggregate_evidence == "partial" && delta->total_tokens == 25,
            "usage3 preserves native turn-delta provenance without granting aggregate completeness");
        turn_delta.source_counter_semantics = "future_counter";
        SDK_CHECK(!addon::decode_usage_record(addon::encode_usage_record(turn_delta, 3), &failure, 3),
            "unknown usage3 provenance fails instead of being rewritten or omitted");
        const auto encoded = addon::encode_stored_usage_accounting(aggregate);
        SDK_CHECK(!addon::decode_usage_record(encoded, &failure, 3),
            "stored accounting alone cannot claim public aggregate evidence");
        const auto decoded = addon::decode_stored_usage_accounting(encoded, &failure);
        SDK_CHECK(decoded && decoded->record_id == "session-work" && decoded->input.value == 12 &&
            decoded->input.coverage->covered_records == 2 && decoded->input.coverage->total_records == 3 &&
            decoded->cache_read.coverage->support == addon::Usage_support::UNKNOWN &&
            decoded->cache_creation.coverage->support == addon::Usage_support::UNSUPPORTED &&
            decoded->repository_usage->unattributed_tokens == 7 && decoded->source_counter_semantics == "per_event_sum",
            "stored accounting preserves partial sums, category coverage, repository allocation and stable session identity");
        aggregate.repository_usage->allocations.front().total_tokens = 19;
        SDK_CHECK(!addon::decode_stored_usage_accounting(addon::encode_stored_usage_accounting(aggregate), &failure),
            "repository allocations must reconcile exactly before a usage snapshot can replace history");
        aggregate.repository_usage->allocations.front().total_tokens = 18;
        aggregate.input.coverage->overflow = true;
        SDK_CHECK(!addon::decode_stored_usage_accounting(addon::encode_stored_usage_accounting(aggregate), &failure),
            "overflow evidence cannot retain a fabricated known sum");
        aggregate.input.value.reset();
        SDK_CHECK(addon::decode_stored_usage_accounting(addon::encode_stored_usage_accounting(aggregate), &failure).has_value(),
            "overflow remains explicit without inventing a token count");
    }

    // Oracle: current paging, exact counts, category applicability and cache acknowledgment;
    // Gregorian dates and unsigned UTF-8 byte ordering supply fixed oracles.
    addon::Contract_failure failure;
    addon::Usage_scan_request request;
    request.context = launch_context();
    request.context.profile_storage = usage_page().profile_storage;
    request.cache_selection = addon::Usage_cache_selection{std::nullopt, usage_page().cache_candidate->root};
    SDK_CHECK(addon::decode_usage_scan_request(addon::encode_usage_scan_request(request), &failure).has_value(),
        "initial default-scope scan carries explicit storage authority without an invented account");
    request.snapshot_id = "snapshot-1";
    SDK_CHECK(!addon::decode_usage_scan_request(addon::encode_usage_scan_request(request), &failure),
        "continuation snapshot and cursor must be supplied together");
    request.cursor = "page-2";
    SDK_CHECK(addon::decode_usage_scan_request(addon::encode_usage_scan_request(request), &failure).has_value(),
        "a bounded continuation carries the frozen snapshot identity");
    const auto encoded = addon::encode_usage_scan_result(usage_page());
    const auto result = addon::decode_usage_scan_result(encoded, &failure);
    SDK_CHECK(result && result->records.front().total_tokens == 9007199254740991ULL &&
        result->records.front().input.value == 0 && !result->records.front().cache_read.value &&
        !result->records.front().source_path,
        "largest exact total, applicable zero, inapplicability and absent paths survive");
    auto invalid = encoded;
    invalid.set("complete", addon::Json_value(false));
    SDK_CHECK(!addon::decode_usage_scan_result(invalid, &failure), "unfinished paging requires a continuation cursor");
    invalid = encoded;
    invalid.set("next_cursor", addon::Json_value("page-2"));
    SDK_CHECK(!addon::decode_usage_scan_result(invalid, &failure), "terminal pages cannot retain a continuation cursor");
    for (const auto& total : {addon::Json_value(-1), addon::Json_value(1.5), addon::Json_value(static_cast<std::uint64_t>(9007199254740992ULL))}) {
        invalid = encoded;
        invalid.find("records")->as_array().front().set("total_tokens", total);
        SDK_CHECK(!addon::decode_usage_scan_result(invalid, &failure), "usage rejects nonexact or out-of-bound totals");
    }
    for (const char* date : {"2023-02-29", "1900-02-29", "2024-13-01", "0000-01-01", "2024-01-00"}) {
        invalid = encoded;
        invalid.find("records")->as_array().front().set("completed_date", addon::Json_value(date));
        SDK_CHECK(!addon::decode_usage_scan_result(invalid, &failure), "usage dates must be actual Gregorian dates");
    }
    invalid = encoded;
    invalid.find("records")->as_array().front().find("cache_read")->set("value", addon::Json_value(0));
    SDK_CHECK(!addon::decode_usage_scan_result(invalid, &failure), "inapplicable zero is not an observation");
    invalid = encoded;
    invalid.find("records")->as_array().front().find("input")->erase("value");
    SDK_CHECK(!addon::decode_usage_scan_result(invalid, &failure), "applicable dimensions require observed values");
    invalid = encoded;
    invalid.find("records")->as_array().front().set("source_path", addon::Json_value("fabricated/relative"));
    SDK_CHECK(!addon::decode_usage_scan_result(invalid, &failure), "present source paths must be real absolute paths");
    invalid = encoded;
    invalid.find("records")->push_back(invalid.find("records")->as_array().front());
    SDK_CHECK(!addon::decode_usage_scan_result(invalid, &failure), "duplicate IDs invalidate a page");
    auto page = usage_page();
    page.records.front().record_id = "z";
    auto second = page.records.front();
    second.record_id = "\xC3\xA9";
    page.records.push_back(second);
    SDK_CHECK(addon::decode_usage_scan_result(addon::encode_usage_scan_result(page), &failure).has_value(),
        "non-ASCII record ordering compares unsigned UTF-8 bytes");
    std::reverse(page.records.begin(), page.records.end());
    SDK_CHECK(!addon::decode_usage_scan_result(addon::encode_usage_scan_result(page), &failure),
        "descending page IDs are rejected");
    page.records.assign(addon::k_max_usage_page_records + 1, second);
    SDK_CHECK(!addon::decode_usage_scan_result(addon::encode_usage_scan_result(page), &failure),
        "a page cannot exceed its record bound");

    return true;
}

bool test_observation_fixture_subsets()
{
    for (const bool long_allocation : {false, true}) {
        addon::Simulator_options options;
        options.package_directory = fixture_packages_directory() /
            "org.example.unknown-fixture-provider-usage-cache-acceptance";
        addon::Host_simulator host(options, nullptr);
        addon::Contract_failure failure;
        SDK_CHECK(host.start(&failure), "usage3 fixture validates and handshakes through the real SDK runtime");
        std::string error;
        const auto root = addon::allocate_profile_storage_root(
            host.profile_storage_root(), host.package()->bootstrap.package_id, &error);
        SDK_CHECK(root.has_value(), "allocate acknowledged profile for usage3 fixture");
        auto candidate = addon::path_from_utf8(*root);
        while (long_allocation && candidate.native().size() < 291) {
            candidate /= "retained-account-cache-allocation-component";
        }
        candidate /= "scan-first";
        std::error_code allocation_error;
        const bool allocated = std::filesystem::create_directories(fixture_io_path(candidate), allocation_error);
        SDK_CHECK(allocated, "host candidate allocation: " + allocation_error.message());
        addon::Usage_scan_request request;
        request.context = launch_context();
        request.context.profile_storage = addon::Profile_storage_context{1, *root};
        request.context.launch_environment = addon::Launch_environment{
            addon::host_platform_id(), addon::Json_value::object(), std::nullopt};
        request.cache_selection = addon::Usage_cache_selection{std::nullopt, addon::path_to_utf8(candidate)};
        addon::Simulator_invocation_options invocation;
        invocation.profile_storage = request.context.profile_storage;
        const auto acknowledge_id = host.invoke({"profile-storage", 1}, "acknowledge",
            addon::Json_value::object(), 5000, invocation);
        const auto acknowledged = host.wait_outcome(acknowledge_id, 6000);
        SDK_CHECK(acknowledged && acknowledged->status == addon::Host_status::SUCCESS,
            "usage3 fixture acknowledges allocated profile before scanning");
        const auto first_id = host.invoke({"usage-scan", 3}, "scan",
            addon::encode_usage_scan_request(request, 3), 5000, invocation);
        const auto first = host.wait_outcome(first_id, 6000);
        SDK_CHECK(first && first->status == addon::Host_status::SUCCESS,
            first ? first->reason_code : "usage3 first page timed out");
        const auto page = addon::decode_usage_scan_result(first->payload, &failure, 3);
        SDK_CHECK(page && !page->complete && page->cache_candidate, "usage3 fixture returns sealed cache descriptor");
        request.snapshot_id = page->snapshot_id;
        request.cursor = page->next_cursor;
        const auto last_id = host.invoke({"usage-scan", 3}, "scan",
            addon::encode_usage_scan_request(request, 3), 5000, invocation);
        const auto last = host.wait_outcome(last_id, 6000);
        SDK_CHECK(last && last->status == addon::Host_status::SUCCESS, "usage3 continuation reaches fixture adapter");
        const auto terminal = addon::decode_usage_scan_result(last->payload, &failure, 3);
        SDK_CHECK(terminal && terminal->complete && terminal->cache_candidate == page->cache_candidate,
            "fixture pagination preserves candidate identity");
        SDK_CHECK(addon::validate_usage_cache_candidate(*terminal->cache_candidate, *root, std::nullopt,
                [] { return false; }, &failure).has_value(), "fixture cache is independently reopenable");
        SDK_CHECK(host.shutdown(1000, nullptr), "usage3 fixture exits cleanly");
    }
    // Oracle: independent capability presence must not imply a dependency on
    // accounts, a sibling observation capability, or structured sessions.
    const struct
    {
        const char* suffix;
        bool auth;
        bool quota;
        bool usage;
    }
    subsets[] = {
        {"auth",         true,  false, false},
        {"quota",        false, true,  false},
        {"usage",        false, false, true },
        {"auth-quota",   true,  true,  false},
        {"auth-usage",   true,  false, true },
        {"quota-usage",  false, true,  true },
        {"observations", true,  true,  true },
    };
    for (const auto& subset : subsets) {
        for (const bool accounts : {false, true}) {
            addon::Simulator_options options;
            options.package_directory = fixture_packages_directory() /
                (std::string("org.example.unknown-fixture-provider-") + subset.suffix + (accounts ? "-accounts" : ""));
            addon::Host_simulator host(options, nullptr);
            addon::Contract_failure failure;
            SDK_CHECK(host.start(&failure), "every independent observation subset starts as a real fixture package");
            const auto live = host.live_capabilities();
            const auto has = [&live](const char* id) {
                const std::uint16_t version = std::string_view(id) == addon::k_usage_scan_capability_id ? 3 : 2;
                return std::find(live.begin(), live.end(), addon::Capability_pair{id, version}) != live.end();
            };
            SDK_CHECK(has(addon::k_auth_status_capability_id) == subset.auth &&
                has(addon::k_quota_capability_id) == subset.quota &&
                has(addon::k_usage_scan_capability_id) == subset.usage &&
                has(addon::k_accounts_capability_id) == accounts,
                "live capability intersection preserves exactly the chosen subset");
            if (subset.usage) {
                addon::Usage_scan_request request;
                request.context = launch_context();
                std::string storage_error;
                const auto profile = addon::allocate_profile_storage_root(
                    host.profile_storage_root(), host.package()->bootstrap.package_id, &storage_error);
                SDK_CHECK(profile.has_value(), "allocate current subset cache scope: " + storage_error);
                request.context.profile_storage = addon::Profile_storage_context{1, *profile};
                const auto candidate = addon::path_from_utf8(*profile) / "subset-scan";
                SDK_CHECK(std::filesystem::create_directory(candidate), "allocate current subset candidate");
                request.cache_selection = addon::Usage_cache_selection{std::nullopt, addon::path_to_utf8(candidate)};
                addon::Simulator_invocation_options invocation;
                invocation.profile_storage = request.context.profile_storage;
                const auto acknowledge_id = host.invoke({"profile-storage", 1}, "acknowledge",
                    addon::Json_value::object(), 5000, invocation);
                const auto acknowledged = host.wait_outcome(acknowledge_id, 6000);
                SDK_CHECK(acknowledged && acknowledged->status == addon::Host_status::SUCCESS,
                    "subset acknowledges the actual allocated profile before scanning");
                if (accounts) {
                    request.context.account_context = addon::Account_context{1, "Primary"};
                }
                const auto first_id = host.invoke({addon::k_usage_scan_capability_id, 3}, addon::k_usage_scan_operation,
                    addon::encode_usage_scan_request(request), 5000, invocation);
                const auto first = host.wait_outcome(first_id, 6000);
                SDK_CHECK(first && first->status == addon::Host_status::SUCCESS, "usage-only package can scan its own scope");
                const auto page = addon::decode_usage_scan_result(first->payload, &failure);
                SDK_CHECK(page && !page->complete && page->records.size() == 1 &&
                    page->scope.source_account_key == (accounts ? std::optional<std::string>("Primary") : std::nullopt),
                    "first usage page preserves account selection or explicit package default");
                request.snapshot_id = page->snapshot_id;
                request.cursor      = page->next_cursor;
                const auto next_id = host.invoke({addon::k_usage_scan_capability_id, 3}, addon::k_usage_scan_operation,
                    addon::encode_usage_scan_request(request), 5000, invocation);
                const auto next = host.wait_outcome(next_id, 6000);
                SDK_CHECK(next && next->status == addon::Host_status::SUCCESS, "snapshot continuation succeeds");
                const auto last = addon::decode_usage_scan_result(next->payload, &failure);
                SDK_CHECK(last && last->complete && last->snapshot_id == page->snapshot_id &&
                    last->scope.observed_at_ms == page->scope.observed_at_ms && last->records.front().record_id == "record-02",
                    "last page retains snapshot metadata and advances stable record IDs");
            }
            SDK_CHECK(host.shutdown(1000, nullptr), "observation subset fixture exits cleanly");
        }
    }
    return true;
}

} // namespace sdk_tests
