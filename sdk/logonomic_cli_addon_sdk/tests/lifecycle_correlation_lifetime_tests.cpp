#include "sdk_test_support.h"

#include "logonomic_cli_addon/capability_ids.h"
#include "logonomic_cli_addon/lifecycle_correlation_lifetime.h"

namespace sdk_tests {

bool test_lifecycle_correlation_lifetime_contracts()
{
    namespace addon = logonomic::cli_addon;
    constexpr const char* test = "lifecycle_correlation_lifetime_contracts";
    addon::Contract_failure failure;
    const addon::Session_correlation correlation{
        "0123456789abcdef0123456789abcdef"};

    const addon::Lifecycle_correlation_reserve_request reserve{correlation};
    const auto decoded_reserve = addon::decode_lifecycle_correlation_reserve_request(
        addon::encode_lifecycle_correlation_reserve_request(reserve), &failure);
    if (!check(decoded_reserve && decoded_reserve->correlation.transition_nonce ==
            correlation.transition_nonce, test,
            "reserve preserves the exact lifecycle correlation nonce")) return false;
    auto malformed_reserve = addon::encode_lifecycle_correlation_reserve_request(reserve);
    malformed_reserve.set("extra", addon::Json_value(true));
    if (!check(!addon::decode_lifecycle_correlation_reserve_request(malformed_reserve, &failure), test,
            "reserve rejects an extra member")) return false;

    const addon::Lifecycle_correlation_retire_request abandoned{
        correlation,
        addon::Lifecycle_correlation_retire_disposition::ABANDONED,
        std::nullopt,
    };
    const auto decoded_abandoned = addon::decode_lifecycle_correlation_retire_request(
        addon::encode_lifecycle_correlation_retire_request(abandoned), &failure);
    if (!check(decoded_abandoned && !decoded_abandoned->consumed_cursor, test,
            "abandoned retirement has no terminal cursor")) return false;

    const addon::Lifecycle_correlation_retire_request consumed{
        correlation,
        addon::Lifecycle_correlation_retire_disposition::TERMINAL_CONSUMED,
        std::string("final-page"),
    };
    const auto decoded_consumed = addon::decode_lifecycle_correlation_retire_request(
        addon::encode_lifecycle_correlation_retire_request(consumed), &failure);
    if (!check(decoded_consumed && decoded_consumed->consumed_cursor == consumed.consumed_cursor, test,
            "terminal-consumed retirement carries its exact final cursor")) return false;
    auto missing_cursor = addon::encode_lifecycle_correlation_retire_request(consumed);
    missing_cursor.as_object().pop_back();
    if (!check(!addon::decode_lifecycle_correlation_retire_request(missing_cursor, &failure), test,
            "terminal-consumed retirement requires a cursor")) return false;
    auto abandoned_cursor = addon::encode_lifecycle_correlation_retire_request(abandoned);
    abandoned_cursor.set("consumed_cursor", addon::Json_value("final-page"));
    if (!check(!addon::decode_lifecycle_correlation_retire_request(abandoned_cursor, &failure), test,
            "abandoned retirement cannot claim a terminal cursor")) return false;

    const addon::Json_value ack = addon::encode_lifecycle_correlation_lifetime_ack();
    if (!check(addon::decode_lifecycle_correlation_lifetime_ack(ack, &failure).has_value(), test,
            "an empty operation acknowledgment is accepted")) return false;
    auto nonempty_ack = ack;
    nonempty_ack.set("transition_nonce", addon::Json_value(correlation.transition_nonce));
    if (!check(!addon::decode_lifecycle_correlation_lifetime_ack(nonempty_ack, &failure), test,
            "an acknowledgment cannot carry a second correlation channel")) return false;

    const addon::Capability_pair lifecycle{
        addon::k_lifecycle_events_capability_id,
        addon::k_lifecycle_events_version,
    };
    for (const std::uint16_t helper_version : {
             addon::k_helper_ingress_degradation_version,
             addon::k_helper_ingress_subagent_reconciliation_version,
         })
    {
        if (!check(addon::validate_lifecycle_correlation_lifetime_dependency({
                lifecycle,
                {addon::k_helper_ingress_capability_id, helper_version},
            }, &failure), test,
            "each known live helper-ingress version admits correlation lifetime")) return false;
    }
    return check(!addon::validate_lifecycle_correlation_lifetime_dependency(
            {lifecycle}, &failure) &&
            failure.reason_code == addon::reason::k_dependency_capability_unavailable,
        test, "correlation lifetime refuses a generation without helper ingress");
}

} // namespace sdk_tests
