#include "sdk_test_support.h"

#include "logonomic_cli_addon/usage_session.h"

#include <string>
#include <vector>

namespace sdk_tests {

bool test_usage_session_contracts()
{
    namespace addon = logonomic::cli_addon;
    const char* id = __func__;
    bool ok = true;
    addon::Contract_failure failure;
    const addon::Usage_session_fact fact{"native-session", true};
    const auto encoded_fact = addon::encode_usage_session_fact(fact);
    const auto decoded_fact = addon::decode_usage_session_fact(encoded_fact, &failure);
    ok &= check(decoded_fact && *decoded_fact == fact, id, "explicit lifecycle identity and cutoff round trip");
    auto invalid_fact = encoded_fact;
    invalid_fact.set("version", addon::Json_value(2));
    ok &= check(!addon::decode_usage_session_fact(invalid_fact, &failure), id, "unknown version establishes no fact");
    invalid_fact = encoded_fact;
    invalid_fact.set("excludes_turn_in_flight", addon::Json_value("true"));
    ok &= check(!addon::decode_usage_session_fact(invalid_fact, &failure), id, "cutoff requires a boolean");
    invalid_fact = encoded_fact;
    invalid_fact.set("provider_session_id", addon::Json_value(std::string(257, 'a')));
    ok &= check(!addon::decode_usage_session_fact(invalid_fact, &failure), id, "lifecycle identity is bounded");
    const std::vector<addon::Usage_session_identity> records{
        {"kimi:native-session", "kimi:native-session"},
        {"unattributed-record", std::nullopt},
    };
    const auto request = addon::encode_usage_session_request(records);
    const auto decoded_request = addon::decode_usage_session_request(request, &failure);
    ok &= check(decoded_request && *decoded_request == records, id, "opaque record and session keys round trip");
    auto duplicate = records;
    duplicate.push_back(records.front());
    ok &= check(!addon::decode_usage_session_request(addon::encode_usage_session_request(duplicate), &failure),
        id, "duplicate record IDs cannot produce ambiguous descriptions");
    ok &= check(!addon::decode_usage_session_request(addon::encode_usage_session_request({}), &failure),
        id, "empty requests are not sent");
    std::vector<addon::Usage_session_identity> excess;
    for (std::size_t index = 0; index <= addon::k_max_usage_page_records; ++index) {
        excess.push_back({"record-" + std::to_string(index), std::nullopt});
    }
    ok &= check(!addon::decode_usage_session_request(addon::encode_usage_session_request(excess), &failure),
        id, "one operation cannot exceed a scan page");
    const std::vector<addon::Usage_session_description> descriptions{
        {records.front(), fact},
        {records.back(), std::nullopt},
    };
    const auto result = addon::decode_usage_session_result(addon::encode_usage_session_result(descriptions), &failure);
    ok &= check(result && result->size() == records.size() && result->front().identity == records.front() &&
            result->front().session == fact && result->back().identity == records.back() && !result->back().session,
        id, "one positional response preserves recognized and unknown identities");
    ok &= check(addon::decode_usage_session_declaration(addon::Json_value::object(), &failure),
        id, "independent declaration is an empty object");
    return ok;
}

} // namespace sdk_tests
