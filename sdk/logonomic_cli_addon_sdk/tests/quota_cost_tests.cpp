#include "sdk_test_support.h"
#include "logonomic_cli_addon/account_refresh.h"
#include "logonomic_cli_addon/quota.h"

#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace logonomic::cli_addon {

// The existing compatibility fixture compiles the shipped strict quota@2
// reader, so omission is checked against the deployed grammar.
std::optional<Quota_inspect_result> fixture_decode_quota_inspect_result(
    const Json_value& payload, bool final_inspection, Contract_failure* out_failure, std::uint16_t version);

} // namespace logonomic::cli_addon

namespace sdk_tests {
namespace {

namespace addon = logonomic::cli_addon;
using Json = addon::Json_value;

addon::Quota_inspect_result cost_result(std::uint16_t version, addon::Quota_status state)
{
    addon::Quota_inspect_result result;
    result.complete = true;
    result.observation = addon::Quota_observation{
        {"selected-profile", 1700000000000ULL, 60000}, state, {}, {}, {}, {}, {}, {}};
    if (state == addon::Quota_status::READY) {
        const addon::Quota_window window{25, 300, 1700000060000ULL, "Five hours"};
        if (version == 2) {
            result.observation->primary = window;
        }
        else {
            result.observation->windows = std::vector<addon::Quota_window>{window};
        }
    }
    result.cost = addon::Quota_cost{6.6474052, "USD", true, "Recorded sessions", std::nullopt};
    return result;
}

bool same_observation(
    const std::optional<addon::Quota_inspect_result>& decoded,
    const addon::Quota_inspect_result& source,
    std::uint16_t version)
{
    return decoded && decoded->complete && decoded->observation &&
        decoded->observation->state == source.observation->state &&
        decoded->observation->scope.source_account_key == source.observation->scope.source_account_key &&
        addon::encode_quota_inspect_result(*decoded, version) == addon::encode_quota_inspect_result(source, version);
}

} // namespace

// The optional capability's contract, rather than UI layout, is the oracle:
// missing data is not zero, amounts retain their source scope, and an unusable
// cost must never invalidate a quota or sibling authentication observation.
bool test_quota_cost_contracts()
{
    addon::Contract_failure failure;
    bool ok = true;
    for (const std::uint16_t version : {2, 3}) {
        for (const auto state : {addon::Quota_status::READY, addon::Quota_status::UNAVAILABLE,
                addon::Quota_status::FAILED, addon::Quota_status::NOT_LOGGED_IN})
        {
            auto source = cost_result(version, state);
            const auto plain = addon::encode_quota_inspect_result(source, version);
            const auto absent = addon::decode_quota_inspect_result(plain, true, &failure, version);
            ok &= check(!plain.find("quota_cost") && same_observation(absent, source, version) &&
                !absent->cost && !absent->cost_failed && !absent->cost_ignored,
                __func__, "unrequested monetary evidence is omitted without changing ordinary quota");
            if (version == 2) {
                const auto shipped = addon::fixture_decode_quota_inspect_result(plain, true, &failure, version);
                ok &= check(same_observation(shipped, source, version),
                    __func__, "a deployed strict quota reader accepts the unrequested-cost result");
            }
            for (const bool has_available : {false, true}) {
                source.cost->available = has_available ? std::optional<double>(18.75) : std::nullopt;
                const auto wire = addon::encode_quota_inspect_result(source, version, false, true);
                const auto parsed = addon::parse_json(addon::serialize_json(wire), {}, nullptr);
                const auto decoded = parsed
                    ? addon::decode_quota_inspect_result(*parsed, true, &failure, version) : std::nullopt;
                ok &= check(same_observation(decoded, source, version) && decoded->cost &&
                    !decoded->cost_ignored && !decoded->cost_failed &&
                    decoded->cost->amount    == source.cost->amount &&
                    decoded->cost->currency  == "USD" && decoded->cost->estimated &&
                    decoded->cost->label     == "Recorded sessions" &&
                    decoded->cost->available == source.cost->available,
                    __func__, "negotiated cost preserves amount, currency, estimate basis, scope and optional available funds");
            }
            source.cost->amount    = 0;
            source.cost->available = 0;
            source.cost->estimated = false;
            const auto zero = addon::decode_quota_inspect_result(
                addon::encode_quota_inspect_result(source, version, false, true), true, &failure, version);
            ok &= check(zero && zero->cost && zero->cost->amount == 0 && zero->cost->available == 0 &&
                !zero->cost->estimated && !zero->cost_ignored,
                __func__, "explicit source-reported zero and billed basis survive without inventing missing values");

            source.cost.reset();
            source.cost_failed = true;
            const auto unreadable = addon::decode_quota_inspect_result(
                addon::encode_quota_inspect_result(source, version, false, true), true, &failure, version);
            ok &= check(same_observation(unreadable, source, version) && unreadable->cost_failed &&
                !unreadable->cost && !unreadable->cost_ignored,
                __func__, "a cost source failure is distinct from absence and preserves core quota");
        }
    }

    const auto source = cost_result(3, addon::Quota_status::READY);
    const auto wire   = addon::encode_quota_inspect_result(source, 3, false, true);
    const auto cost   = *wire.find("quota_cost");
    const auto ignored = [&](Json evidence) {
        auto altered = wire;
        altered.set("quota_cost", std::move(evidence));
        const auto decoded = addon::decode_quota_inspect_result(altered, true, &failure, 3);
        return same_observation(decoded, source, 3) && !decoded->cost &&
            !decoded->cost_failed && decoded->cost_ignored;
    };
    for (const auto* member : {"version", "amount", "currency", "estimated", "label"}) {
        auto missing = cost;
        missing.erase(member);
        ok &= check(ignored(std::move(missing)), __func__, "missing cost members do not fabricate a monetary value");
    }
    for (const auto* member : {"amount", "available"}) {
        for (const double amount : {-0.01, std::numeric_limits<double>::infinity(),
                std::numeric_limits<double>::quiet_NaN()})
        {
            auto invalid = cost;
            invalid.set(member, Json(amount));
            ok &= check(ignored(std::move(invalid)), __func__, "monetary amounts must be finite and nonnegative");
        }
        for (const auto& amount : {Json(), Json("3.50"), Json(false)}) {
            auto invalid = cost;
            invalid.set(member, amount);
            ok &= check(ignored(std::move(invalid)), __func__, "monetary amounts must use numeric source values");
        }
    }
    for (const auto* currency : {"", "usd", "US", "USDD", "US1"}) {
        auto invalid = cost;
        invalid.set("currency", Json(currency));
        ok &= check(ignored(std::move(invalid)), __func__, "currency uses three uppercase ASCII letters");
    }
    for (const auto& label : {std::string{}, std::string(129, 'x'), std::string("Recorded\nsessions")}) {
        auto invalid = cost;
        invalid.set("label", Json(label));
        ok &= check(ignored(std::move(invalid)), __func__, "cost scope follows the quota label bound");
    }
    auto invalid = cost;
    invalid.set("estimated", Json("true"));
    ok &= check(ignored(invalid), __func__, "estimate provenance is explicit boolean evidence");
    invalid = cost;
    invalid.set("version", Json(2));
    ok &= check(ignored(invalid), __func__, "unknown cost versions remain capability-local");
    invalid = cost;
    invalid.set("extra", Json("unknown"));
    ok &= check(ignored(invalid), __func__, "unknown cost fields do not invalidate quota");
    invalid = cost;
    invalid.set("error", Json(true));
    ok &= check(ignored(invalid), __func__, "cost values and a source failure cannot be mixed");
    invalid = addon::encode_quota_cost_request();
    invalid.set("error", Json(false));
    ok &= check(ignored(invalid), __func__, "a false failure flag is not a monetary observation");
    ok &= check(ignored(Json("malformed")), __func__, "malformed cost evidence stays separate from quota");

    for (const auto reason : {addon::Quota_failure::TIMED_OUT, addon::Quota_failure::AUTHENTICATION_REQUIRED}) {
        auto failed = addon::make_quota_failure(reason, source.observation->scope);
        failed.cost = source.cost;
        auto output = addon::encode_quota_inspect_result(failed, 3, true, true);
        const auto decoded = addon::decode_quota_inspect_result(output, true, &failure, 3);
        ok &= check(same_observation(decoded, failed, 3) && decoded->cost &&
            decoded->cost->amount == source.cost->amount && !decoded->cost_ignored &&
            decoded->failure_reason == addon::quota_failure_reason(reason),
            __func__, "recorded cost survives a classified quota failure without changing its cause");
        output.set("quota_cost", Json("malformed"));
        const auto malformed = addon::decode_quota_inspect_result(output, true, &failure, 3);
        ok &= check(same_observation(malformed, failed, 3) && !malformed->cost && malformed->cost_ignored &&
            malformed->failure_reason == addon::quota_failure_reason(reason),
            __func__, "unusable independent cost evidence leaves a classified quota failure intact");
    }
    addon::Quota_inspect_result pending;
    pending.cost = source.cost;
    auto pending_wire = addon::encode_quota_inspect_result(pending, 3, false, true);
    ok &= check(!pending_wire.find("quota_cost"),
        __func__, "an incomplete result cannot publish cost without a completed account observation");
    pending_wire.set("quota_cost", cost);
    const auto incomplete = addon::decode_quota_inspect_result(pending_wire, false, &failure, 3);
    ok &= check(incomplete && !incomplete->complete && !incomplete->cost && incomplete->cost_ignored,
        __func__, "incomplete quota cannot publish cost before its account observation");
    pending.complete = true;
    const auto unscoped = addon::encode_quota_inspect_result(pending, 3, false, true);
    ok &= check(!unscoped.find("quota_cost") && !addon::decode_quota_inspect_result(unscoped, true, &failure, 3),
        __func__, "a completed result still requires its account-scoped observation before cost can be used");

    auto conflicting = source;
    conflicting.cost_failed = true;
    bool rejected = false;
    try {
        (void)addon::encode_quota_inspect_result(conflicting, 3, false, true);
    }
    catch (const std::invalid_argument&) {
        rejected = true;
    }
    ok &= check(rejected, __func__, "a producer cannot encode both cost success and failure");

    addon::Probe_inspect_request request;
    request.inspection.final = true;
    request.report_quota_failure = true;
    const auto plain_request = addon::encode_probe_inspect_request(request);
    const auto plain_decoded = addon::decode_probe_inspect_request(plain_request, &failure);
    ok &= check(!plain_request.find("quota_cost") && plain_decoded &&
        !plain_decoded->report_quota_cost && plain_decoded->report_quota_failure,
        __func__, "failure negotiation does not opt in to monetary evidence");
    request.report_quota_cost = true;
    const auto asked = addon::encode_probe_inspect_request(request);
    const auto quota_request = addon::decode_probe_inspect_request(asked, &failure);
    const auto refresh_request = addon::decode_account_refresh_inspect_request(
        addon::encode_account_refresh_inspect_request(request), &failure);
    ok &= check(quota_request && quota_request->report_quota_cost && quota_request->report_quota_failure &&
        refresh_request && refresh_request->report_quota_cost,
        __func__, "quota and combined refresh negotiate cost independently through the same request");
    for (const auto& evidence : {Json(), Json(true), Json("unknown"), Json::object()}) {
        auto malformed = asked;
        malformed.set("quota_cost", evidence);
        const auto decoded = addon::decode_probe_inspect_request(malformed, &failure);
        ok &= check(decoded && !decoded->report_quota_cost && decoded->report_quota_failure,
            __func__, "malformed optional cost negotiation preserves the ordinary request");
    }
    auto unsupported = asked;
    unsupported.find("quota_cost")->set("version", Json(2));
    const auto declined = addon::decode_probe_inspect_request(unsupported, &failure);
    ok &= check(declined && !declined->report_quota_cost && declined->report_quota_failure,
        __func__, "unsupported cost negotiation does not disable failure evidence");
    ok &= check(addon::decode_quota_cost_declaration(Json::object(), &failure) &&
        !addon::decode_quota_cost_declaration(Json("invalid"), &failure),
        __func__, "quota-cost declares an empty sidecar payload");

    addon::Account_refresh_inspect_result refresh;
    refresh.complete = true;
    refresh.quota = cost_result(2, addon::Quota_status::UNAVAILABLE);
    refresh.auth = addon::Auth_inspect_result{true, {},
        addon::Auth_observation{refresh.quota->observation->scope, addon::Auth_status::AUTHENTICATED}};
    auto refresh_wire = addon::encode_account_refresh_inspect_result(refresh, false, true);
    const auto combined = addon::decode_account_refresh_inspect_result(refresh_wire, true, &failure);
    ok &= check(combined && combined->quota && combined->quota->cost && combined->auth &&
        combined->auth->observation->state == addon::Auth_status::AUTHENTICATED,
        __func__, "combined refresh preserves monetary evidence beside successful authentication");
    refresh_wire.find("quota")->set("quota_cost", Json("malformed"));
    const auto isolated = addon::decode_account_refresh_inspect_result(refresh_wire, true, &failure);
    ok &= check(isolated && isolated->quota && isolated->quota->cost_ignored && !isolated->quota->cost &&
        isolated->auth && isolated->auth->observation->state == addon::Auth_status::AUTHENTICATED,
        __func__, "malformed cost preserves a combined refresh's authentication result");
    return ok;
}

} // namespace sdk_tests
