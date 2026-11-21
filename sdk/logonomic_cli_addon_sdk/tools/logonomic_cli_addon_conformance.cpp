// logonomic_cli_addon_conformance <package-dir> [--platform <id>] [--no-isolation]
//
// The black-box conformance suite over one package directory
// (docs/conformance.md, "Checks"). Every check prints one line
// `PASS|FAIL|SKIP <check-id> <detail>`; the exit code is 1 when any check
// failed, 0 otherwise, 2 on usage errors. SKIP identifies declarations this
// suite does not assess and probes whose capabilities are not declared live.

#include "tool_arguments.h"

#include "logonomic_cli_addon/capability_ids.h"
#include "logonomic_cli_addon/launcher_approval.h"
#include "logonomic_cli_addon/frame.h"
#include "logonomic_cli_addon/host_simulator.h"
#include "logonomic_cli_addon/ids.h"
#include "logonomic_cli_addon/json.h"
#include "logonomic_cli_addon/limits.h"
#include "logonomic_cli_addon/managed_configuration.h"
#include "logonomic_cli_addon/package_source.h"
#include "logonomic_cli_addon/package_validator.h"
#include "logonomic_cli_addon/process_proposal.h"
#include "logonomic_cli_addon/protocol.h"
#include "logonomic_cli_addon/session_contracts.h"

#include <algorithm>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace {

namespace addon = logonomic::cli_addon;

constexpr int k_probe_deadline_ms = 3000;

const addon::Capability_pair k_probe{"conformance-probe", 1};
const addon::Capability_pair k_profile_storage{addon::k_profile_storage_capability_id, addon::k_profile_storage_version};

class Report
{
public:
    void pass(const std::string& check, const std::string& detail = {}) { line("PASS", check, detail); }
    void skip(const std::string& check, const std::string& detail)      { line("SKIP", check, detail); }
    void fail(const std::string& check, const std::string& detail)
    {
        line("FAIL", check, detail);
        m_failed = true;
    }

    void verdict(const std::string& check, bool ok, const std::string& detail)
    {
        if (ok) {
            pass(check, detail);
        }
        else {
            fail(check, detail);
        }
    }

    bool failed() const { return m_failed; }

private:
    void line(const char* status, const std::string& check, const std::string& detail)
    {
        std::fprintf(stdout, "%s %s%s%s\n", status, check.c_str(), detail.empty() ? "" : " ", detail.c_str());
        std::fflush(stdout);
    }

    bool m_failed = false;
};

std::string describe(const std::optional<addon::Host_outcome>& outcome)
{
    if (!outcome.has_value()) {
        return "no outcome";
    }
    std::string text = addon::host_status_name(outcome->status);
    if (!outcome->reason_code.empty()) {
        text += "/" + outcome->reason_code;
    }
    if (!outcome->message.empty()) {
        text += " (" + outcome->message + ")";
    }
    return text;
}

std::string pair_name(const addon::Capability_pair& pair)
{
    return pair.capability_id + "@" + std::to_string(pair.version);
}

// A successful result carrying exactly the payload that was sent.
bool echoed(const std::optional<addon::Host_outcome>& outcome, const addon::Json_value& payload)
{
    return outcome.has_value() && outcome->status == addon::Host_status::SUCCESS && outcome->payload == payload;
}

// An invocation the host refused before sending, leaving the adapter untouched.
bool refused_locally(const addon::Host_simulator& session, const std::optional<addon::Host_outcome>& outcome)
{
    return outcome.has_value() &&
           outcome->status == addon::Host_status::FAILED &&
           outcome->reason_code == addon::reason::k_payload_too_large &&
           session.state() == addon::Simulator_state::READY;
}

class Suite
{
public:
    Suite(addon::Simulator_options options, Report& report)
    :
        m_options(std::move(options)),
        m_report(report)
    {}

