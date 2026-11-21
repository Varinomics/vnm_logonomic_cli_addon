#include "sdk_test_support.h"
#include "logonomic_cli_addon/auth_status.h"
#include "logonomic_cli_addon/account_refresh.h"
#include "logonomic_cli_addon/quota.h"

// probe_observations.cpp as shipped before quota-failure@1 is compiled
// unchanged, so the mixed-version tests below exercise the strict quota@2
// reader and writer that deployed hosts and packages carry rather than a
// second imitation of them.
#define encode_observation_scope fixture_encode_observation_scope
#define decode_observation_scope fixture_decode_observation_scope
#define auth_status_name fixture_auth_status_name
#define quota_status_name fixture_quota_status_name
#define decode_current_account_declaration fixture_decode_current_account_declaration
#define encode_current_account_request fixture_encode_current_account_request
#define decode_current_account_request fixture_decode_current_account_request
#define encode_current_account_observation fixture_encode_current_account_observation
#define decode_current_account_observation fixture_decode_current_account_observation
#define encode_auth_inspect_result fixture_encode_auth_inspect_result
#define decode_auth_inspect_result fixture_decode_auth_inspect_result
#define encode_quota_inspect_result fixture_encode_quota_inspect_result
#define decode_quota_inspect_result fixture_decode_quota_inspect_result
#include "quota_2_codec_fixture.inc"
#undef encode_observation_scope
#undef decode_observation_scope
#undef auth_status_name
#undef quota_status_name
#undef decode_current_account_declaration
#undef encode_current_account_request
#undef decode_current_account_request
#undef encode_current_account_observation
#undef decode_current_account_observation
#undef encode_auth_inspect_result
#undef decode_auth_inspect_result
#undef encode_quota_inspect_result
#undef decode_quota_inspect_result

