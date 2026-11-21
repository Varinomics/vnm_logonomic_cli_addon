#include "sdk_test_support.h"

#include "logonomic_cli_addon/frame.h"
#include "logonomic_cli_addon/host_simulator.h"
#include "logonomic_cli_addon/ids.h"
#include "logonomic_cli_addon/json.h"
#include "logonomic_cli_addon/limits.h"
#include "logonomic_cli_addon/process_proposal.h"
#include "logonomic_cli_addon/protocol.h"
#include "logonomic_cli_addon/transcript.h"

#include <algorithm>
#include <string>
#include <vector>

namespace sdk_tests {

namespace {

namespace addon = logonomic::cli_addon;

const std::string k_digest(64, 'a');
const std::string k_nonce(32, 'b');

addon::Host_hello sample_host_hello()
{
    addon::Host_hello hello;
    hello.core_protocol_generations = {1};
    hello.package_id                = "org.example.unknown-fixture";
    hello.generation_digest         = k_digest;
    hello.generation_nonce          = k_nonce;
    hello.host_product              = "logonomic";
    hello.host_version              = "0.1";
    return hello;
}

addon::Adapter_hello sample_adapter_hello()
{
    addon::Adapter_hello hello;
    hello.core_protocol_generation = 1;
    hello.package_id               = "org.example.unknown-fixture";
    hello.generation_digest        = k_digest;
    hello.generation_nonce         = k_nonce;
    hello.adapter_name             = "unknown-fixture-adapter";
    hello.adapter_version          = "1.0.0";
    return hello;
}

std::string frame_of(const addon::Host_message& message)
{
    return addon::serialize_json(addon::encode_host_message(message));
}

std::string frame_of(const addon::Adapter_message& message)
{
    return addon::serialize_json(addon::encode_adapter_message(message));
}

// Decodes an adapter frame and returns the reason code of the failure, or
// empty when it decoded.
std::string adapter_decode_failure(std::string_view frame)
{
    addon::Contract_failure failure;
    return addon::decode_adapter_message(frame, addon::Json_parse_limits{}, &failure).has_value() ? std::string() : failure.reason_code;
}

std::string host_decode_failure(std::string_view frame)
{
    addon::Contract_failure failure;
    return addon::decode_host_message(frame, addon::Json_parse_limits{}, &failure).has_value() ? std::string() : failure.reason_code;
}

} // namespace

bool test_protocol_round_trips()
{
    bool ok = true;
    const char* id = __func__;

    // Host hello.
    {
        const addon::Host_hello hello = sample_host_hello();
        const std::string frame = frame_of(addon::Host_message(hello));
        ok &= check(frame == R"({"type":"hello","core_protocol_generations":[1],"package_id":"org.example.unknown-fixture",)"
            R"("generation_digest":")" + k_digest + R"(","generation_nonce":")" + k_nonce + R"(",)"
            R"("limits":{"max_frame_bytes":4194304,"max_json_depth":32,"max_in_flight_requests":64,"max_progress_frames_per_request":9223372036854775807},)"
            R"("host":{"product":"logonomic","version":"0.1"}})", id, "host hello wire form");
        const std::optional<addon::Host_message> decoded = addon::decode_host_message(frame, {}, nullptr);
        const addon::Host_hello* back = decoded.has_value() ? std::get_if<addon::Host_hello>(&*decoded) : nullptr;
        ok &= check(back != nullptr && back->package_id == hello.package_id && back->limits.max_frame_bytes == hello.limits.max_frame_bytes &&
            back->core_protocol_generations == hello.core_protocol_generations && back->host_version == "0.1", id, "host hello round trip");
    }
    // Invoke, cancel, shutdown.
    {
        addon::Invoke_request invoke;
        invoke.request_id  = "r1";
        invoke.capability  = addon::Capability_pair{"profile-storage", 1};
        invoke.operation   = "acknowledge";
        invoke.payload     = addon::Json_value::object();
        invoke.payload.set("k", addon::Json_value("v"));
        invoke.deadline_ms = 5000;
        const std::string frame = frame_of(addon::Host_message(invoke));
        ok &= check(frame == R"({"type":"invoke","request_id":"r1","capability_id":"profile-storage","version":1,"operation":"acknowledge","payload":{"k":"v"},"deadline_ms":5000})", id,
            "invoke wire form");
        const std::optional<addon::Host_message> decoded = addon::decode_host_message(frame, {}, nullptr);
        const addon::Invoke_request* back = decoded.has_value() ? std::get_if<addon::Invoke_request>(&*decoded) : nullptr;
        ok &= check(back != nullptr && back->capability == invoke.capability && back->payload == invoke.payload && back->deadline_ms == 5000, id,
            "invoke round trip");
        invoke.deadline_ms = addon::limits::k_no_invoke_deadline_ms;
        const auto unbounded = addon::decode_host_message(frame_of(addon::Host_message(invoke)), {}, nullptr);
        const auto* unbounded_back = unbounded ? std::get_if<addon::Invoke_request>(&*unbounded) : nullptr;
        ok &= check(unbounded_back && unbounded_back->deadline_ms == addon::limits::k_no_invoke_deadline_ms, id,
            "a request without an elapsed deadline uses the accepted invoke envelope");

        ok &= check(frame_of(addon::Host_message(addon::Cancel_request{"r1"})) == R"({"type":"cancel","request_id":"r1"})", id, "cancel wire form");
        ok &= check(frame_of(addon::Host_message(addon::Shutdown_request{5000})) == R"({"type":"shutdown","grace_ms":5000})", id, "shutdown wire form");
        const std::optional<addon::Host_message> cancel = addon::decode_host_message(R"({"type":"cancel","request_id":"r7"})", {}, nullptr);
        ok &= check(cancel.has_value() && std::get<addon::Cancel_request>(*cancel).request_id == "r7", id, "cancel round trip");
        const std::optional<addon::Host_message> shutdown = addon::decode_host_message(R"({"type":"shutdown","grace_ms":0})", {}, nullptr);
        ok &= check(shutdown.has_value() && std::get<addon::Shutdown_request>(*shutdown).grace_ms == 0, id, "shutdown with zero grace");
    }
    // Adapter hello with and without limits.
    {
        addon::Adapter_hello hello = sample_adapter_hello();
        std::string frame = frame_of(addon::Adapter_message(hello));
        ok &= check(frame.find("\"limits\"") == std::string::npos, id, "absent adapter limits are omitted");
        hello.limits.max_frame_bytes = 1048576;
        frame = frame_of(addon::Adapter_message(hello));
        ok &= check(frame.find(R"("limits":{"max_frame_bytes":1048576})") != std::string::npos, id, "present adapter limit is written");
        const std::optional<addon::Adapter_message> decoded = addon::decode_adapter_message(frame, {}, nullptr);
        const addon::Adapter_hello* back = decoded.has_value() ? std::get_if<addon::Adapter_hello>(&*decoded) : nullptr;
        ok &= check(back != nullptr && back->limits.max_frame_bytes == 1048576 && !back->limits.max_json_depth.has_value() &&
            back->adapter_name == "unknown-fixture-adapter", id, "adapter hello round trip");
    }
    // Capabilities, progress, results.
    {
        addon::Capabilities_message capabilities;
        capabilities.capabilities = {{"profile-storage", 1}, {"conformance-probe", 1}};
        const std::string frame = frame_of(addon::Adapter_message(capabilities));
        ok &= check(frame == R"({"type":"capabilities","capabilities":[{"capability_id":"profile-storage","version":1},{"capability_id":"conformance-probe","version":1}]})", id,
            "capabilities wire form");
        const std::optional<addon::Adapter_message> decoded = addon::decode_adapter_message(frame, {}, nullptr);
        ok &= check(decoded.has_value() && std::get<addon::Capabilities_message>(*decoded).capabilities == capabilities.capabilities, id,
            "capabilities round trip");

        addon::Progress_message progress;
        progress.request_id = "r1";
        progress.completed  = 3;
        progress.total      = 10;
        progress.note       = "scanning";
        const std::string progress_frame = frame_of(addon::Adapter_message(progress));
        ok &= check(progress_frame == R"({"type":"progress","request_id":"r1","completed":3,"total":10,"note":"scanning"})", id, "progress wire form");
        const std::optional<addon::Adapter_message> progress_back = addon::decode_adapter_message(progress_frame, {}, nullptr);
        ok &= check(progress_back.has_value() && std::get<addon::Progress_message>(*progress_back).total == 10, id, "progress round trip");
        const std::optional<addon::Adapter_message> bare = addon::decode_adapter_message(R"({"type":"progress","request_id":"r1","completed":0})", {}, nullptr);
        ok &= check(bare.has_value() && !std::get<addon::Progress_message>(*bare).total.has_value(), id, "progress without total");

        addon::Result_message success;
        success.request_id = "r1";
        success.status     = addon::Result_status::SUCCESS;
        success.payload.set("ok", addon::Json_value(true));
        ok &= check(frame_of(addon::Adapter_message(success)) == R"({"type":"result","request_id":"r1","status":"success","payload":{"ok":true}})", id,
            "success result wire form");
        addon::Result_message error;
        error.request_id = "r2";
        error.status     = addon::Result_status::FAILED;
        error.reason     = "vendor_unreachable";
        error.message    = "no route";
        ok &= check(frame_of(addon::Adapter_message(error)) == R"({"type":"result","request_id":"r2","status":"error","reason":"vendor_unreachable","message":"no route"})", id,
            "error result wire form");
        const std::optional<addon::Adapter_message> error_back = addon::decode_adapter_message(frame_of(addon::Adapter_message(error)), {}, nullptr);
        ok &= check(error_back.has_value() && std::get<addon::Result_message>(*error_back).status == addon::Result_status::FAILED &&
            std::get<addon::Result_message>(*error_back).message == "no route", id, "error result round trip");
        addon::Result_message cancelled;
        cancelled.request_id = "r3";
        cancelled.status     = addon::Result_status::CANCELLED;
        cancelled.reason     = "cancelled";
        ok &= check(frame_of(addon::Adapter_message(cancelled)) == R"({"type":"result","request_id":"r3","status":"cancelled","reason":"cancelled"})", id,
            "cancelled result omits message");
    }
    ok &= check(std::string(addon::result_status_name(addon::Result_status::UNAVAILABLE)) == "unavailable" &&
        addon::parse_result_status("cancelled") == addon::Result_status::CANCELLED && !addon::parse_result_status("ok").has_value(), id,
        "status vocabulary");
    return ok;
}

bool test_protocol_strict_envelopes()
{
    bool ok = true;
    const char* id = __func__;

    const std::string hello = frame_of(addon::Adapter_message(sample_adapter_hello()));

    ok &= check(adapter_decode_failure("not json") == addon::reason::k_frame_violation,           id, "non-JSON frame");
    ok &= check(adapter_decode_failure("[1]") == addon::reason::k_frame_violation,                id, "non-object frame");
    ok &= check(adapter_decode_failure("{}") == addon::reason::k_frame_violation,                 id, "frame without type");
    ok &= check(adapter_decode_failure(R"({"type":"telemetry"})") == addon::reason::k_unknown_message_type, id, "unknown type");
    ok &= check(adapter_decode_failure(R"({"type":"invoke","request_id":"r1"})") == addon::reason::k_unknown_message_type, id,
        "host type on the adapter side is unknown");
    ok &= check(host_decode_failure(R"({"type":"result","request_id":"r1","status":"success","payload":{}})") == addon::reason::k_unknown_message_type, id,
        "adapter type on the host side is unknown");

    // Unknown members are violations in every message.
    ok &= check(adapter_decode_failure(hello.substr(0, hello.size() - 1) + R"(,"extra":1})") == addon::reason::k_unexpected_member, id,
        "extra member in adapter hello");
    ok &= check(adapter_decode_failure(R"({"type":"capabilities","capabilities":[],"note":"x"})") == addon::reason::k_unexpected_member, id,
        "extra member in capabilities");
    ok &= check(adapter_decode_failure(R"({"type":"capabilities","capabilities":[{"capability_id":"a","version":1,"live":true}]})") == addon::reason::k_unexpected_member, id,
        "extra member in a capability entry");
    ok &= check(adapter_decode_failure(R"({"type":"progress","request_id":"r1","completed":1,"eta":5})") == addon::reason::k_unexpected_member, id,
        "extra member in progress");
    ok &= check(adapter_decode_failure(R"({"type":"result","request_id":"r1","status":"success","payload":{},"trace":"x"})") == addon::reason::k_unexpected_member, id,
        "extra member in result");
    ok &= check(host_decode_failure(R"({"type":"cancel","request_id":"r1","why":"x"})") == addon::reason::k_unexpected_member, id, "extra member in cancel");
    ok &= check(host_decode_failure(R"({"type":"shutdown","grace_ms":1,"force":true})") == addon::reason::k_unexpected_member, id, "extra member in shutdown");
    {
        addon::Json_value hello_object = addon::encode_host_message(addon::Host_message(sample_host_hello()));
        hello_object.find("limits")->set("max_widgets", addon::Json_value(1));
        ok &= check(host_decode_failure(addon::serialize_json(hello_object)) == addon::reason::k_unexpected_member, id, "extra member in host limits");
        hello_object = addon::encode_host_message(addon::Host_message(sample_host_hello()));
        hello_object.find("host")->set("build", addon::Json_value("x"));
        ok &= check(host_decode_failure(addon::serialize_json(hello_object)) == addon::reason::k_unexpected_member, id, "extra member in host.host");
    }

    // Missing or mistyped members and grammar failures are frame violations.
    ok &= check(adapter_decode_failure(R"({"type":"hello","core_protocol_generation":1})") == addon::reason::k_frame_violation, id, "hello without identity");
    ok &= check(adapter_decode_failure(R"({"type":"result","request_id":"R1","status":"success","payload":{}})") == addon::reason::k_frame_violation, id,
        "request id out of grammar");
    ok &= check(adapter_decode_failure(R"({"type":"result","request_id":"r1","status":"done","payload":{}})") == addon::reason::k_frame_violation, id,
        "status outside the vocabulary");
    ok &= check(adapter_decode_failure(R"({"type":"result","request_id":"r1","status":"success"})") == addon::reason::k_frame_violation, id,
        "success without payload");
    ok &= check(adapter_decode_failure(R"({"type":"result","request_id":"r1","status":"success","payload":[]})") == addon::reason::k_frame_violation, id,
        "success with non-object payload");
    ok &= check(adapter_decode_failure(R"({"type":"result","request_id":"r1","status":"success","payload":{},"reason":"x"})") == addon::reason::k_unexpected_member, id,
        "success with reason");
    ok &= check(adapter_decode_failure(R"({"type":"result","request_id":"r1","status":"error"})") == addon::reason::k_frame_violation, id,
        "error without reason");
    ok &= check(adapter_decode_failure(R"({"type":"result","request_id":"r1","status":"error","reason":"Bad-Reason"})") == addon::reason::k_frame_violation, id,
        "reason out of grammar");
    ok &= check(adapter_decode_failure(R"({"type":"result","request_id":"r1","status":"error","reason":"x","payload":{}})") == addon::reason::k_unexpected_member, id,
        "error with payload");
    ok &= check(adapter_decode_failure(R"({"type":"result","request_id":"r1","status":"error","reason":"x","message":")" + std::string(1025, 'm') + R"("})") == addon::reason::k_frame_violation, id,
        "message above 1024 bytes");
    ok &= check(adapter_decode_failure(R"({"type":"progress","request_id":"r1","completed":5,"total":4})") == addon::reason::k_frame_violation, id,
        "progress total below completed");
    ok &= check(adapter_decode_failure(R"({"type":"progress","request_id":"r1","completed":-1})") == addon::reason::k_frame_violation, id,
        "negative completed");
    ok &= check(adapter_decode_failure(R"({"type":"progress","request_id":"r1","completed":1,"note":")" + std::string(257, 'n') + R"("})") == addon::reason::k_frame_violation, id,
        "note above 256 bytes");
    ok &= check(adapter_decode_failure(R"({"type":"capabilities","capabilities":[{"capability_id":"a","version":1},{"capability_id":"a","version":1}]})") == addon::reason::k_frame_violation, id,
        "duplicate capability pair");
    ok &= check(adapter_decode_failure(R"({"type":"capabilities","capabilities":[{"capability_id":"a","version":1.0}]})") == addon::reason::k_frame_violation, id,
        "version must be an integer");
    ok &= check(adapter_decode_failure(R"({"type":"hello","core_protocol_generation":1,"package_id":"org.example.unknown-fixture","generation_digest":")" + k_digest +
        R"(","generation_nonce":")" + k_nonce + R"(","limits":{"max_frame_bytes":0},"adapter":{"name":"a","version":"1"}})") == addon::reason::k_frame_violation, id,
        "zero limit");
    ok &= check(host_decode_failure(R"({"type":"invoke","request_id":"r1","capability_id":"c","version":1,"operation":"op","payload":{},"deadline_ms":0})") == addon::reason::k_frame_violation, id,
        "zero deadline");
    ok &= check(host_decode_failure(R"({"type":"invoke","request_id":"r1","capability_id":"c","version":1,"operation":"Op","payload":{},"deadline_ms":1})") == addon::reason::k_frame_violation, id,
        "operation out of grammar");
    ok &= check(host_decode_failure(R"({"type":"invoke","request_id":"r1","capability_id":"c","version":1,"operation":"op","payload":[],"deadline_ms":1})") == addon::reason::k_frame_violation, id,
        "payload must be an object");
    ok &= check(host_decode_failure(R"({"type":"shutdown","grace_ms":-1})") == addon::reason::k_frame_violation, id, "negative grace");

    // Parse limits apply to frames.
    addon::Json_parse_limits tight;
    tight.max_depth = 3;
    addon::Contract_failure failure;
    ok &= check(!addon::decode_host_message(R"({"type":"invoke","request_id":"r1","capability_id":"c","version":1,"operation":"op","payload":{"a":[[1]]},"deadline_ms":1})", tight, &failure).has_value() &&
        failure.reason_code == addon::reason::k_frame_violation, id, "depth limit rejects a deep payload");
    return ok;
}