    void run()
    {
        // 1. The package itself.
        addon::Directory_package_source source(m_options.package_directory);
        addon::Package_validator validator(m_options.validation);
        addon::Contract_failure failure;
        std::optional<addon::Validated_package> package = validator.validate(source, &failure);
        if (!package.has_value()) {
            m_report.fail("package.validates", failure.reason_code + ": " + failure.detail);
            return;   // nothing below can run without a valid package
        }
        m_report.pass("package.validates", package->bootstrap.package_id + " " + package->generation_digest_hex);
        m_initial_digest = package->generation_digest_hex;
        check_capability_declarations(*package);
        if (m_report.failed()) {
            return;
        }

        // 2. Handshake, in the session that also hosts the simple probes.
        std::unique_ptr<addon::Host_simulator> session = start_session("handshake.completes");
        if (!session) {
            return;
        }
        check_handshake_agreement(*session, *package);
        const std::vector<addon::Capability_pair> live = session->live_capabilities();
        m_probe_live   = std::find(live.begin(), live.end(), k_probe) != live.end();
        m_profile_live = std::find(live.begin(), live.end(), k_profile_storage) != live.end();

        check_profile_storage(*session);
        check_probe_basics(*session);
        check_limits(*session);
        check_terminal_uniqueness(*session);
        m_report.verdict("liveness.ready_after_probes",
            session->state() == addon::Simulator_state::READY && !session->adapter_exited(),
            session->state() == addon::Simulator_state::READY ? "adapter still ready" : "retired: " + session->retirement().reason_code);
        int exit_code = -1;
        session->shutdown(addon::limits::k_shutdown_grace_ms, &exit_code);
        session.reset();

        // 3. Sessions that destabilize on purpose.
        check_cancellation();
        check_deadline();
        check_descendants();
        check_graceful_shutdown();

        // 4. The package directory is untouched by everything above.
        std::optional<addon::Validated_package> after = validator.validate(source, &failure);
        m_report.verdict("path.confinement",
            after.has_value() && after->generation_digest_hex == m_initial_digest,
            after.has_value() ? "generation digest unchanged" : failure.reason_code + ": " + failure.detail);
    }

private:
    void check_capability_declarations(const addon::Validated_package& package)
    {
        for (const addon::Capability_sidecar& sidecar : package.sidecars) {
            const std::string name = pair_name(sidecar.pair());
            if (sidecar.capability_id == addon::k_launcher_approval_capability_id &&
                sidecar.version == addon::k_launcher_approval_version)
            {
                addon::Contract_failure failure;
                const bool valid = addon::validate_launcher_approval_declaration(sidecar.payload, &failure);
                m_report.verdict("declaration.launcher_approval", valid,
                    valid ? name : name + ": " + failure.reason_code + ": " + failure.detail);
                continue;
            }
            if (sidecar.capability_id == addon::k_managed_configuration_capability_id &&
                (sidecar.version == addon::k_managed_configuration_version ||
                    sidecar.version == addon::k_managed_configuration_selector_version ||
                    sidecar.version == addon::k_managed_configuration_home_version))
            {
                addon::Contract_failure failure;
                const auto declaration = addon::decode_managed_configuration_declaration(
                    sidecar.payload, &failure, sidecar.version);
                m_report.verdict("declaration.managed_configuration", declaration.has_value(),
                    declaration ? name : name + ": " + failure.reason_code + ": " + failure.detail);
                continue;
            }
            if (sidecar.capability_id == addon::k_delegated_model_selection_capability_id &&
                sidecar.version == addon::k_delegated_model_selection_version)
            {
                addon::Contract_failure failure;
                const auto declaration = addon::decode_delegated_model_selection_declaration(sidecar.payload, &failure);
                m_report.verdict("declaration.delegated_model_selection", declaration.has_value(),
                    declaration ? name : name + ": " + failure.reason_code + ": " + failure.detail);
                continue;
            }
            if (sidecar.capability_id != addon::k_structured_session_capability_id &&
                sidecar.capability_id != addon::k_delegated_session_capability_id)
            {
                m_report.skip("declaration.unassessed", name + ": no declaration decoder in this suite");
                continue;
            }
            // Assess only the current session declaration grammars.
            if (sidecar.version != addon::k_structured_session_version) {
                m_report.skip("declaration.unassessed", name + ": unsupported declaration version");
                continue;
            }
            addon::Contract_failure failure;
            if (!addon::decode_purposes_declaration(sidecar.payload, &failure)) {
                m_report.fail("declaration.session_purposes",
                    name + ": " + failure.reason_code + ": " + failure.detail);
            }
            else {
                m_report.pass("declaration.session_purposes", name);
            }
        }
    }

