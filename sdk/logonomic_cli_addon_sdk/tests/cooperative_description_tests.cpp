#include "sdk_test_support.h"

#include "logonomic_cli_addon/capability_ids.h"
#include "logonomic_cli_addon/cooperative_description.h"
#include "logonomic_cli_addon/helper_ingress.h"
#include "logonomic_cli_addon/limits.h"

namespace sdk_tests {
namespace {
namespace addon = logonomic::cli_addon;

addon::Description_submission title()
{
    return {"native-root", "0123456789abcdef0123456789abcdef", "native-tool-call-1",
        addon::Description_kind::TITLE, "Session subject", std::nullopt};
}

bool explicit_model_actions()
{
    auto submission = title();
    SDK_CHECK(addon::decode_description_submission(addon::encode_description_submission(submission)) == submission,
        "an explicit model title retains its native activation and invocation identity");
    submission.kind = addon::Description_kind::SUMMARY;
    submission.summary = "## Working state\n- Implemented the integration.\n- Awaiting review.\n";
    SDK_CHECK(addon::decode_description_submission(addon::encode_description_submission(submission)) == submission,
        "a working summary may carry the host's optional revised title");
    submission.kind = addon::Description_kind::SUMMARY_UNCHANGED;
    SDK_CHECK(!addon::is_valid_description_submission(submission), "keep-summary cannot also replace summary text");
    submission.summary.reset();
    SDK_CHECK(addon::is_valid_description_submission(submission), "an explicit review can keep the recorded summary");
    submission.kind = addon::Description_kind::TITLE;
    submission.title.reset();
    SDK_CHECK(!addon::is_valid_description_submission(submission), "a title action requires title text");

    for (const auto& invalid : {std::string{}, std::string("line\nline"), std::string("bad\xff", 4),
            std::string(addon::limits::k_max_cooperative_title_bytes + 1, 'x')})
    {
        submission = title();
        submission.title = invalid;
        SDK_CHECK(!addon::is_valid_description_submission(submission), "invalid title bytes cannot reach host policy");
    }
    submission = title();
    submission.kind = addon::Description_kind::SUMMARY;
    submission.summary = "text\x1b[31m";
    SDK_CHECK(!addon::is_valid_description_submission(submission), "terminal controls are not working-summary text");
    submission.summary = std::string(addon::limits::k_max_cooperative_summary_bytes, '\n');
    SDK_CHECK(!addon::is_valid_description_submission(submission), "escaped wire size has its own bounded limit");
    submission.summary = std::string(addon::limits::k_max_cooperative_summary_bytes + 1, 'x');
    SDK_CHECK(!addon::is_valid_description_submission(submission), "summary byte bounds apply before host policy");
    submission = title();
    submission.transition_nonce = "other-activation";
    SDK_CHECK(!addon::is_valid_description_submission(submission), "native activation must be an exact transition nonce");
    auto payload = addon::encode_description_submission(title());
    payload.set("completed", addon::Json_value(true));
    SDK_CHECK(!addon::decode_description_submission(payload), "a description cannot smuggle completion policy");
    return true;
}

bool independent_negotiation()
{
    addon::Classify_event_request request;
    request.event.name = "native_model_tool";
    request.transition_nonce = title().transition_nonce;
    auto encoded = addon::encode_classify_event_request(request);
    SDK_CHECK(!encoded.find("cooperative_description"), "older hosts receive no new optional envelope");
    request.cooperative_description_version = addon::k_cooperative_description_version;
    encoded = addon::encode_classify_event_request(request);
    auto decoded = addon::decode_classify_event_request(encoded, nullptr);
    SDK_CHECK(decoded && decoded->cooperative_description_version == 1 && !decoded->description_failure,
        "description return negotiates independently of helper ingress versions");
    encoded.find("cooperative_description")->set("version", addon::Json_value(2));
    decoded = addon::decode_classify_event_request(encoded, nullptr);
    SDK_CHECK(decoded && decoded->cooperative_description_version == 0 && decoded->description_failure,
        "an unknown optional version does not invalidate ordinary lifecycle classification");

    addon::Classify_event_result result;
    result.description_submission = title();
    auto classified = addon::decode_classify_event_result(addon::encode_classify_event_result(result), nullptr);
    SDK_CHECK(classified && classified->classification == addon::Classification::IGNORED &&
        classified->observations.empty() && classified->description_submission == title(),
        "an explicit submission creates no lifecycle observation");
    encoded = addon::encode_classify_event_result(result);
    encoded.find("cooperative_description")->find("submission")->set("summary", addon::Json_value("foreign"));
    classified = addon::decode_classify_event_result(encoded, nullptr);
    SDK_CHECK(classified && !classified->description_submission && classified->description_failure,
        "a malformed optional action cannot reach the description consumer");

    result.classification = addon::Classification::EVENT;
    addon::Lifecycle_observation started;
    started.provider_event_name = "native_start";
    started.phase_transition = addon::Phase_transition::WORKING;
    started.phase_evidence = addon::Evidence::CONFIRMED;
    result.observations.push_back(started);
    classified = addon::decode_classify_event_result(addon::encode_classify_event_result(result), nullptr);
    SDK_CHECK(classified && classified->observations.size() == 1 && !classified->description_submission &&
        classified->description_failure, "a mixed description action is isolated from valid root lifecycle evidence");
    return true;
}

bool actual_policy_receipts()
{
    addon::Description_receipt receipt{title().request_id, true, ""};
    SDK_CHECK(addon::decode_description_receipt(addon::encode_description_receipt(receipt)) == receipt,
        "host acceptance names the exact model invocation");
    receipt.accepted = false;
    receipt.reason = "already_assigned";
    SDK_CHECK(addon::decode_description_receipt(addon::encode_description_receipt(receipt)) == receipt,
        "an existing title policy refusal remains distinct from carrier admission");
    receipt.reason.clear();
    SDK_CHECK(!addon::decode_description_receipt(addon::encode_description_receipt(receipt)),
        "a refusal requires an attributable reason");
    auto payload = addon::encode_description_receipt({title().request_id, true, ""});
    payload.set("allow_completion", addon::Json_value(true));
    SDK_CHECK(!addon::decode_description_receipt(payload), "description receipts cannot authorize completion");
    return true;
}

} // namespace

bool test_cooperative_description_contracts()
{
    return explicit_model_actions() && independent_negotiation() && actual_policy_receipts();
}

} // namespace sdk_tests