bool test_session_tracker_rules()
{
    bool ok = true;
    const char* id = __func__;
    addon::Contract_failure violation;

    // Ordering: capabilities before hello, progress before ready, second hello.
    {
        addon::Session_tracker tracker(sample_host_hello());
        ok &= check(!tracker.accept(addon::Adapter_message(addon::Capabilities_message{}), &violation) &&
            violation.reason_code == addon::reason::k_out_of_order_message, id, "capabilities before hello");
        addon::Progress_message progress;
        progress.request_id = "r1";
        ok &= check(!tracker.accept(addon::Adapter_message(progress), &violation) &&
            violation.reason_code == addon::reason::k_out_of_order_message, id, "progress before handshake");
        ok &= check(tracker.accept(addon::Adapter_message(sample_adapter_hello()), &violation), id, "hello accepted");
        ok &= check(!tracker.ready(), id, "not ready after hello alone");
        ok &= check(!tracker.accept(addon::Adapter_message(sample_adapter_hello()), &violation) &&
            violation.reason_code == addon::reason::k_out_of_order_message, id, "second hello");
        addon::Capabilities_message capabilities;
        capabilities.capabilities = {{"profile-storage", 1}};
        ok &= check(tracker.accept(addon::Adapter_message(capabilities), &violation) && tracker.ready(), id, "ready after capabilities");
        ok &= check(!tracker.accept(addon::Adapter_message(capabilities), &violation) &&
            violation.reason_code == addon::reason::k_out_of_order_message, id, "second capabilities");
        ok &= check(tracker.handshake_declares({"profile-storage", 1}) && !tracker.handshake_declares({"profile-storage", 2}), id,
            "handshake pairs");
    }
    // Identity.
    for (int variant = 0; variant < 4; ++variant) {
        addon::Session_tracker tracker(sample_host_hello());
        addon::Adapter_hello hello = sample_adapter_hello();
        switch (variant) {
            case 0:  hello.core_protocol_generation = 2;        break;
            case 1:  hello.package_id = "org.example.other";    break;
            case 2:  hello.generation_digest = std::string(64, 'c'); break;
            default: hello.generation_nonce = std::string(32, 'd');  break;
        }
        ok &= check(!tracker.accept(addon::Adapter_message(hello), &violation) && violation.reason_code == addon::reason::k_identity_mismatch, id,
            "identity mismatch variant " + std::to_string(variant));
    }
    // Limits above the host's.
    {
        addon::Session_tracker tracker(sample_host_hello());
        addon::Adapter_hello hello = sample_adapter_hello();
        hello.limits.max_frame_bytes = addon::limits::k_max_frame_bytes + 1;
        ok &= check(!tracker.accept(addon::Adapter_message(hello), &violation) && violation.reason_code == addon::reason::k_frame_violation, id,
            "adapter limit above the host's");
    }
    // Progress lifetime does not consume a request's correlation state.
    {
        addon::Session_tracker tracker(sample_host_hello());
        tracker.accept(addon::Adapter_message(sample_adapter_hello()), &violation);
        tracker.accept(addon::Adapter_message(addon::Capabilities_message{}), &violation);
        tracker.note_invoke_sent("r1");
        addon::Progress_message progress;
        progress.request_id = "r1";
        for (std::uint64_t step = 0; step < 8192; ++step) {
            progress.completed = step;
            if (!tracker.accept(addon::Adapter_message(progress), &violation)) {
                ok &= check(false, id, "continuous progress remains accepted: " + violation.detail);
                break;
            }
        }
        ok &= check(tracker.live_request_count() == 1, id, "progress retains only its live request");
        addon::Result_message result;
        result.request_id = "r1";
        result.status = addon::Result_status::SUCCESS;
        ok &= check(tracker.accept(addon::Adapter_message(result), &violation), id,
            "result settles a long progress stream");
        ok &= check(!tracker.accept(addon::Adapter_message(progress), &violation) &&
            violation.reason_code == addon::reason::k_impossible_correlation, id,
            "progress cannot follow the terminal result");
    }
    // Effective limits and correlation.
    {
        addon::Session_tracker tracker(sample_host_hello());
        addon::Adapter_hello hello = sample_adapter_hello();
        hello.limits.max_frame_bytes                 = 1024;
        hello.limits.max_progress_frames_per_request = 2;
        ok &= check(tracker.accept(addon::Adapter_message(hello), &violation), id, "lowered limits accepted");
        ok &= check(tracker.accept(addon::Adapter_message(addon::Capabilities_message{}), &violation), id, "empty capabilities accepted");
        ok &= check(tracker.effective_limits().max_frame_bytes == 1024 && tracker.effective_limits().max_json_depth == addon::limits::k_max_json_depth, id,
            "effective limits are the smaller of host and adapter");

        addon::Result_message result;
        result.request_id = "r1";
        result.status     = addon::Result_status::SUCCESS;
        ok &= check(!tracker.accept(addon::Adapter_message(result), &violation) && violation.reason_code == addon::reason::k_impossible_correlation, id,
            "result for an unknown request");
        tracker.note_invoke_sent("r1");
        ok &= check(tracker.is_live("r1") && tracker.live_request_count() == 1, id, "invoke registered");
        addon::Progress_message progress;
        progress.request_id = "r1";
        ok &= check(tracker.accept(addon::Adapter_message(progress), &violation) && tracker.accept(addon::Adapter_message(progress), &violation), id,
            "progress within the bound");
        ok &= check(!tracker.accept(addon::Adapter_message(progress), &violation) && violation.reason_code == addon::reason::k_progress_limit_exceeded, id,
            "third progress frame exceeds the lowered bound");
    }
    {
        addon::Session_tracker tracker(sample_host_hello());
        tracker.accept(addon::Adapter_message(sample_adapter_hello()), &violation);
        tracker.accept(addon::Adapter_message(addon::Capabilities_message{}), &violation);
        tracker.note_invoke_sent("r1");
        addon::Result_message result;
        result.request_id = "r1";
        result.status     = addon::Result_status::SUCCESS;
        ok &= check(tracker.accept(addon::Adapter_message(result), &violation) && !tracker.is_live("r1") && tracker.is_finished("r1"), id,
            "terminal result finishes the request");
        ok &= check(!tracker.accept(addon::Adapter_message(result), &violation) && violation.reason_code == addon::reason::k_duplicate_terminal_result, id,
            "second terminal result");
        addon::Progress_message late;
        late.request_id = "r1";
        ok &= check(!tracker.accept(addon::Adapter_message(late), &violation) && violation.reason_code == addon::reason::k_impossible_correlation, id,
            "progress after the terminal result");
        addon::Result_message unknown;
        unknown.request_id = "r9";
        unknown.status     = addon::Result_status::CANCELLED;
        unknown.reason     = "cancelled";
        ok &= check(!tracker.accept(addon::Adapter_message(unknown), &violation) && violation.reason_code == addon::reason::k_impossible_correlation, id,
            "result for a never-sent request");

        bool long_session_accepted = true;
        for (int sequence = 2; sequence <= 10000; ++sequence) {
            result.request_id = "r" + std::to_string(sequence);
            tracker.note_invoke_sent(result.request_id);
            if (!tracker.accept(addon::Adapter_message(result), &violation)) {
                long_session_accepted = false;
                break;
            }
        }
        ok &= check(long_session_accepted && tracker.live_request_count() == 0, id,
            "a long sequential session completes without retained live requests");
        result.request_id = "r1";
        ok &= check(!tracker.accept(addon::Adapter_message(result), &violation) &&
            violation.reason_code == addon::reason::k_duplicate_terminal_result, id,
            "a very late duplicate remains distinguishable by request high-water");
    }
    return ok;
}