    std::unique_ptr<addon::Host_simulator> start_session(const std::string& check)
    {
        if (!m_handshake_failure.empty()) {
            m_report.fail(check, "handshake failed earlier: " + m_handshake_failure);
            return nullptr;
        }
        auto session = std::make_unique<addon::Host_simulator>(m_options, nullptr);
        addon::Contract_failure failure;
        if (!session->start(&failure)) {
            m_handshake_failure = failure.reason_code + ": " + failure.detail;
            m_report.fail(check, m_handshake_failure);
            return nullptr;
        }
        if (check == "handshake.completes") {
            const addon::Adapter_hello& hello = *session->session()->adapter_hello();
            m_report.pass(check, hello.adapter_name + " " + hello.adapter_version +
                " generation " + std::to_string(hello.core_protocol_generation));
        }
        return session;
    }

    // A probe invocation that must produce a terminal outcome while the
    // generation stays ready.
    std::optional<addon::Host_outcome> probe(
        addon::Host_simulator&         session,
        const std::string&             operation,
        addon::Json_value              payload,
        const addon::Capability_pair&  pair = k_probe,
        bool                           bypass = false)
    {
        addon::Simulator_invocation_options invocation;
        invocation.bypass_liveness = bypass;
        const std::string id = session.invoke(pair, operation, std::move(payload), k_probe_deadline_ms, invocation);
        return session.wait_outcome(id, k_probe_deadline_ms + m_options.cancel_grace_ms + 500);
    }

    bool ready_or_fail(addon::Host_simulator& session, const std::string& check)
    {
        if (session.state() == addon::Simulator_state::READY) {
            return true;
        }
        m_report.fail(check, "generation retired: " + session.retirement().reason_code + ": " + session.retirement().detail);
        return false;
    }

    void check_handshake_agreement(addon::Host_simulator& session, const addon::Validated_package& package)
    {
        const std::vector<addon::Capability_pair>& handshake = session.session()->handshake_capabilities();
        std::string missing;
        for (const addon::Capability_pair& declared : package.declared_capabilities()) {
            // This exact metadata contract has no runtime handshake entry.
            if (declared == addon::Capability_pair{
                    addon::k_account_profile_managed_configuration_capability_id,
                    addon::k_account_profile_managed_configuration_version})
            {
                continue;
            }
            if (std::find(handshake.begin(), handshake.end(), declared) == handshake.end()) {
                missing += " " + pair_name(declared);
            }
        }
        m_report.verdict("handshake.sidecars_live", missing.empty(),
            missing.empty() ? "every handshake-required sidecar is in the handshake"
                            : "required sidecars absent from the handshake:" + missing);

        std::string undeclared;
        const std::vector<addon::Capability_pair> declared = package.declared_capabilities();
        for (const addon::Capability_pair& pair : handshake) {
            if (std::find(declared.begin(), declared.end(), pair) == declared.end()) {
                undeclared += " " + pair_name(pair);
            }
        }
        m_report.verdict("handshake.no_undeclared_pairs", undeclared.empty(),
            undeclared.empty() ? "every handshake pair has a sidecar" : "handshake pairs without a sidecar:" + undeclared);
    }