namespace sdk_tests {
namespace {

namespace addon = logonomic::cli_addon;

constexpr addon::Quota_failure k_failures[] = {
    addon::Quota_failure::AUTHENTICATION_REQUIRED, addon::Quota_failure::CREDENTIALS_EXPIRED,
    addon::Quota_failure::REQUEST_REJECTED,        addon::Quota_failure::REQUEST_FAILED,
    addon::Quota_failure::INVALID_RESPONSE,        addon::Quota_failure::TIMED_OUT,
    addon::Quota_failure::PROCESS_FAILED,          addon::Quota_failure::REGISTRY_FAILED,
};

} // namespace

// quota@2 readers and writers exist in deployed hosts and packages, so a cause
// beyond lost authentication may cross only when the host opted in.
bool test_quota_failure_compatibility()
{
    const addon::Observation_scope scope{"selected-profile", 1700000000000ULL, 60000};
    addon::Contract_failure failure;
    bool ok = true;
    for (const auto reason : k_failures) {
        const auto result = addon::make_quota_failure(reason, scope,
            addon::Quota_diagnostic{addon::Quota_failure_stage::PROCESS_EXIT, 17});
        const auto wire = addon::encode_quota_inspect_result(result, 2);
        const auto shipped = addon::fixture_decode_quota_inspect_result(wire, true, &failure, 2);
        ok &= check(shipped && shipped->observation &&
            shipped->observation->state == addon::Quota_status::FAILED &&
            shipped->observation->scope.source_account_key == scope.source_account_key &&
            shipped->failure_reason.has_value() == (reason == addon::Quota_failure::AUTHENTICATION_REQUIRED),
            __func__, "a quota@2 reader accepts a package failure and learns only lost authentication");
        const auto current = addon::decode_quota_inspect_result(wire, true, &failure, 2);
        ok &= check(shipped && current && current->failure_reason == shipped->failure_reason &&
            !current->diagnostic && !current->explanation_ignored,
            __func__, "the current reader draws the same conclusion from a failure nobody asked to explain");
    }

    addon::Quota_inspect_result shipped_result;
    shipped_result.complete = true;
    shipped_result.observation = addon::Quota_observation{
        scope, addon::Quota_status::FAILED, {}, {}, {}, {}, {}, {}};
    for (const bool authentication : {false, true}) {
        shipped_result.failure_reason = authentication
            ? std::optional<std::string>("authentication_required") : std::nullopt;
        const auto decoded = addon::decode_quota_inspect_result(
            addon::fixture_encode_quota_inspect_result(shipped_result, 2), true, &failure, 2);
        ok &= check(decoded && decoded->observation &&
            decoded->observation->state == addon::Quota_status::FAILED &&
            decoded->failure_reason == shipped_result.failure_reason && !decoded->explanation_ignored,
            __func__, "the current reader takes a quota@2 writer neutral failure and lost authentication");
    }

    for (const auto reason : k_failures) {
        for (const bool diagnosed : {false, true}) {
            auto result = addon::make_quota_failure(reason, scope);
            if (diagnosed) {
                result.diagnostic = addon::Quota_diagnostic{addon::Quota_failure_stage::HTTP_RESPONSE, 503};
            }
            const auto decoded = addon::decode_quota_inspect_result(
                addon::encode_quota_inspect_result(result, 2, true), true, &failure, 2);
            ok &= check(decoded && decoded->observation &&
                decoded->observation->state == addon::Quota_status::FAILED &&
                decoded->failure_reason == addon::quota_failure_reason(reason) &&
                decoded->diagnostic.has_value() == diagnosed && !decoded->explanation_ignored &&
                (!diagnosed || (decoded->diagnostic->stage == addon::Quota_failure_stage::HTTP_RESPONSE &&
                    decoded->diagnostic->code == 503)),
                __func__, "a failure explained at the host request keeps its cause and evidence");
        }
    }

    addon::Account_refresh_inspect_result refresh;
    refresh.complete = true;
    refresh.auth = addon::Auth_inspect_result{true, {},
        addon::Auth_observation{scope, addon::Auth_status::AUTHENTICATED}};
    refresh.quota = addon::make_quota_failure(addon::Quota_failure::TIMED_OUT, scope);
    const auto explained = addon::decode_account_refresh_inspect_result(
        addon::encode_account_refresh_inspect_result(refresh, true), true, &failure);
    ok &= check(explained && explained->quota &&
        explained->quota->failure_reason == addon::quota_failure_reason(addon::Quota_failure::TIMED_OUT) &&
        explained->auth && explained->auth->observation &&
        explained->auth->observation->state == addon::Auth_status::AUTHENTICATED,
        __func__, "a combined refresh explains its quota failure beside authentication");
    const auto unexplained = addon::decode_account_refresh_inspect_result(
        addon::encode_account_refresh_inspect_result(refresh), true, &failure);
    ok &= check(unexplained && unexplained->quota && unexplained->quota->observation &&
        unexplained->quota->observation->state == addon::Quota_status::FAILED &&
        !unexplained->quota->failure_reason && unexplained->auth && unexplained->auth->observation,
        __func__, "a combined refresh nobody asked to explain stays neutral and keeps authentication");

    // Evidence a reader cannot use leaves the failure neutral and the sibling
    // authentication intact. A newer package may send values this reader lacks.
    refresh.quota->failure_reason.reset();
    for (const int scenario : {0, 1, 2, 3, 4, 5}) {
        auto wire = addon::encode_account_refresh_inspect_result(refresh);
        auto explanation = addon::Json_value::object();
        explanation.set("version", addon::Json_value(scenario == 3 ? 17 : 1));
        explanation.set("reason", addon::Json_value(scenario == 0 ? "private-unknown-reason" : "quota_request_failed"));
        auto diagnostic = addon::Json_value::object();
        diagnostic.set("stage", addon::Json_value(scenario == 1 ? "private-unknown-stage" : "http_response"));
        diagnostic.set("code", addon::Json_value(503));
        if (scenario == 4) {
            diagnostic.set("private-member", addon::Json_value("private-output"));
        }
        explanation.set("diagnostic", scenario == 2 ? addon::Json_value("private-malformed") : diagnostic);
        if (scenario == 5) {
            explanation.set("private-member", addon::Json_value("private-output"));
        }
        wire.find("quota")->set("quota_failure", explanation);
        const auto decoded = addon::decode_account_refresh_inspect_result(wire, true, &failure);
        ok &= check(decoded && decoded->auth && decoded->auth->observation &&
            decoded->auth->observation->state == addon::Auth_status::AUTHENTICATED &&
            decoded->quota && decoded->quota->observation &&
            decoded->quota->observation->state == addon::Quota_status::FAILED &&
            decoded->quota->observation->scope.source_account_key == scope.source_account_key &&
            !decoded->quota->failure_reason && !decoded->quota->diagnostic && decoded->quota->explanation_ignored,
            __func__, "an unusable quota explanation leaves a neutral failure and the valid authentication");
    }

    shipped_result.failure_reason.reset();
    auto standalone = addon::encode_quota_inspect_result(shipped_result, 2);
    standalone.set("quota_failure", addon::Json_value("private-malformed"));
    const auto neutral = addon::decode_quota_inspect_result(standalone, true, &failure, 2);
    ok &= check(neutral && neutral->observation && !neutral->failure_reason && neutral->explanation_ignored,
        __func__, "an unusable explanation of a standalone quota failure leaves it neutral");
    auto misplaced = addon::encode_quota_inspect_result(
        addon::make_quota_failure(addon::Quota_failure::TIMED_OUT, scope), 2, true);
    misplaced.find("observation")->set("state", addon::Json_value("unavailable"));
    const auto unavailable = addon::decode_quota_inspect_result(misplaced, true, &failure, 2);
    ok &= check(unavailable && unavailable->observation &&
        unavailable->observation->state == addon::Quota_status::UNAVAILABLE &&
        !unavailable->failure_reason && unavailable->explanation_ignored,
        __func__, "an explanation beside an observation that did not fail is not used");

    addon::Probe_inspect_request request;
    request.inspection.final = true;
    const auto plain = addon::decode_probe_inspect_request(
        addon::encode_probe_inspect_request(request, 2), &failure, 2);
    ok &= check(plain && !plain->report_quota_failure,
        __func__, "a request that did not ask for explanations does not carry the member");
    request.report_quota_failure = true;
    const auto asked = addon::encode_probe_inspect_request(request, 2);
    const auto quota_request = addon::decode_probe_inspect_request(asked, &failure, 2);
    const auto refresh_request = addon::decode_account_refresh_inspect_request(
        addon::encode_account_refresh_inspect_request(request), &failure);
    ok &= check(quota_request && quota_request->report_quota_failure &&
        refresh_request && refresh_request->report_quota_failure,
        __func__, "a quota and a combined refresh request ask for explanations through the same member");
    auto unsupported = asked;
    unsupported.find("quota_failure")->set("version", addon::Json_value(2));
    const auto declined = addon::decode_probe_inspect_request(unsupported, &failure, 2);
    ok &= check(declined && !declined->report_quota_failure,
        __func__, "an unsupported request member leaves an ordinary inspect request usable");
    ok &= check(addon::decode_quota_failure_declaration(addon::Json_value::object(), &failure) &&
        !addon::decode_quota_failure_declaration(addon::Json_value("private"), &failure),
        __func__, "the capability declares an empty sidecar payload");
    return ok;
}

} // namespace sdk_tests