bool test_process_proposal_policy()
{
    bool ok = true;
    const char* id = __func__;
    addon::Contract_failure failure;
    const addon::Process_proposal_policy probe;   // the conformance probe: no launcher-program reference

    addon::Process_proposal proposal;
    proposal.executable        = "helpers/probe-helper";
    proposal.argv              = {"--json"};
    proposal.environment_names = {"HOME"};
    proposal.environment       = {{"PROBE_MODE", "fast"}};
    proposal.assets            = {"assets/mark.svg"};
    ok &= check(addon::validate_process_proposal(proposal, probe, &failure), id, "attached helper proposal is accepted");

    const addon::Json_value encoded = addon::encode_process_proposal(proposal);
    const std::optional<addon::Process_proposal> decoded = addon::decode_process_proposal(encoded, &failure);
    ok &= check(decoded.has_value() && decoded->executable == proposal.executable && decoded->argv == proposal.argv &&
        decoded->environment == proposal.environment && decoded->assets == proposal.assets && decoded->start == addon::Start_intent::ATTACHED, id,
        "proposal round trip");
    ok &= check(addon::serialize_json(encoded).find(R"("start":"attached")") != std::string::npos, id, "start intent on the wire");

    for (const addon::Start_intent intent : {addon::Start_intent::DETACHED, addon::Start_intent::NEW_SESSION, addon::Start_intent::BREAKAWAY}) {
        addon::Process_proposal detached = proposal;
        detached.start = intent;
        ok &= check(!addon::validate_process_proposal(detached, probe, &failure) && failure.reason_code == addon::reason::k_detached_helper_proposal, id,
            std::string("start intent ") + addon::start_intent_name(intent) + " is rejected");
        const std::optional<addon::Process_proposal> wire = addon::decode_process_proposal(addon::encode_process_proposal(detached), &failure);
        ok &= check(wire.has_value() && wire->start == intent, id, "nonconforming intent decodes so it can be rejected");
    }

    addon::Process_proposal shell = proposal;
    shell.executable = "helpers/run.sh";
    ok &= check(!addon::validate_process_proposal(shell, probe, &failure) && failure.reason_code == addon::reason::k_shell_adapter_entry_point, id,
        "shell script entry point rejected");
    shell.executable = "helpers/Run.CMD";
    ok &= check(!addon::validate_process_proposal(shell, probe, &failure) && failure.reason_code == addon::reason::k_shell_adapter_entry_point, id,
        "cmd entry point rejected case-insensitively");

    addon::Process_proposal escaped = proposal;
    escaped.executable = "helpers/../adapter/x";
    ok &= check(!addon::validate_process_proposal(escaped, probe, &failure) && failure.reason_code == addon::reason::k_path_escape, id,
        "escaped executable path rejected");
    escaped.executable = "assets/tool";
    ok &= check(!addon::validate_process_proposal(escaped, probe, &failure) && failure.reason_code == addon::reason::k_path_escape, id,
        "executable outside helpers/ or adapter/ rejected");
    escaped = proposal;
    escaped.assets = {"notices/x.txt"};
    ok &= check(!addon::validate_process_proposal(escaped, probe, &failure) && failure.reason_code == addon::reason::k_path_escape, id,
        "asset outside assets/ or helpers/ rejected");
    escaped.assets = {"/etc/passwd"};
    ok &= check(!addon::validate_process_proposal(escaped, probe, &failure) && failure.reason_code == addon::reason::k_path_escape, id,
        "absolute asset path rejected");

    addon::Process_proposal reserved = proposal;
    reserved.environment_names = {"LOGONOMIC_ADDON_PACKAGE_ROOT"};
    ok &= check(!addon::validate_process_proposal(reserved, probe, &failure) && failure.reason_code == addon::reason::k_reserved_environment_variable, id,
        "reserved environment name rejected");
    reserved = proposal;
    reserved.environment = {{"LOGONOMIC_X", "1"}};
    ok &= check(!addon::validate_process_proposal(reserved, probe, &failure) && failure.reason_code == addon::reason::k_reserved_environment_variable, id,
        "reserved environment update rejected");
    reserved.environment = {{"1BAD", "1"}};
    ok &= check(!addon::validate_process_proposal(reserved, probe, &failure) && failure.reason_code == addon::reason::k_reserved_environment_variable, id,
        "malformed environment name rejected");

    addon::Process_proposal oversized = proposal;
    oversized.argv.assign(addon::limits::k_max_argv_entries + 1, "a");
    ok &= check(!addon::validate_process_proposal(oversized, probe, &failure) && failure.reason_code == addon::reason::k_payload_too_large, id,
        "argv entry count above the bound");
    oversized = proposal;
    oversized.argv = {std::string(addon::limits::k_max_argv_entry_bytes + 1, 'a')};
    ok &= check(!addon::validate_process_proposal(oversized, probe, &failure) && failure.reason_code == addon::reason::k_payload_too_large, id,
        "argv entry above the byte bound");
    const auto argv_with_total = [](std::size_t bytes) {
        std::vector<std::string> argv;
        while (bytes != 0) {
            const std::size_t entry_bytes = std::min(bytes, addon::limits::k_max_argv_entry_bytes);
            argv.emplace_back(entry_bytes, 'a');
            bytes -= entry_bytes;
        }
        return argv;
    };
    for (const std::size_t bytes : {
             addon::limits::k_max_argv_total_bytes - 1,
             addon::limits::k_max_argv_total_bytes,
         })
    {
        oversized = proposal;
        oversized.argv = argv_with_total(bytes);
        ok &= check(addon::validate_process_proposal(oversized, probe, &failure), id,
            bytes == addon::limits::k_max_argv_total_bytes
                ? "proposal argv at k_max_argv_total_bytes"
                : "proposal argv one byte below k_max_argv_total_bytes");
    }
    oversized = proposal;
    oversized.argv = argv_with_total(addon::limits::k_max_argv_total_bytes + 1);
    ok &= check(!addon::validate_process_proposal(oversized, probe, &failure) &&
            failure.reason_code == addon::reason::k_payload_too_large,
        id,
        "proposal argv one byte above k_max_argv_total_bytes");
    for (const std::size_t bytes : {
             addon::limits::k_max_environment_value_bytes - 1,
             addon::limits::k_max_environment_value_bytes,
         })
    {
        oversized = proposal;
        oversized.environment = {{"VALUE", std::string(bytes, 'x')}};
        ok &= check(addon::validate_process_proposal(oversized, probe, &failure), id,
            bytes == addon::limits::k_max_environment_value_bytes
                ? "proposal environment value at k_max_environment_value_bytes"
                : "proposal environment value one byte below k_max_environment_value_bytes");
    }
    oversized = proposal;
    oversized.environment = {{"VALUE", std::string(addon::limits::k_max_environment_value_bytes + 1, 'x')}};
    ok &= check(!addon::validate_process_proposal(oversized, probe, &failure) &&
            failure.reason_code == addon::reason::k_payload_too_large,
        id,
        "proposal environment value one byte above k_max_environment_value_bytes");
    oversized = proposal;
    oversized.stdin_payload = "data";
    ok &= check(!addon::validate_process_proposal(oversized, probe, &failure) && failure.reason_code == addon::reason::k_frame_violation, id,
        "stdin payload without payload behaviour");
    oversized.stdin_behavior = addon::Stdin_behavior::PAYLOAD;
    ok &= check(addon::validate_process_proposal(oversized, probe, &failure), id, "stdin payload with payload behaviour");

    // Structural decode.
    addon::Json_value object = addon::encode_process_proposal(proposal);
    object.set("shell", addon::Json_value("bash"));
    ok &= check(!addon::decode_process_proposal(object, &failure).has_value() && failure.reason_code == addon::reason::k_unexpected_member, id,
        "unknown proposal member");
    object = addon::encode_process_proposal(proposal);
    object.set("start", addon::Json_value("background"));
    ok &= check(!addon::decode_process_proposal(object, &failure).has_value() && failure.reason_code == addon::reason::k_frame_violation, id,
        "start outside the vocabulary");
    object = addon::Json_value::object();
    ok &= check(!addon::decode_process_proposal(object, &failure).has_value() && failure.reason_code == addon::reason::k_frame_violation, id,
        "executable required");
    return ok;
}