    void check_profile_storage(addon::Host_simulator& session)
    {
        const std::string check = "profile_storage.acknowledged";
        if (!m_profile_live) {
            m_report.skip(check, "profile-storage@1 is not live");
            return;
        }
        if (!ready_or_fail(session, check)) {
            return;
        }
        std::string error;
        const std::optional<std::string> root = addon::allocate_profile_storage_root(
            session.profile_storage_root(), session.package()->bootstrap.package_id, &error);
        if (!root.has_value()) {
            m_report.fail(check, "cannot allocate a root: " + error);
            return;
        }
        addon::Simulator_invocation_options invocation;
        invocation.profile_storage = addon::Profile_storage_context{addon::k_profile_storage_version, *root};
        const std::string id = session.invoke(k_profile_storage, addon::k_profile_storage_acknowledge,
            addon::Json_value::object(), k_probe_deadline_ms, invocation);
        const std::optional<addon::Host_outcome> outcome = session.wait_outcome(id, k_probe_deadline_ms + m_options.cancel_grace_ms + 500);
        m_report.verdict(check, outcome.has_value() && outcome->status == addon::Host_status::SUCCESS, describe(outcome));
    }

    void check_probe_basics(addon::Host_simulator& session)
    {
        if (!m_probe_live) {
            for (const char* check : {"invoke.echo_round_trip", "result.unknown_operation", "capability.unknown_pair_answered",
                                      "capability.unknown_version_local", "capability.unknown_local",
                                      "malformed.payload_rejected", "progress.delivery", "failure.package_local",
                                      "unavailable.reported", "proposal.detached_rejected"})
            {
                m_report.skip(check, "conformance-probe@1 is not live");
            }
            return;
        }

        {
            const std::string check = "invoke.echo_round_trip";
            if (ready_or_fail(session, check)) {
                addon::Json_value payload = addon::Json_value::object();
                payload.set("marker", addon::Json_value("conformance-echo"));
                payload.set("count",  addon::Json_value(3));
                const std::optional<addon::Host_outcome> outcome = probe(session, "echo", payload);
                m_report.verdict(check, echoed(outcome, payload), describe(outcome));
            }
        }
        {
            const std::string check = "result.unknown_operation";
            if (ready_or_fail(session, check)) {
                const std::optional<addon::Host_outcome> outcome = probe(session, "no_such_operation", addon::Json_value::object());
                m_report.verdict(check,
                    outcome.has_value() && outcome->status == addon::Host_status::UNAVAILABLE && outcome->reason_code == addon::reason::k_unknown_operation,
                    describe(outcome));
            }
        }
        {
            const std::string check = "capability.unknown_pair_answered";
            if (ready_or_fail(session, check)) {
                const std::optional<addon::Host_outcome> outcome =
                    probe(session, "noop", addon::Json_value::object(), addon::Capability_pair{"org.example.absent", 1}, true);
                m_report.verdict(check,
                    outcome.has_value() && outcome->status == addon::Host_status::UNAVAILABLE && outcome->reason_code == addon::reason::k_unknown_operation,
                    describe(outcome));
            }
        }
        {
            const std::string check = "capability.unknown_version_local";
            const std::optional<addon::Host_outcome> outcome =
                probe(session, "echo", addon::Json_value::object(), addon::Capability_pair{k_probe.capability_id, 7});
            m_report.verdict(check, outcome.has_value() && outcome->status == addon::Host_status::UNSUPPORTED_VERSION, describe(outcome));
        }
        {
            const std::string check = "capability.unknown_local";
            const std::optional<addon::Host_outcome> outcome =
                probe(session, "noop", addon::Json_value::object(), addon::Capability_pair{"org.example.absent", 1});
            m_report.verdict(check, outcome.has_value() && outcome->status == addon::Host_status::UNKNOWN_CAPABILITY, describe(outcome));
        }
        {
            const std::string check = "malformed.payload_rejected";
            if (ready_or_fail(session, check)) {
                addon::Json_value payload = addon::Json_value::object();
                payload.set("ms", addon::Json_value("soon"));
                const std::optional<addon::Host_outcome> outcome = probe(session, "sleep", payload);
                const bool graceful = outcome.has_value() &&
                    (outcome->status == addon::Host_status::FAILED || outcome->status == addon::Host_status::UNAVAILABLE) &&
                    session.state() == addon::Simulator_state::READY;
                m_report.verdict(check, graceful, describe(outcome));
            }
        }
        {
            const std::string check = "progress.delivery";
            if (ready_or_fail(session, check)) {
                addon::Json_value payload = addon::Json_value::object();
                payload.set("count", addon::Json_value(5));
                std::string id = session.invoke(k_probe, "progress", payload, k_probe_deadline_ms);
                std::optional<addon::Host_outcome> outcome = session.wait_outcome(id, k_probe_deadline_ms + m_options.cancel_grace_ms);
                const std::uint64_t seen = session.progress_count(id);
                bool ok = outcome.has_value() && outcome->status == addon::Host_status::SUCCESS && seen == 5;
                std::string detail = "5 requested, " + std::to_string(seen) + " observed";
                if (ok) {
                    const auto bound = session.session()->effective_limits().max_progress_frames_per_request;
                    constexpr std::int64_t reports = 8192;
                    payload.set("count", addon::Json_value(reports));
                    id      = session.invoke(k_probe, "progress", payload, k_probe_deadline_ms * 4);
                    outcome = session.wait_outcome(id, k_probe_deadline_ms * 4 + m_options.cancel_grace_ms);
                    const std::uint64_t flood_seen = session.progress_count(id);
                    ok = outcome.has_value() && outcome->status == addon::Host_status::SUCCESS &&
                         flood_seen <= bound && session.state() == addon::Simulator_state::READY;
                    detail += "; " + std::to_string(reports) + " requested, " + std::to_string(flood_seen) +
                              " observed" +
                              (session.state() == addon::Simulator_state::READY ? "" : ", retired " + session.retirement().reason_code);
                }
                m_report.verdict(check, ok, detail);
            }
        }
        {
            const std::string check = "failure.package_local";
            if (ready_or_fail(session, check)) {
                addon::Json_value payload = addon::Json_value::object();
                payload.set("reason",  addon::Json_value("probe_failure"));
                payload.set("message", addon::Json_value("requested failure"));
                const std::optional<addon::Host_outcome> failed = probe(session, "fail", payload);
                addon::Json_value echo = addon::Json_value::object();
                echo.set("after", addon::Json_value("failure"));
                const std::optional<addon::Host_outcome> recovered = probe(session, "echo", echo);
                const bool ok = failed.has_value() && failed->status == addon::Host_status::FAILED && failed->reason_code == "probe_failure" &&
                                recovered.has_value() && recovered->status == addon::Host_status::SUCCESS;
                m_report.verdict(check, ok, describe(failed) + ", then " + describe(recovered));
            }
        }
        {
            const std::string check = "unavailable.reported";
            if (ready_or_fail(session, check)) {
                const std::optional<addon::Host_outcome> outcome = probe(session, "unavailable", addon::Json_value::object());
                m_report.verdict(check, outcome.has_value() && outcome->status == addon::Host_status::UNAVAILABLE, describe(outcome));
            }
        }
        {
            const std::string check = "proposal.detached_rejected";
            if (ready_or_fail(session, check)) {
                const std::optional<addon::Host_outcome> outcome = probe(session, "propose_detached", addon::Json_value::object());
                if (!outcome.has_value() || outcome->status != addon::Host_status::SUCCESS) {
                    m_report.fail(check, describe(outcome));
                }
                else {
                    const addon::Json_value* proposal_value = outcome->payload.find("process_proposal");
                    addon::Contract_failure failure;
                    std::optional<addon::Process_proposal> proposal = proposal_value == nullptr
                        ? std::nullopt
                        : addon::decode_process_proposal(*proposal_value, &failure);
                    if (!proposal.has_value()) {
                        m_report.fail(check, "payload carries no decodable process_proposal: " + failure.reason_code + " " + failure.detail);
                    }
                    else {
                        const bool rejected = !addon::validate_process_proposal(*proposal, addon::Process_proposal_policy{}, &failure) &&
                                              failure.reason_code == addon::reason::k_detached_helper_proposal;
                        m_report.verdict(check, rejected, rejected ? "rejected as " + failure.reason_code : "proposal was accepted");
                    }
                }
            }
        }
    }

