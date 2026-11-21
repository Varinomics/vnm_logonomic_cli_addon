#include "sdk_test_support.h"

#include "logonomic_cli_addon/limits.h"
#include "logonomic_cli_addon/package_source.h"
#include "logonomic_cli_addon/usage_owned_event_correction.h"

#include <filesystem>
#include <string>
#include <utility>

namespace sdk_tests {
namespace {

namespace addon = logonomic::cli_addon;
using Json = addon::Json_value;

std::string profile_root()
{
    return addon::path_to_utf8(std::filesystem::temp_directory_path() / "owned-event-profile");
}

std::string profile_path(const char* name)
{
    return addon::path_to_utf8(addon::path_from_utf8(profile_root()) / name);
}

addon::Usage_owned_event_correction_request request()
{
    addon::Usage_owned_event_correction_request value;
    value.scan.context.launcher      = {"kimi", addon::k_runner_direct, {}};
    value.scan.context.launcher_argv = {"kimi"};
    value.scan.context.profile_storage = addon::Profile_storage_context{1, profile_root()};
    value.scan.context.launch_environment = addon::Launch_environment{
        addon::host_platform_id(), Json::object(), std::nullopt};
    value.scan.context.account_context = addon::Account_context{1, "work-account", std::nullopt};
    value.scan.cache_selection = addon::Usage_cache_selection{std::nullopt, profile_path("candidate")};
    value.predecessors = {profile_path("predecessors.jsonl"), std::string(64, 'a')};
    return value;
}

addon::Usage_record record()
{
    addon::Usage_record value;
    value.record_id          = "fork-session-23";
    value.session_key        = "native-session-23";
    value.completed_date     = "2026-10-18";
    value.total_tokens       = 200;
    value.source_path        = profile_path("wire.jsonl");
    value.aggregate_evidence = "partial";
    value.input = {true, 130, addon::Usage_field_coverage{3, 3, false, addon::Usage_support::SUPPORTED}};
    value.output = {true, 70, addon::Usage_field_coverage{3, 3, false, addon::Usage_support::SUPPORTED}};
    value.cache_read = {true, 0, addon::Usage_field_coverage{3, 3, false, addon::Usage_support::SUPPORTED}};
    value.cache_creation = {true, std::nullopt,
        addon::Usage_field_coverage{0, 3, false, addon::Usage_support::UNKNOWN}};
    value.repository_usage = addon::Usage_repository{{}, 200};
    addon::Usage_temporal temporal;
    temporal.parent = {value.record_id, 200, "accepted-snapshot-9", "work-account"};
    temporal.portions = {{"instant", "2026-10-18T08:15:00Z", "", "", 200}};
    value.temporal = std::move(temporal);
    value.capabilities.set("other-proof", Json::object());
    value.capabilities.find("other-proof")->set("version", Json(7));
    return value;
}

addon::Usage_owned_event_correction correction()
{
    addon::Usage_owned_event_correction value;
    value.disposition            = "correct";
    value.predecessor_sha256_hex = std::string(64, 'b');
    value.coverage_policy        = "records";
    value.original_record_ids    = {"creator-session-17", "creator-session-18"};
    addon::Usage_owned_event_removal removed;
    removed.total_tokens  = 150;
    removed.total_records = 2;
    removed.input          = {100, 2};
    removed.output         = {50, 2};
    removed.cache_read     = {20, 1};
    removed.cache_creation = {0, 0};
    removed.repository_usage = {{{profile_path("repository"), 100}}, 50};
    removed.temporal = addon::Usage_owned_event_temporal_removal{
        {{"instant", "2026-10-18T08:15:00Z", "", "", 100},
         {"source-calendar", "", "2026-10-18", "native-local", 20}}, 30};
    value.removed = std::move(removed);
    return value;
}

bool request_transport_context()
{
    addon::Contract_failure failure;
    const auto input = request();
    const auto wire = addon::encode_usage_owned_event_correction_request(input);
    auto injected = wire;
    // The host proxy inserts trusted invocation context at the payload's top level.
    injected.set(addon::k_account_context_member, addon::encode_account_context(*input.scan.context.account_context));
    injected.set(addon::k_profile_storage_member, addon::profile_storage_member(*input.scan.context.profile_storage));
    SDK_CHECK(addon::decode_usage_owned_event_correction_request(injected, &failure).has_value(),
        "the wrapper accepts the host proxy's top-level account and profile injection");
    SDK_CHECK(wire == injected, "the encoder emits the common top-level invocation contexts");

    for (const char* member : {addon::k_account_context_member, addon::k_profile_storage_member}) {
        auto invalid = wire;
        invalid.erase(member);
        SDK_CHECK(!addon::decode_usage_owned_event_correction_request(invalid, &failure),
            "transport context cannot be omitted while the nested scan selects it");
        for (const auto& malformed : {Json(), Json("context"), Json::object()}) {
            invalid = wire;
            invalid.set(member, malformed);
            SDK_CHECK(!addon::decode_usage_owned_event_correction_request(invalid, &failure),
                "transport context requires the existing common object grammar");
        }
        invalid = wire;
        invalid.find(member)->set("extra", Json(true));
        SDK_CHECK(!addon::decode_usage_owned_event_correction_request(invalid, &failure),
            "common transport contexts retain their closed member sets");
    }
    for (const auto& account : {addon::Account_context{1, "other-account", std::nullopt},
             addon::Account_context{2, "work-account", std::nullopt}})
    {
        injected = wire;
        injected.set(addon::k_account_context_member, addon::encode_account_context(account));
        SDK_CHECK(!addon::decode_usage_owned_event_correction_request(injected, &failure),
            "the nested scan cannot override the host's account key or context version");
    }
    for (const auto& storage : {addon::Profile_storage_context{2, profile_root()},
             addon::Profile_storage_context{1, profile_root() + "-other"}})
    {
        injected = wire;
        injected.set(addon::k_profile_storage_member, addon::profile_storage_member(storage));
        SDK_CHECK(!addon::decode_usage_owned_event_correction_request(injected, &failure),
            "the nested scan cannot override the host's profile root or context version");
    }

    auto profiled = input;
    profiled.scan.context.account_context = addon::Account_context{2, "work-account", profile_path("account")};
    const auto profiled_wire = addon::encode_usage_owned_event_correction_request(profiled);
    SDK_CHECK(addon::decode_usage_owned_event_correction_request(profiled_wire, &failure).has_value(),
        "matching account version two preserves its selected profile path");
    for (const auto& path : {Json(), Json(profile_path("other-account"))}) {
        injected = profiled_wire;
        injected.find(addon::k_account_context_member)->set("profile_path", path);
        SDK_CHECK(!addon::decode_usage_owned_event_correction_request(injected, &failure),
            "the nested account profile path must equal the host's selection including null");
    }
    profiled.scan.context.account_context->profile_path.reset();
    SDK_CHECK(addon::decode_usage_owned_event_correction_request(
            addon::encode_usage_owned_event_correction_request(profiled), &failure).has_value(),
        "matching account version two permits its explicit null profile path");

    auto unselected = input;
    unselected.scan.context.account_context.reset();
    const auto unselected_wire = addon::encode_usage_owned_event_correction_request(unselected);
    SDK_CHECK(!unselected_wire.find(addon::k_account_context_member) &&
            !unselected_wire.find("scan")->find(addon::k_account_context_member) &&
            addon::decode_usage_owned_event_correction_request(unselected_wire, &failure),
        "an absent account is omitted consistently at both levels");
    for (const auto& account : {Json(), addon::encode_account_context(*input.scan.context.account_context)}) {
        injected = unselected_wire;
        injected.set(addon::k_account_context_member, account);
        SDK_CHECK(!addon::decode_usage_owned_event_correction_request(injected, &failure),
            "a transport account cannot be present while the nested scan omits it");
    }
    auto invalid = wire;
    invalid.erase(addon::k_profile_storage_member);
    invalid.find("scan")->erase(addon::k_profile_storage_member);
    SDK_CHECK(!addon::decode_usage_owned_event_correction_request(invalid, &failure),
        "matching absence cannot bypass the nested scan's required profile storage");
    invalid = wire;
    invalid.set("extra", Json(true));
    SDK_CHECK(!addon::decode_usage_owned_event_correction_request(invalid, &failure),
        "the wrapper admits only its operation fields and the common transport contexts");
    return true;
}

bool request_contract()
{
    addon::Contract_failure failure;
    SDK_CHECK(addon::decode_usage_owned_event_correction_declaration(Json::object(), &failure),
        "the capability has an empty declaration");
    auto invalid = Json::object();
    invalid.set("scan", Json(true));
    SDK_CHECK(!addon::decode_usage_owned_event_correction_declaration(invalid, &failure),
        "the declaration has no settings");

    auto input = request();
    const auto wire = addon::encode_usage_owned_event_correction_request(input);
    const auto decoded = addon::decode_usage_owned_event_correction_request(wire, &failure);
    SDK_CHECK(decoded && decoded->scan.mode == "incremental" && decoded->scan.cache_selection &&
            !decoded->scan.cache_selection->accepted && decoded->predecessors.path == input.predecessors.path,
        "incremental mode uses the paged scan request and explicit null accepted cache");
    SDK_CHECK(wire.find("scan") && *wire.find("scan") == addon::encode_usage_scan_request(input.scan, 3),
        "the nested scan is the unchanged usage-scan@3 grammar");
    SDK_CHECK(!addon::decode_usage_scan_request(wire, &failure, 3),
        "ordinary scan cannot accept a correction request");
    for (const char* member : {"scan", "predecessors"}) {
        invalid = wire;
        invalid.erase(member);
        SDK_CHECK(!addon::decode_usage_owned_event_correction_request(invalid, &failure),
            "both request members are required");
    }
    invalid = wire;
    invalid.find("scan")->erase("cache_selection");
    SDK_CHECK(!addon::decode_usage_owned_event_correction_request(invalid, &failure),
        "the optional operation cannot weaken scan cache selection");
    for (const auto& path : {std::string("predecessors.jsonl"), profile_root(),
             profile_path("../outside.jsonl"), profile_root() + "-other/predecessors.jsonl"})
    {
        invalid = wire;
        invalid.find("predecessors")->set("path", Json(path));
        SDK_CHECK(!addon::decode_usage_owned_event_correction_request(invalid, &failure),
            "predecessor files must be lexical descendants of the selected profile");
    }
    for (const auto& digest : {std::string(64, 'A'), std::string(63, 'a'), std::string(64, 'g')}) {
        invalid = wire;
        invalid.find("predecessors")->set("sha256_hex", Json(digest));
        SDK_CHECK(!addon::decode_usage_owned_event_correction_request(invalid, &failure),
            "the stream digest is exactly lowercase SHA256");
    }
    invalid = wire;
    invalid.find("predecessors")->set("accepted", Json());
    SDK_CHECK(!addon::decode_usage_owned_event_correction_request(invalid, &failure),
        "predecessor descriptors have a closed member set");
    return true;
}

bool predecessor_contract()
{
    addon::Contract_failure failure;
    const auto input = request();
    addon::Usage_owned_event_predecessor_header header{
        "org.example.kimi", input.scan.context.account_context, profile_root(),
        addon::Usage_cache_reference{profile_path("accepted"), "accepted-snapshot-9", std::string(64, 'c')}};
    const auto wire = addon::encode_usage_owned_event_predecessor_header(header);
    const auto decoded = addon::decode_usage_owned_event_predecessor_header(wire, &failure);
    SDK_CHECK(decoded && decoded->accepted_cache == header.accepted_cache && decoded->account_context &&
            addon::encode_account_context(*decoded->account_context) == addon::encode_account_context(*header.account_context),
        "the predecessor header preserves exact account and accepted cache including its root");
    header.account_context.reset();
    header.accepted_cache.reset();
    const auto absent = addon::encode_usage_owned_event_predecessor_header(header);
    SDK_CHECK(absent.find("account_context")->is_null() && absent.find("accepted_cache")->is_null() &&
            addon::decode_usage_owned_event_predecessor_header(absent, &failure),
        "absent selections are explicit nulls");
    for (const char* member : {"version", "provider_id", "profile_root", "account_context", "accepted_cache"}) {
        auto invalid = wire;
        invalid.erase(member);
        SDK_CHECK(!addon::decode_usage_owned_event_predecessor_header(invalid, &failure),
            "the entire predecessor selection header is required");
    }
    auto invalid = wire;
    invalid.find("accepted_cache")->set("root", Json(profile_root() + "-other"));
    SDK_CHECK(!addon::decode_usage_owned_event_predecessor_header(invalid, &failure),
        "the accepted cache must stay inside the header's profile");
    invalid = wire;
    invalid.set("provider_id", Json("not a package id"));
    SDK_CHECK(!addon::decode_usage_owned_event_predecessor_header(invalid, &failure),
        "the provider identity is an adapter package ID");

    addon::Usage_owned_event_predecessor predecessor{record(), std::string(64, 'd'), Json::object()};
    predecessor.temporal_acceptance.set("host-owned", Json("original-acceptance"));
    const auto row = addon::encode_usage_owned_event_predecessor(predecessor);
    const auto round_trip = addon::decode_usage_owned_event_predecessor(row, &failure);
    SDK_CHECK(round_trip && round_trip->predecessor_sha256_hex == predecessor.predecessor_sha256_hex &&
            round_trip->temporal_acceptance == predecessor.temporal_acceptance && round_trip->record.temporal &&
            round_trip->record.temporal->parent.snapshot_id == "accepted-snapshot-9" &&
            round_trip->record.capabilities.find("other-proof"),
        "the projection preserves original temporal evidence and opaque host acceptance");
    for (const char* member : {"record", "predecessor_sha256_hex", "temporal_acceptance"}) {
        invalid = row;
        invalid.erase(member);
        SDK_CHECK(!addon::decode_usage_owned_event_predecessor(invalid, &failure),
            "predecessor projection fields cannot be inferred");
    }
    invalid = row;
    invalid.set("temporal_acceptance", Json("accepted"));
    SDK_CHECK(!addon::decode_usage_owned_event_predecessor(invalid, &failure),
        "opaque acceptance still requires an object or explicit null");
    invalid = row;
    invalid.set("predecessor_sha256_hex", Json(std::string(64, 'D')));
    SDK_CHECK(!addon::decode_usage_owned_event_predecessor(invalid, &failure), "host row digests use lowercase SHA256");
    invalid = row;
    invalid.find("record")->erase("aggregate_evidence");
    SDK_CHECK(!addon::decode_usage_owned_event_predecessor(invalid, &failure),
        "accounting projections retain the complete usage-scan@3 record grammar");
    invalid = row;
    invalid.find("record")->find("capabilities")->find(addon::k_usage_temporal_capability_id)->set("version", Json(9));
    const auto opaque = addon::decode_usage_owned_event_predecessor(invalid, &failure);
    SDK_CHECK(opaque && !opaque->record.temporal &&
            opaque->record.capabilities.find(addon::k_usage_temporal_capability_id)->find("version")->as_integer() == 9,
        "unknown temporal versions stay opaque instead of acquiring removal authority");
    invalid = row;
    invalid.find("temporal_acceptance")->set("opaque",
        Json(std::string((std::size_t)addon::limits::k_max_exchange_payload_bytes, 'x')));
    SDK_CHECK(!addon::decode_usage_owned_event_predecessor(invalid, &failure),
        "a predecessor line cannot exceed the existing transport payload bound");
    return true;
}

bool correction_contract()
{
    addon::Contract_failure failure;
    const auto value = correction();
    const auto wire = addon::encode_usage_owned_event_correction(value);
    const auto decoded = addon::decode_usage_owned_event_correction(wire, &failure);
    SDK_CHECK(decoded && decoded->removed && decoded->removed->total_tokens == 150 &&
            decoded->removed->total_records == 2 && decoded->removed->cache_read.value == 20 &&
            decoded->removed->temporal && decoded->removed->temporal->portions.size() == 2 &&
            decoded->original_record_ids == value.original_record_ids,
        "a correction carries independent native counts and exact repository/temporal vectors");
    SDK_CHECK(!wire.find("removed")->find("temporal")->find("parent"),
        "removal vectors never invent a temporal parent");
    auto invalid = wire;
    invalid.set("coverage_policy", Json("retained_unknown_sessions"));
    SDK_CHECK(addon::decode_usage_owned_event_correction(invalid, &failure).has_value(),
        "retained session-unit coverage is an explicit policy");
    invalid.set("coverage_policy", Json("sessions"));
    SDK_CHECK(!addon::decode_usage_owned_event_correction(invalid, &failure),
        "unrecognized coverage policies grant no subtraction authority");

    for (const auto& version : {Json(), Json(0), Json(-1), Json("1"), Json(1.0), Json(true)}) {
        invalid = wire;
        invalid.set("version", version);
        SDK_CHECK(!addon::decode_usage_owned_event_correction(invalid, &failure),
            "version one requires an exact positive JSON integer");
    }
    invalid = wire;
    invalid.set("version", Json(2));
    invalid.set("future", Json::array());
    SDK_CHECK(!addon::decode_usage_owned_event_correction(invalid, &failure) &&
            failure.reason_code == addon::reason::k_unsupported_version,
        "future optional versions are unavailable without interpreting their grammar");
    auto ordinary = record();
    ordinary.capabilities.set(addon::k_usage_owned_event_correction_capability_id, invalid);
    SDK_CHECK(addon::decode_usage_record(addon::encode_usage_record(ordinary, 3), &failure, 3).has_value(),
        "an unknown correction version cannot invalidate ordinary accounting records");
    for (const char* member : {"version", "disposition", "predecessor_sha256_hex", "coverage_policy",
             "removed", "original_record_ids"})
    {
        invalid = wire;
        invalid.erase(member);
        SDK_CHECK(!addon::decode_usage_owned_event_correction(invalid, &failure),
            "correction authority requires every declared field");
    }
    for (const auto& ids : {Json::Array{}, Json::Array{Json("creator-b"), Json("creator-a")},
             Json::Array{Json("creator-a"), Json("creator-a")}, Json::Array{Json(7)},
             Json::Array{Json("")}, Json::Array{Json(std::string(257, 'a'))},
             Json::Array{Json(std::string("creator\0suffix", 14))}})
    {
        invalid = wire;
        invalid.set("original_record_ids", Json(ids));
        SDK_CHECK(!addon::decode_usage_owned_event_correction(invalid, &failure),
            "original owners are nonempty sorted unique bounded stable IDs");
    }
    for (const auto& number : {Json(-1), Json(0.5), Json(true), Json("150"), Json(std::uint64_t{9007199254740992})}) {
        for (const char* field : {"total_tokens", "total_records"}) {
            invalid = wire;
            invalid.find("removed")->set(field, number);
            SDK_CHECK(!addon::decode_usage_owned_event_correction(invalid, &failure),
                "removed totals require exact nonnegative bounded integers");
        }
        for (const char* field : {"value", "covered_records"}) {
            invalid = wire;
            invalid.find("removed")->find("input")->set(field, number);
            SDK_CHECK(!addon::decode_usage_owned_event_correction(invalid, &failure),
                "removed categories require exact nonnegative bounded integers");
        }
    }
    for (const int coverage : {0, 3}) {
        invalid = wire;
        invalid.find("removed")->find("input")->set("covered_records", Json(coverage));
        SDK_CHECK(!addon::decode_usage_owned_event_correction(invalid, &failure),
            "nonzero category removals need coverage within the removed occurrence count");
    }
    auto zero = value;
    zero.removed = addon::Usage_owned_event_removal{};
    zero.removed->total_records = 1;
    zero.removed->input.covered_records = 1;
    SDK_CHECK(addon::decode_usage_owned_event_correction(addon::encode_usage_owned_event_correction(zero), &failure).has_value(),
        "zero-token native occurrences still carry coverage removal");
    zero.removed->total_tokens = 1;
    zero.removed->repository_usage.unattributed_tokens = 1;
    zero.removed->total_records = 0;
    SDK_CHECK(!addon::decode_usage_owned_event_correction(addon::encode_usage_owned_event_correction(zero), &failure),
        "nonzero token removal cannot claim zero occurrences");

    for (const int amount : {49, 51}) {
        invalid = wire;
        invalid.find("removed")->find("repository_usage")->set("unattributed_tokens", Json(amount));
        SDK_CHECK(!addon::decode_usage_owned_event_correction(invalid, &failure),
            "repository removal must exactly partition the declared scalar");
    }
    invalid = wire;
    auto* allocations = invalid.find("removed")->find("repository_usage")->find("allocations");
    allocations->push_back(allocations->as_array().front());
    SDK_CHECK(!addon::decode_usage_owned_event_correction(invalid, &failure),
        "repeated repository paths cannot provide independent subtraction components");
    invalid = wire;
    invalid.find("removed")->find("repository_usage")->find("allocations")->as_array().front().set(
        "repository_path", Json("relative/repository"));
    SDK_CHECK(!addon::decode_usage_owned_event_correction(invalid, &failure),
        "repository removal retains the existing absolute path grammar");

    for (const int amount : {29, 31}) {
        invalid = wire;
        invalid.find("removed")->find("temporal")->set("unavailable_tokens", Json(amount));
        SDK_CHECK(!addon::decode_usage_owned_event_correction(invalid, &failure),
            "temporal removal must exactly partition the declared scalar");
    }
    invalid = wire;
    invalid.find("removed")->find("temporal")->set("parent", Json::object());
    SDK_CHECK(!addon::decode_usage_owned_event_correction(invalid, &failure),
        "a subtraction vector cannot name or replace a temporal parent");
    invalid = wire;
    auto* temporal = invalid.find("removed")->find("temporal");
    temporal->find("portions")->as_array().front().set("total_tokens", Json(50));
    temporal->find("portions")->push_back(temporal->find("portions")->as_array().front());
    SDK_CHECK(!addon::decode_usage_owned_event_correction(invalid, &failure),
        "repeated temporal positions remain invalid even when amounts reconcile");
    for (const char* instant : {"2026-10-18T08:15:00-00:00", "2026-02-30T08:15:00Z"}) {
        invalid = wire;
        invalid.find("removed")->find("temporal")->find("portions")->as_array().front().set("instant", Json(instant));
        SDK_CHECK(!addon::decode_usage_owned_event_correction(invalid, &failure),
            "removal reuses the existing known-offset instant grammar");
    }
    auto retained = value;
    retained.disposition = "retain";
    const auto retention = addon::encode_usage_owned_event_correction(retained);
    const auto retained_decoded = addon::decode_usage_owned_event_correction(retention, &failure);
    SDK_CHECK(retention.as_object().size() == 3 && retained_decoded && !retained_decoded->removed &&
            retained_decoded->coverage_policy.empty() && retained_decoded->original_record_ids.empty(),
        "exact retention carries only version, disposition and predecessor digest");
    for (const char* field : {"coverage_policy", "removed", "original_record_ids"}) {
        invalid = retention;
        invalid.set(field, *wire.find(field));
        SDK_CHECK(!addon::decode_usage_owned_event_correction(invalid, &failure),
            "retention rejects every correction-only authority field");
    }
    return true;
}

} // namespace

bool test_usage_owned_event_correction_contracts()
{
    // Oracle: the adopted usage-owned-event-correction@1 wire contract.
    return request_transport_context() && request_contract() && predecessor_contract() && correction_contract();
}

} // namespace sdk_tests