bool test_transcript_normalization()
{
    bool ok = true;
    const char* id = __func__;

    addon::Transcript_recorder recorder;
    addon::Transcript_placeholders placeholders;
    placeholders.generation_digest = k_digest;
    placeholders.generation_nonce  = k_nonce;
    placeholders.roots             = {"/tmp/profile/org.example.unknown-fixture/default"};
    recorder.set_placeholders(placeholders);

    recorder.record_host_frame(addon::encode_host_message(addon::Host_message(sample_host_hello())));
    addon::Adapter_hello hello = sample_adapter_hello();
    hello.adapter_version = "9.9.9";
    recorder.record_adapter_frame(addon::encode_adapter_message(addon::Adapter_message(hello)));

    addon::Invoke_request invoke;
    invoke.request_id = "r7";
    invoke.capability = {"profile-storage", 1};
    invoke.operation  = "acknowledge";
    invoke.payload.set("profile_storage", addon::profile_storage_member(addon::Profile_storage_context{1, "/tmp/profile/org.example.unknown-fixture/default"}));
    recorder.record_host_frame(addon::encode_host_message(addon::Host_message(invoke)));
    addon::Result_message result;
    result.request_id = "r7";
    result.status     = addon::Result_status::FAILED;
    result.reason     = "probe_failure";
    result.message    = "/tmp/profile/org.example.unknown-fixture/default/other";
    recorder.record_adapter_frame(addon::encode_adapter_message(addon::Adapter_message(result)));
    recorder.record_event("deadline_expired", "r7");
    recorder.record_event("retired", "adapter_exited");
    recorder.record_event("adapter_exited", "exit_code=0");

    const std::vector<std::string>& lines = recorder.lines();
    SDK_CHECK(lines.size() == 7, "seven transcript lines");
    ok &= check(lines[0] == R"(> {"core_protocol_generations":[1],"generation_digest":"<digest>","generation_nonce":"<nonce>","host":{"product":"<product>","version":"<version>"},)"
        R"("limits":{"max_frame_bytes":4194304,"max_in_flight_requests":64,"max_json_depth":32,"max_progress_frames_per_request":9223372036854775807},"package_id":"org.example.unknown-fixture","type":"hello"})", id,
        "host hello normalized: placeholders, canonical order, host product and version");
    ok &= check(lines[1].find(R"("adapter":{"name":"unknown-fixture-adapter","version":"<version>"})") != std::string::npos, id, "adapter version normalized");
    ok &= check(lines[2] == R"(> {"capability_id":"profile-storage","deadline_ms":5000,"operation":"acknowledge","payload":{"profile_storage":{"root":"<root>","version":1}},"request_id":"<r#1>","type":"invoke","version":1})", id,
        "invoke normalized: request ordinal and root placeholder");
    ok &= check(lines[3] == R"(< {"message":"<root>/other","reason":"probe_failure","request_id":"<r#1>","status":"error","type":"result"})", id,
        "result normalized: root prefix inside a message");
    ok &= check(lines[4] == "! deadline_expired <r#1>", id, "event request id normalized");
    ok &= check(lines[5] == "! retired adapter_exited" && lines[6] == "! adapter_exited exit_code=0", id, "plain events");

    // A second request gets the next ordinal regardless of its wire id.
    addon::Cancel_request cancel{"r3"};
    recorder.record_host_frame(addon::encode_host_message(addon::Host_message(cancel)));
    ok &= check(recorder.lines().back() == R"(> {"request_id":"<r#2>","type":"cancel"})", id, "second request id becomes <r#2>");

    // Comparison.
    addon::Transcript_difference difference;
    std::vector<std::string> expected = recorder.lines();
    ok &= check(addon::compare_transcripts(expected, recorder.lines(), &difference), id, "identical transcripts compare equal");
    std::vector<std::string> altered = expected;
    altered[4] = "! deadline_expired <r#2>";
    ok &= check(!addon::compare_transcripts(expected, altered, &difference) && difference.line == 4, id, "first differing line reported");
    altered = expected;
    altered.pop_back();
    ok &= check(!addon::compare_transcripts(expected, altered, &difference) && difference.line == expected.size() - 1 && difference.actual == "<end>", id,
        "shorter transcript reports the missing line");
    ok &= check(addon::split_transcript_lines(recorder.text()) == recorder.lines(), id, "text round trips through split_transcript_lines");

    const std::string native_root = R"(C:\fixture\profile)";
    const std::string qt_root     = "C:/fixture/profile";
    addon::Json_value paths = addon::Json_value::object();
    paths.set("native", addon::Json_value(native_root + R"(\cache)"));
    paths.set("qt",     addon::Json_value(qt_root + "/cache"));
    paths.set("mixed",  addon::Json_value(R"(C:/fixture\profile/mixed)"));
    paths.set("short",  addon::Json_value("C:/fixture"));
    paths.set("other",  addon::Json_value("C:/fixture/elsewhere/cache"));
    for (const std::string& root : {native_root, qt_root}) {
        placeholders.roots = {root};
        recorder.set_placeholders(placeholders);
        recorder.record_host_frame(paths);
        const auto normalized = addon::parse_json(
            std::string_view(recorder.lines().back()).substr(2), {}, nullptr);
        SDK_CHECK(normalized.has_value(), "normalized root paths remain valid JSON");
#if defined(_WIN32)
        ok &= check(normalized->find("native")->as_string() == R"(<root>\cache)", id,
            "Windows native root aliases are normalized without changing the suffix");
        ok &= check(normalized->find("qt")->as_string() == "<root>/cache", id,
            "Windows Qt root aliases are normalized without changing the suffix");
        ok &= check(normalized->find("mixed")->as_string() == "<root>/mixed", id,
            "Windows root prefixes accept mixed separators");
#else
        ok &= check(normalized->find("native")->as_string() ==
            (root == native_root ? R"(<root>\cache)" : native_root + R"(\cache)"), id,
            "POSIX preserves literal backslashes when matching root prefixes");
        ok &= check(normalized->find("qt")->as_string() ==
            (root == qt_root ? "<root>/cache" : qt_root + "/cache"), id,
            "POSIX root prefixes retain exact separator matching");
        ok &= check(normalized->find("mixed")->as_string() == R"(C:/fixture\profile/mixed)", id,
            "POSIX does not treat backslashes as path separators");
#endif
        ok &= check(normalized->find("short")->as_string() == "C:/fixture", id,
            "a shorter string is not a complete root prefix");
        ok &= check(normalized->find("other")->as_string() == "C:/fixture/elsewhere/cache", id,
            "unrelated paths are preserved");
    }
    return ok;
}

bool test_adapter_environment_contract()
{
    bool ok = true;
    const char* id = __func__;

    addon::Adapter_environment_inputs inputs;
    inputs.platform_id               = addon::k_platform_linux_x86_64;
    inputs.package_id                = "org.example.unknown-fixture";
    inputs.generation_digest         = k_digest;
    inputs.generation_nonce          = k_nonce;
    inputs.package_root              = "/custody/org.example.unknown-fixture/abc";
    inputs.scratch_directory         = "/scratch/abc";
    inputs.core_protocol_generations = {1, 2};
    inputs.host_product              = "logonomic";
    inputs.manifest_environment      = {{"FIXTURE_MODE", "conformant"}, {"LANG", "C"}, {"LOGONOMIC_ADDON_PACKAGE_ID", "spoofed"}};

    const std::vector<std::pair<std::string, std::string>> ambient = {
        {"HOME", "/home/operator"},
        {"USER", "operator"},
        {"LANG", "en_US.UTF-8"},
        {"PATH", "/home/operator/.local/bin:/usr/bin"},
        {"SSH_AUTH_SOCK", "/run/user/1000/keyring/ssh"},
        {"GITHUB_TOKEN", "secret"},
        {"LOGONOMIC_SESSION_CAPABILITY", "ambient-capability"},
        {"LOGONOMIC_VNMCTL_PATH", "/opt/logonomic/vnmctl"},
        {"HTTPS_PROXY", "http://proxy:3128"},
    };

    const auto environment = addon::build_adapter_environment(inputs, ambient);
    const auto value_of = [&environment](std::string_view name) -> std::optional<std::string> {
        for (const auto& [key, value] : environment) {
            if (key == name) {
                return value;
            }
        }
        return std::nullopt;
    };

    ok &= check(value_of("HOME") == "/home/operator" && value_of("USER") == "operator", id, "baseline names are inherited");
    ok &= check(value_of("PATH") == "/custody/org.example.unknown-fixture/abc/helpers:/usr/local/bin:/usr/bin:/bin", id,
        "PATH is host-constructed, not inherited");
    ok &= check(!value_of("SSH_AUTH_SOCK").has_value() && !value_of("GITHUB_TOKEN").has_value() && !value_of("HTTPS_PROXY").has_value(), id,
        "credentials, agents and proxies are not inherited");
    ok &= check(!value_of("LOGONOMIC_SESSION_CAPABILITY").has_value() && !value_of("LOGONOMIC_VNMCTL_PATH").has_value(), id,
        "ambient LOGONOMIC_* variables are absent");
    ok &= check(value_of("FIXTURE_MODE") == "conformant", id, "manifest variable is set");
    ok &= check(value_of("LANG") == "C", id, "manifest variable replaces a baseline value");
    ok &= check(value_of(addon::k_env_package_id) == "org.example.unknown-fixture", id, "manifest cannot override a reserved variable");
    ok &= check(value_of(addon::k_env_generation_digest) == k_digest && value_of(addon::k_env_generation_nonce) == k_nonce, id,
        "digest and nonce are set");
    ok &= check(value_of(addon::k_env_package_root) == inputs.package_root && value_of(addon::k_env_scratch_dir) == inputs.scratch_directory, id,
        "roots are set");
    ok &= check(value_of(addon::k_env_core_protocol_generations) == "1,2" && value_of(addon::k_env_host_product) == "logonomic", id,
        "generations and host product are set");
    std::size_t logonomic_count = 0;
    for (const auto& [key, value] : environment) {
        if (addon::is_reserved_environment_name(key)) {
            ++logonomic_count;
        }
    }
    ok &= check(logonomic_count == 7, id, "exactly the seven reserved variables carry the prefix");
    ok &= check(std::is_sorted(environment.begin(), environment.end()), id, "environment is sorted");

    // Windows baseline: case-insensitive names and the Windows PATH shape.
    addon::Adapter_environment_inputs windows_inputs = inputs;
    windows_inputs.platform_id  = addon::k_platform_windows_x86_64;
    windows_inputs.package_root = "C:\\custody\\pkg";
    const auto windows_environment = addon::build_adapter_environment(windows_inputs, {
        {"SYSTEMROOT", "C:\\Windows"}, {"Path", "C:\\tools"}, {"USERPROFILE", "C:\\Users\\op"}, {"PSModulePath", "x"},
    });
    std::optional<std::string> windows_path;
    std::optional<std::string> system_root;
    bool has_psmodulepath = false;
    for (const auto& [key, value] : windows_environment) {
        if (key == "PATH")        { windows_path = value; }
        if (key == "SystemRoot")  { system_root = value; }
        if (key == "PSModulePath") { has_psmodulepath = true; }
    }
    ok &= check(system_root == "C:\\Windows", id, "Windows baseline name matched case-insensitively and written canonically");
    ok &= check(windows_path == "C:\\custody\\pkg\\helpers;C:\\Windows\\System32;C:\\Windows;C:\\Windows\\System32\\Wbem", id, "Windows PATH shape");
    ok &= check(!has_psmodulepath, id, "non-baseline Windows variable is not inherited");

    const std::size_t aggregate_value_bytes =
        (addon::limits::k_max_start_environment_bytes - 3) / 2;
    const auto aggregate_environment = [](std::size_t final_value_bytes) {
        return std::vector<std::pair<std::string, std::string>>{
            {"A", std::string(aggregate_value_bytes, 'x')},
            {"B", std::string(aggregate_value_bytes, 'x')},
            {"C", std::string(final_value_bytes, 'x')},
        };
    };
    constexpr std::size_t k_aggregate_environment_name_bytes = 3;
    const std::size_t aggregate_prefix =
        2 * aggregate_value_bytes + k_aggregate_environment_name_bytes;
    ok &= check(addon::adapter_start_environment_within_budget(
            aggregate_environment(addon::limits::k_max_start_environment_bytes - aggregate_prefix - 1)),
        id,
        "start environment one byte below its aggregate bound");
    ok &= check(addon::adapter_start_environment_within_budget(
            aggregate_environment(addon::limits::k_max_start_environment_bytes - aggregate_prefix)),
        id,
        "start environment at its aggregate bound");
    ok &= check(!addon::adapter_start_environment_within_budget(
            aggregate_environment(addon::limits::k_max_start_environment_bytes - aggregate_prefix + 1)),
        id,
        "start environment one byte above its aggregate bound");

    const auto entry_environment = [](std::size_t count) {
        std::vector<std::pair<std::string, std::string>> environment;
        environment.reserve(count);
        for (std::size_t i = 0; i < count; ++i) {
            environment.emplace_back("V" + std::to_string(i), "");
        }
        return environment;
    };
    ok &= check(addon::adapter_start_environment_within_budget(
            entry_environment(addon::limits::k_max_start_environment_entries - 1)),
        id,
        "start environment one entry below its entry bound");
    ok &= check(addon::adapter_start_environment_within_budget(
            entry_environment(addon::limits::k_max_start_environment_entries)),
        id,
        "start environment at its entry bound");
    ok &= check(!addon::adapter_start_environment_within_budget(
            entry_environment(addon::limits::k_max_start_environment_entries + 1)),
        id,
        "start environment one entry above its entry bound");

    std::size_t reserved_bytes = 0;
    for (const auto& [name, value] : environment) {
        if (addon::is_reserved_environment_name(name)) {
            reserved_bytes += name.size() + 1 + value.size();
        }
    }
    const auto with_reserved_suffix = [&environment](std::size_t suffix_bytes) {
        std::vector<std::pair<std::string, std::string>> bounded = environment;
        for (auto& [name, value] : bounded) {
            if (name == addon::k_env_host_product) {
                value.append(suffix_bytes, 'x');
                break;
            }
        }
        return bounded;
    };
    ok &= check(addon::adapter_reserved_environment_within_budget(
            with_reserved_suffix(addon::limits::k_max_reserved_environment_bytes - reserved_bytes - 1)),
        id,
        "seven derived reserved variables one byte below their aggregate bound");
    ok &= check(addon::adapter_reserved_environment_within_budget(
            with_reserved_suffix(addon::limits::k_max_reserved_environment_bytes - reserved_bytes)),
        id,
        "seven derived reserved variables at their aggregate bound");
    ok &= check(!addon::adapter_reserved_environment_within_budget(
            with_reserved_suffix(addon::limits::k_max_reserved_environment_bytes - reserved_bytes + 1)),
        id,
        "seven derived reserved variables one byte above their aggregate bound");
    return ok;
}

} // namespace sdk_tests