    // The exact frame size the simulator will send for this payload as an
    // echo invocation, including the request ID it will issue next.
    std::size_t frame_size_for(const addon::Host_simulator& session, const addon::Json_value& payload) const
    {
        addon::Invoke_request request;
        request.request_id  = session.next_request_id();
        request.capability  = k_probe;
        request.operation   = "echo";
        request.deadline_ms = k_probe_deadline_ms;
        request.payload     = payload;
        return addon::encode_frame(addon::encode_host_message(addon::Host_message(request))).size();
    }

    // An echo payload whose encoded invoke frame has exactly frame_bytes
    // bytes. The filler is an array of strings far below
    // k_max_json_string_bytes, so that only the frame bound is exercised;
    // each string costs its length plus two quotes, plus a comma after the
    // first.
    addon::Json_value sized_payload(const addon::Host_simulator& session, std::size_t frame_bytes) const
    {
        constexpr std::size_t k_chunk_bytes = 65536;
        addon::Json_value payload = addon::Json_value::object();
        payload.set("filler", addon::Json_value::array());
        addon::Json_value::Array& filler = payload.find("filler")->as_array();

        std::size_t size = frame_size_for(session, payload);
        if (size > frame_bytes) {
            return payload;   // the caller reports the size it actually got
        }
        while (size + k_chunk_bytes + 3 <= frame_bytes) {
            filler.push_back(addon::Json_value(std::string(k_chunk_bytes, 'x')));
            size = frame_size_for(session, payload);
        }
        std::size_t remaining = frame_bytes - size;
        if (remaining < 3 && !filler.empty()) {
            // Too small a gap for another string: shorten the last chunk instead.
            filler.pop_back();
            remaining += k_chunk_bytes + 3;
        }
        const std::size_t overhead = filler.empty() ? 2 : 3;
        if (remaining >= overhead) {
            filler.push_back(addon::Json_value(std::string(remaining - overhead, 'x')));
        }
        return payload;
    }

    void check_limits(addon::Host_simulator& session)
    {
        if (!m_probe_live) {
            for (const char* check : {"limits.frame_below", "limits.frame_at", "limits.frame_above",
                                      "limits.depth_at", "limits.depth_above"})
            {
                m_report.skip(check, "conformance-probe@1 is not live");
            }
            return;
        }
        const addon::Protocol_limits effective = session.session()->effective_limits();
        const std::size_t max_frame = (std::size_t)effective.max_frame_bytes;

        {
            const std::string check = "limits.frame_below";
            if (ready_or_fail(session, check)) {
                const addon::Json_value payload = sized_payload(session, max_frame / 2);
                const std::size_t actual = frame_size_for(session, payload);
                const std::optional<addon::Host_outcome> outcome = probe(session, "echo", payload);
                m_report.verdict(check, echoed(outcome, payload),
                    std::to_string(actual) + " bytes below bound " + std::to_string(max_frame) + ": " +
                    describe(outcome));
            }
        }
        {
            const std::string check = "limits.frame_at";
            if (ready_or_fail(session, check)) {
                const addon::Json_value payload = sized_payload(session, max_frame);
                const std::size_t actual = frame_size_for(session, payload);
                const std::optional<addon::Host_outcome> outcome = probe(session, "echo", payload);
                m_report.verdict(check, actual == max_frame && echoed(outcome, payload),
                    std::to_string(actual) + " bytes at bound " + std::to_string(max_frame) + ": " + describe(outcome));
            }
        }
        {
            const std::string check = "limits.frame_above";
            if (ready_or_fail(session, check)) {
                const addon::Json_value payload = sized_payload(session, max_frame + 1);
                const std::size_t actual = frame_size_for(session, payload);
                const std::optional<addon::Host_outcome> outcome = probe(session, "echo", payload);
                m_report.verdict(check, actual == max_frame + 1 && refused_locally(session, outcome),
                    std::to_string(actual) + " bytes refused locally: " + describe(outcome));
            }
        }

        const std::size_t max_depth = (std::size_t)effective.max_json_depth;
        const auto nested = [](std::size_t depth) {
            // The frame object is depth 1 and the payload object depth 2;
            // the remaining depth is nested arrays inside member "deep".
            addon::Json_value value = addon::Json_value::array();
            for (std::size_t i = 3; i < depth; ++i) {
                addon::Json_value outer = addon::Json_value::array();
                outer.push_back(std::move(value));
                value = std::move(outer);
            }
            addon::Json_value payload = addon::Json_value::object();
            payload.set("deep", std::move(value));
            return payload;
        };
        {
            const std::string check = "limits.depth_at";
            if (ready_or_fail(session, check)) {
                const addon::Json_value payload = nested(max_depth);
                const std::optional<addon::Host_outcome> outcome = probe(session, "echo", payload);
                m_report.verdict(check, echoed(outcome, payload),
                    "depth " + std::to_string(max_depth) + ": " + describe(outcome));
            }
        }
        {
            const std::string check = "limits.depth_above";
            if (ready_or_fail(session, check)) {
                const std::optional<addon::Host_outcome> outcome = probe(session, "echo", nested(max_depth + 1));
                m_report.verdict(check, refused_locally(session, outcome), "refused locally: " + describe(outcome));
            }
        }
    }

    void check_terminal_uniqueness(addon::Host_simulator& session)
    {
        const std::string check = "result.terminal_uniqueness";
        if (!m_probe_live) {
            m_report.skip(check, "conformance-probe@1 is not live");
            return;
        }
        if (!ready_or_fail(session, check)) {
            return;
        }
        addon::Json_value payload = addon::Json_value::object();
        payload.set("marker", addon::Json_value("uniqueness"));
        const std::optional<addon::Host_outcome> outcome = probe(session, "echo", payload);
        session.pump(300);   // a second result would arrive here
        const bool ok = outcome.has_value() && outcome->status == addon::Host_status::SUCCESS &&
                        session.state() == addon::Simulator_state::READY;
        m_report.verdict(check, ok, session.state() == addon::Simulator_state::READY
            ? describe(outcome)
            : describe(outcome) + ", then retired: " + session.retirement().reason_code);
    }

    void check_cancellation()
    {
        const std::string check = "cancel.sleep_cancelled";
        if (!m_probe_live) {
            m_report.skip(check, "conformance-probe@1 is not live");
            return;
        }
        std::unique_ptr<addon::Host_simulator> session = start_session(check);
        if (!session) {
            return;
        }
        addon::Json_value payload = addon::Json_value::object();
        payload.set("ms", addon::Json_value(60000));
        const std::string id = session->invoke(k_probe, "sleep", payload, 60000);
        session->pump(100);
        session->cancel(id);
        const std::optional<addon::Host_outcome> outcome = session->wait_outcome(id, m_options.cancel_grace_ms + 500);
        m_report.verdict(check, outcome.has_value() && outcome->status == addon::Host_status::CANCELLED, describe(outcome));
        session->shutdown(addon::limits::k_shutdown_grace_ms, nullptr);
    }

    void check_deadline()
    {
        const std::string check = "deadline.cancel_honoured";
        if (!m_probe_live) {
            m_report.skip(check, "conformance-probe@1 is not live");
            return;
        }
        std::unique_ptr<addon::Host_simulator> session = start_session(check);
        if (!session) {
            return;
        }
        addon::Json_value payload = addon::Json_value::object();
        payload.set("ms", addon::Json_value(60000));
        const std::string id = session->invoke(k_probe, "sleep", payload, 300);
        const std::optional<addon::Host_outcome> outcome = session->wait_outcome(id, 300 + m_options.cancel_grace_ms + 500);
        m_report.verdict(check, outcome.has_value() && outcome->status == addon::Host_status::CANCELLED, describe(outcome));
        session->shutdown(addon::limits::k_shutdown_grace_ms, nullptr);
    }

    void check_descendants()
    {
        const std::string check = "descendants.owned";
        if (!m_probe_live) {
            m_report.skip(check, "conformance-probe@1 is not live");
            return;
        }
        std::unique_ptr<addon::Host_simulator> session = start_session(check);
        if (!session) {
            return;
        }
        addon::Json_value payload = addon::Json_value::object();
        payload.set("ms", addon::Json_value(3000));
        const std::optional<addon::Host_outcome> outcome = probe(*session, "spawn_child", payload);
        std::int64_t child_pid = 0;
        if (!outcome.has_value() || outcome->status != addon::Host_status::SUCCESS) {
            m_report.fail(check, "spawn_child: " + describe(outcome));
        }
        else {
            const addon::Json_value* pid_value      = outcome->payload.find("child_pid");
            const addon::Json_value* detached_value = outcome->payload.find("detached");
            child_pid = pid_value != nullptr && pid_value->is_integer() ? pid_value->as_integer() : 0;
            const bool detached =
                detached_value == nullptr || !detached_value->is_boolean() || detached_value->as_boolean();
            const bool owned = child_pid > 0 && session->owns_process(child_pid);
            m_report.verdict(check, owned && !detached,
                "child " + std::to_string(child_pid) +
                (owned ? " in the adapter's group" : " outside the adapter's group") +
                (detached ? ", adapter reported detachment" : ""));
        }
        session->shutdown(addon::limits::k_shutdown_grace_ms, nullptr);
        if (child_pid > 0) {
            addon::kill_process(child_pid);   // a detached child is outside the group kill
        }
    }

    void check_graceful_shutdown()
    {
        const std::string check = "shutdown.graceful";
        std::unique_ptr<addon::Host_simulator> session = start_session(check);
        if (!session) {
            return;
        }
        int exit_code = -1;
        const bool exited = session->shutdown(addon::limits::k_shutdown_grace_ms, &exit_code);
        m_report.verdict(check, exited && exit_code == 0,
            exited ? "exited with code " + std::to_string(exit_code) : "did not exit within the grace");
    }

    addon::Simulator_options m_options;
    Report&                  m_report;
    std::string              m_initial_digest;
    std::string              m_handshake_failure;
    bool                     m_probe_live   = false;
    bool                     m_profile_live = false;
};

int usage()
{
    std::fprintf(stderr, "usage: logonomic_cli_addon_conformance <package-dir> [--platform <id>] [--no-isolation]\n");
    return 2;
}

} // namespace

int run(int argc, char** argv)
{
    std::string directory;
    addon::Simulator_options options;
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument == "--platform" && i + 1 < argc)  { options.validation.platform_id = argv[++i]; } else
        if (argument == "--no-isolation")              { options.isolate_process_group = false; }      else
        if (!argument.empty() && argument[0] == '-')   { return usage(); }                             else
        if (directory.empty())                         { directory = argument; }                       else {
            return usage();
        }
    }
    if (directory.empty() || !addon::is_valid_platform_id(options.validation.platform_id)) {
        return usage();
    }
    options.package_directory = addon::path_from_utf8(directory);

    Report report;
    Suite suite(std::move(options), report);
    suite.run();
    return report.failed() ? 1 : 0;
}

#ifdef _WIN32
int wmain(int argc, wchar_t** argv)
{
    return logonomic::cli_addon::tools::run_utf8_arguments(argc, argv, run);
}
#else
int main(int argc, char** argv)
{
    return run(argc, argv);
}
#endif
