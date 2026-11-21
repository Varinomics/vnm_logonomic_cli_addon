#pragma once

#include "logonomic_cli_addon/ids.h"
#include "logonomic_cli_addon/json.h"
#include "logonomic_cli_addon/limits.h"

#include <cstdint>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

// The control protocol between the desktop (Generation_client) and the
// custodian, carried as one JSON object per Duplex_channel message (the
// channel adds the 4-byte big-endian length prefix; payloads are bounded by
// limits::k_max_control_message_bytes). Logonomic-internal: only the adapter
// protocol is SDK contract.
//
// Sequence. The desktop sends exactly one `start`. The custodian answers
// `error` (and exits) or `started`; a malformed control message after that
// is answered with `error` and the run goes on. The custodian sends `exited`
// for the supervisor and for the adapter as it reaps each of them, once per
// process: live, when one ends on its own, or during the teardown that
// `stop` or control-channel EOF (desktop loss) begins, with the status the
// teardown's signal produced. Both `exited` messages therefore precede the
// one `reaped` that ends the run (after desktop loss nobody receives them).
// The desktop reads EOF after `reaped`, or after an `error` that preceded
// `started`, as a clean end; EOF at any other point is custodian loss,
// answered with terminate_coordinates() on the `started` coordinates.
// Darwin's optional custody record admits original-group cleanup only. Its
// adapter exit travels through the supervisor's ordered link after final
// output; Reaped.complete stays false, and owned_groups_confirmed carries
// the scoped receipt. A normal custodian exit cannot replace that receipt.
namespace logonomic::cli_addon_host::custodian_protocol {

// A second cleanup acknowledgement when the control receipt cannot arrive.
// Only the custodian emits this after exact tree settlement (or before any
// tree could be created). Ordinary exit, EOF and forced termination do not
// carry this proof. Activation failure remains a separate protocol outcome.
inline constexpr int k_custody_clean_exit_code = 74;

struct Start_limits
{
    std::uint64_t max_frame_bytes       = cli_addon::limits::k_max_frame_bytes;
    std::uint64_t stderr_ring_bytes     = cli_addon::limits::k_stderr_ring_bytes;
    std::uint64_t max_link_frame_bytes  = cli_addon::limits::k_max_link_frame_bytes;
    std::uint64_t max_stdin_queue_bytes = cli_addon::limits::k_max_stdin_queue_bytes;
};

// Optional host capability. Its version does not change the adapter protocol
// or the common control schema; unknown versions cannot authorize Mac custody.
struct Darwin_custody
{
    std::int64_t version = 1;
    std::string root_birth_token;
    bool owned_groups_confirmed = false;
};

/// desktop -> custodian. `environment` is the allowlisted platform baseline
/// plus the manifest's adapter.environment, in NAME/VALUE pairs; the
/// custodian rejects any reserved (LOGONOMIC_-prefixed) name and appends the
/// reserved variables itself from the other members. The generation
/// directory is <custody_root>/<package_id>/<snapshot_nonce>. Reopening the
/// same snapshot gives the adapter a fresh generation_nonce and scratch.
/// `link_child_number` is where the supervisor-link end was inherited into
/// the custodian: the descriptor number on Linux, the handle value on Windows.
struct Start_request
{
    std::string                                      package_id;
    std::string                                      generation_digest;      // 64 lowercase hex
    std::string                                      generation_nonce;       // 32 lowercase hex
    std::string                                      snapshot_nonce;         // frozen directory, 32 lowercase hex
    std::string                                      custody_root;           // absolute
    std::string                                      scratch_root;           // absolute, host-owned, per generation
    std::string                                      supervisor_executable;  // absolute
    std::string                                      adapter_relative_path;  // canonical path below adapter/
    std::vector<std::string>                         adapter_argv;           // arguments after argv[0]
    std::vector<std::pair<std::string, std::string>> environment;
    std::vector<std::int64_t>                        core_protocol_generations;   // the host's offered set
    std::int64_t                                     link_child_number = -1;
    Start_limits                                     limits;
    std::optional<Darwin_custody>                    darwin_custody;
};

/// desktop -> custodian. grace_ms is the time the tree gets between
/// terminate (SIGTERM) and kill (SIGKILL), clamped by the custodian to
/// [0, limits::k_terminate_to_kill_ms]. 0 sends no terminate at all: the
/// tree is killed at once - the retirement at cancel-grace expiry, where
/// nothing is asked of the adapter any more - and both `exited` messages
/// then carry the kill signal.
struct Stop_request
{
    int grace_ms = cli_addon::limits::k_terminate_to_kill_ms;
};

/// After the adapter's idle shutdown grace, end the adapter under its native
/// owner while the supervisor can still drain and report ordered evidence.
struct Force_idle_request {};

/// custodian -> desktop. On Linux the message carries the pidfds of
/// supervisor and adapter, in that order, as transferred descriptors.
struct Started
{
    std::int64_t supervisor_pid = 0;
    std::int64_t adapter_pid    = 0;
    std::int64_t process_group  = 0;   // 0 on Windows
    std::optional<Darwin_custody> darwin_custody;
};

/// custodian -> desktop: a status the custodian reaped for one of the two
/// processes it named in `started`. exit_code is -1 when a signal ended the
/// process, signal is 0 when it exited on its own.
struct Exited
{
    std::string who;   // "supervisor" | "adapter"
    int         exit_code = -1;
    int         signal    = 0;
};

/// custodian -> desktop, once, before a clean exit; every `exited` the run
/// produces precedes it. escalated_to_kill records that SIGKILL was sent -
/// after grace_ms passed, or at once for grace_ms 0; processes_reaped counts
/// every status the custodian collected over the run (supervisor, adapter and
/// reparented grandchildren); remaining names tree processes still alive when
/// an incomplete teardown gave up.
struct Reaped
{
    bool                      complete          = false;   // nothing of the tree is left
    bool                      escalated_to_kill = false;
    int                       processes_reaped  = 0;
    std::vector<std::int64_t> remaining;
    std::optional<Darwin_custody> darwin_custody;
    bool idle_adapter_forced = false;   // scoped settlement corroborates the requested adapter termination
};

/// custodian -> desktop. reason is from cli_addon::reason.
struct Error
{
    std::string reason;
    std::string detail;
};

using Message = std::variant<Start_request, Stop_request, Force_idle_request, Started, Exited, Reaped, Error>;

/// Why decode() rejected a control message. reason is one of
/// cli_addon::reason::k_frame_violation, k_unexpected_member,
/// k_unknown_message_type, k_oversize_frame and k_invalid_utf8.
struct Decode_failure
{
    std::string reason;
    std::string detail;
};

inline constexpr const char* k_type_start   = "start";
inline constexpr const char* k_type_stop    = "stop";
inline constexpr const char* k_type_force_idle = "force_idle";
inline constexpr const char* k_type_started = "started";
inline constexpr const char* k_type_exited  = "exited";
inline constexpr const char* k_type_reaped  = "reaped";
inline constexpr const char* k_type_error   = "error";

namespace detail {

inline cli_addon::Json_value integer_value(std::int64_t in_value) { return cli_addon::Json_value(in_value); }
inline cli_addon::Json_value string_value(const std::string& in_value) { return cli_addon::Json_value(in_value); }

inline cli_addon::Json_value string_array(const std::vector<std::string>& in_values)
{
    cli_addon::Json_value array = cli_addon::Json_value::array();
    for (const std::string& value : in_values) {
        array.push_back(cli_addon::Json_value(value));
    }
    return array;
}

inline cli_addon::Json_value integer_array(const std::vector<std::int64_t>& in_values)
{
    cli_addon::Json_value array = cli_addon::Json_value::array();
    for (const std::int64_t value : in_values) {
        array.push_back(cli_addon::Json_value(value));
    }
    return array;
}

/// Strict member access over one object: every member must be expected, and
/// every expected member is read with its type checked. Failures name the
/// member so the reason is actionable.
class Object_reader
{
public:
    Object_reader(
        const cli_addon::Json_value&                       in_object,
        std::initializer_list<std::string_view> in_expected,
        Decode_failure*                         out_failure)
    :
        m_object(in_object),
        m_failure(out_failure)
    {
        for (const cli_addon::Json_member& member : in_object.as_object()) {
            bool expected = false;
            for (const std::string_view name : in_expected) {
                if (member.name == name) {
                    expected = true;
                    break;
                }
            }
            if (!expected) {
                fail(cli_addon::reason::k_unexpected_member, "unexpected member \"" + member.name + "\"");
                return;
            }
        }
    }

    bool ok() const { return m_ok; }

    bool string(std::string_view in_name, std::string* out_value)
    {
        const cli_addon::Json_value* value = require(in_name, cli_addon::Json_type::STRING, "a string");
        if (!value) {
            return false;
        }
        *out_value = value->as_string();
        return true;
    }

    bool integer(std::string_view in_name, std::int64_t* out_value)
    {
        const cli_addon::Json_value* value = require(in_name, cli_addon::Json_type::INTEGER, "an integer");
        if (!value) {
            return false;
        }
        *out_value = value->as_integer();
        return true;
    }

    bool boolean(std::string_view in_name, bool* out_value)
    {
        const cli_addon::Json_value* value = require(in_name, cli_addon::Json_type::BOOLEAN, "a boolean");
        if (!value) {
            return false;
        }
        *out_value = value->as_boolean();
        return true;
    }

    const cli_addon::Json_value* array(std::string_view in_name)  { return require(in_name, cli_addon::Json_type::ARRAY,  "an array"); }
    const cli_addon::Json_value* object(std::string_view in_name) { return require(in_name, cli_addon::Json_type::OBJECT, "an object"); }

    bool string_array(std::string_view in_name, std::vector<std::string>* out_values)
    {
        const cli_addon::Json_value* array = this->array(in_name);
        if (!array) {
            return false;
        }
        out_values->clear();
        for (const cli_addon::Json_value& item : array->as_array()) {
            if (!item.is_string()) {
                return fail(cli_addon::reason::k_frame_violation,
                    "member \"" + std::string(in_name) + "\" must hold strings");
            }
            out_values->push_back(item.as_string());
        }
        return true;
    }

    bool integer_array(std::string_view in_name, std::vector<std::int64_t>* out_values)
    {
        const cli_addon::Json_value* array = this->array(in_name);
        if (!array) {
            return false;
        }
        out_values->clear();
        for (const cli_addon::Json_value& item : array->as_array()) {
            if (!item.is_integer()) {
                return fail(cli_addon::reason::k_frame_violation,
                    "member \"" + std::string(in_name) + "\" must hold integers");
            }
            out_values->push_back(item.as_integer());
        }
        return true;
    }

    bool fail(const char* in_reason, std::string in_detail)
    {
        if (m_ok) {
            m_ok = false;
            if (m_failure) {
                m_failure->reason = in_reason;
                m_failure->detail = std::move(in_detail);
            }
        }
        return false;
    }

private:
    const cli_addon::Json_value* require(std::string_view in_name, cli_addon::Json_type in_type, const char* in_type_name)
    {
        if (!m_ok) {
            return nullptr;
        }
        const cli_addon::Json_value* value = m_object.find(in_name);
        if (!value) {
            fail(cli_addon::reason::k_frame_violation, "member \"" + std::string(in_name) + "\" is missing");
            return nullptr;
        }
        if (value->type() != in_type) {
            fail(cli_addon::reason::k_frame_violation,
                "member \"" + std::string(in_name) + "\" must be " + in_type_name);
            return nullptr;
        }
        return value;
    }

    const cli_addon::Json_value& m_object;
    Decode_failure*   m_failure;
    bool              m_ok = true;
};

inline bool integer_in_int_range(std::int64_t in_value)
{
    return in_value >= -2147483647LL - 1 && in_value <= 2147483647LL;
}

inline void encode_darwin_custody(
    cli_addon::Json_value& object, const std::optional<Darwin_custody>& record)
{
    if (!record) {
        return;
    }
    auto value = cli_addon::Json_value::object();
    value.set("version", integer_value(record->version));
    value.set("root_birth_token", string_value(record->root_birth_token));
    value.set("owned_groups_confirmed", cli_addon::Json_value(record->owned_groups_confirmed));
    object.set("darwin_custody", std::move(value));
}

inline void decode_darwin_custody(
    const cli_addon::Json_value& object, std::optional<Darwin_custody>& record)
{
    const auto* value = object.find("darwin_custody");
    if (!value || !value->is_object()) {
        return;
    }
    const auto* version = value->find("version");
    const auto* token = value->find("root_birth_token");
    const auto* confirmed = value->find("owned_groups_confirmed");
    if (!version || !version->is_integer() || version->as_integer() != 1 ||
        !token || !token->is_string() || !confirmed || !confirmed->is_boolean())
    {
        return;
    }
    record = Darwin_custody{version->as_integer(), token->as_string(), confirmed->as_boolean()};
}

} // namespace detail

inline std::string encode(const Message& in_message)
{
    using cli_addon::Json_value;
    Json_value object = Json_value::object();

    if (const Start_request* start = std::get_if<Start_request>(&in_message)) {
        object.set("type", Json_value(k_type_start));
        object.set("package_id",            detail::string_value(start->package_id));
        object.set("generation_digest",     detail::string_value(start->generation_digest));
        object.set("generation_nonce",      detail::string_value(start->generation_nonce));
        object.set("snapshot_nonce",        detail::string_value(start->snapshot_nonce));
        object.set("custody_root",          detail::string_value(start->custody_root));
        object.set("scratch_root",          detail::string_value(start->scratch_root));
        object.set("supervisor_executable", detail::string_value(start->supervisor_executable));
        object.set("adapter_relative_path", detail::string_value(start->adapter_relative_path));
        object.set("adapter_argv",          detail::string_array(start->adapter_argv));
        Json_value environment = Json_value::object();
        for (const auto& [name, value] : start->environment) {
            environment.set(name, Json_value(value));
        }
        object.set("environment", std::move(environment));
        object.set("core_protocol_generations", detail::integer_array(start->core_protocol_generations));
        object.set("link_child_number", detail::integer_value(start->link_child_number));
        Json_value limits = Json_value::object();
        limits.set("max_frame_bytes",       detail::integer_value(static_cast<std::int64_t>(start->limits.max_frame_bytes)));
        limits.set("stderr_ring_bytes",     detail::integer_value(static_cast<std::int64_t>(start->limits.stderr_ring_bytes)));
        limits.set("max_link_frame_bytes",  detail::integer_value(static_cast<std::int64_t>(start->limits.max_link_frame_bytes)));
        limits.set("max_stdin_queue_bytes", detail::integer_value(static_cast<std::int64_t>(start->limits.max_stdin_queue_bytes)));
        object.set("limits", std::move(limits));
        detail::encode_darwin_custody(object, start->darwin_custody);
    }
    else
    if (const Stop_request* stop = std::get_if<Stop_request>(&in_message)) {
        object.set("type", Json_value(k_type_stop));
        object.set("grace_ms", detail::integer_value(stop->grace_ms));
    }
    else
    if (std::holds_alternative<Force_idle_request>(in_message)) {
        object.set("type", Json_value(k_type_force_idle));
    }
    else
    if (const Started* started = std::get_if<Started>(&in_message)) {
        object.set("type", Json_value(k_type_started));
        object.set("supervisor_pid", detail::integer_value(started->supervisor_pid));
        object.set("adapter_pid",    detail::integer_value(started->adapter_pid));
        object.set("process_group",  detail::integer_value(started->process_group));
        detail::encode_darwin_custody(object, started->darwin_custody);
    }
    else
    if (const Exited* exited = std::get_if<Exited>(&in_message)) {
        object.set("type", Json_value(k_type_exited));
        object.set("who",       detail::string_value(exited->who));
        object.set("exit_code", detail::integer_value(exited->exit_code));
        object.set("signal",    detail::integer_value(exited->signal));
    }
    else
    if (const Reaped* reaped = std::get_if<Reaped>(&in_message)) {
        object.set("type", Json_value(k_type_reaped));
        object.set("complete",          Json_value(reaped->complete));
        object.set("escalated_to_kill", Json_value(reaped->escalated_to_kill));
        object.set("processes_reaped",  detail::integer_value(reaped->processes_reaped));
        object.set("remaining",         detail::integer_array(reaped->remaining));
        if (reaped->idle_adapter_forced) {
            object.set("idle_adapter_forced", Json_value(true));
        }
        detail::encode_darwin_custody(object, reaped->darwin_custody);
    }
    else
    if (const Error* error = std::get_if<Error>(&in_message)) {
        object.set("type", Json_value(k_type_error));
        object.set("reason", detail::string_value(error->reason));
        object.set("detail", detail::string_value(error->detail));
    }
    return cli_addon::serialize_json(object);
}

/// Structural decode: JSON bounds, the closed member set per type, member
/// types and integer ranges. Semantic validation of a `start` (grammars,
/// paths, bounds) is the custodian's, see logonomic_cli_addon_custodian_main.cpp.
inline std::optional<Message> decode(std::string_view in_bytes, Decode_failure* out_failure)
{
    using cli_addon::Json_value;

    cli_addon::Json_parse_limits parse_limits;
    parse_limits.max_bytes = cli_addon::limits::k_max_control_message_bytes;
    cli_addon::Json_parse_error parse_error;
    const std::optional<Json_value> parsed = cli_addon::parse_json(in_bytes, parse_limits, &parse_error);
    if (!parsed) {
        out_failure->reason = cli_addon::is_valid_utf8(in_bytes)
            ? cli_addon::reason::k_frame_violation
            : cli_addon::reason::k_invalid_utf8;
        out_failure->detail = "control message is not valid JSON: " + parse_error.message;
        return std::nullopt;
    }
    if (!parsed->is_object()) {
        out_failure->reason = cli_addon::reason::k_frame_violation;
        out_failure->detail = "control message must be a JSON object";
        return std::nullopt;
    }
    const Json_value* type = parsed->find("type");
    if (!type || !type->is_string()) {
        out_failure->reason = cli_addon::reason::k_frame_violation;
        out_failure->detail = "control message has no string \"type\" member";
        return std::nullopt;
    }
    const std::string& type_name = type->as_string();

    if (type_name == k_type_start) {
        Start_request start;
        detail::Object_reader reader(
            *parsed,
            {
                "type", "package_id", "generation_digest", "generation_nonce", "snapshot_nonce", "custody_root",
                "scratch_root", "supervisor_executable", "adapter_relative_path", "adapter_argv",
                "environment", "core_protocol_generations", "link_child_number", "limits", "darwin_custody",
            },
            out_failure);
        std::int64_t link_child_number = 0;
        const bool members_ok =
            reader.ok()                                                          &&
            reader.string("package_id",            &start.package_id)            &&
            reader.string("generation_digest",     &start.generation_digest)     &&
            reader.string("generation_nonce",      &start.generation_nonce)      &&
            reader.string("snapshot_nonce",        &start.snapshot_nonce)        &&
            reader.string("custody_root",          &start.custody_root)          &&
            reader.string("scratch_root",          &start.scratch_root)          &&
            reader.string("supervisor_executable", &start.supervisor_executable) &&
            reader.string("adapter_relative_path", &start.adapter_relative_path) &&
            reader.string_array("adapter_argv",    &start.adapter_argv)          &&
            reader.integer_array("core_protocol_generations", &start.core_protocol_generations) &&
            reader.integer("link_child_number",    &link_child_number);
        if (!members_ok) {
            return std::nullopt;
        }
        start.link_child_number = link_child_number;

        const Json_value* environment = reader.object("environment");
        if (!environment) {
            return std::nullopt;
        }
        for (const cli_addon::Json_member& member : environment->as_object()) {
            if (!member.value.is_string()) {
                reader.fail(cli_addon::reason::k_frame_violation,
                    "environment value of \"" + member.name + "\" must be a string");
                return std::nullopt;
            }
            start.environment.emplace_back(member.name, member.value.as_string());
        }

        const Json_value* limits = reader.object("limits");
        if (!limits) {
            return std::nullopt;
        }
        detail::Object_reader limits_reader(
            *limits,
            {"max_frame_bytes", "stderr_ring_bytes", "max_link_frame_bytes", "max_stdin_queue_bytes"},
            out_failure);
        std::int64_t max_frame_bytes       = 0;
        std::int64_t stderr_ring_bytes     = 0;
        std::int64_t max_link_frame_bytes  = 0;
        std::int64_t max_stdin_queue_bytes = 0;
        const bool limits_ok =
            limits_reader.ok()                                                &&
            limits_reader.integer("max_frame_bytes",       &max_frame_bytes)       &&
            limits_reader.integer("stderr_ring_bytes",     &stderr_ring_bytes)     &&
            limits_reader.integer("max_link_frame_bytes",  &max_link_frame_bytes)  &&
            limits_reader.integer("max_stdin_queue_bytes", &max_stdin_queue_bytes);
        if (!limits_ok) {
            return std::nullopt;
        }
        if (max_frame_bytes <= 0 || stderr_ring_bytes < 0 || max_link_frame_bytes <= 0 || max_stdin_queue_bytes <= 0) {
            limits_reader.fail(cli_addon::reason::k_frame_violation, "a limit is not positive");
            return std::nullopt;
        }
        start.limits.max_frame_bytes       = static_cast<std::uint64_t>(max_frame_bytes);
        start.limits.stderr_ring_bytes     = static_cast<std::uint64_t>(stderr_ring_bytes);
        start.limits.max_link_frame_bytes  = static_cast<std::uint64_t>(max_link_frame_bytes);
        start.limits.max_stdin_queue_bytes = static_cast<std::uint64_t>(max_stdin_queue_bytes);
        detail::decode_darwin_custody(*parsed, start.darwin_custody);
        return Message(std::move(start));
    }

    if (type_name == k_type_stop) {
        Stop_request stop;
        detail::Object_reader reader(*parsed, {"type", "grace_ms"}, out_failure);
        std::int64_t grace_ms = 0;
        if (!reader.ok() || !reader.integer("grace_ms", &grace_ms)) {
            return std::nullopt;
        }
        if (!detail::integer_in_int_range(grace_ms) || grace_ms < 0) {
            reader.fail(cli_addon::reason::k_frame_violation, "member \"grace_ms\" is out of range");
            return std::nullopt;
        }
        stop.grace_ms = static_cast<int>(grace_ms);
        return Message(stop);
    }

    if (type_name == k_type_started) {
        Started started;
        detail::Object_reader reader(*parsed,
            {"type", "supervisor_pid", "adapter_pid", "process_group", "darwin_custody"}, out_failure);
        const bool ok =
            reader.ok()                                            &&
            reader.integer("supervisor_pid", &started.supervisor_pid) &&
            reader.integer("adapter_pid",    &started.adapter_pid)    &&
            reader.integer("process_group",  &started.process_group);
        if (!ok) {
            return std::nullopt;
        }
        detail::decode_darwin_custody(*parsed, started.darwin_custody);
        return Message(started);
    }

    if (type_name == k_type_force_idle) {
        detail::Object_reader reader(*parsed, {"type"}, out_failure);
        if (!reader.ok()) {
            return std::nullopt;
        }
        return Message(Force_idle_request{});
    }

    if (type_name == k_type_exited) {
        Exited exited;
        detail::Object_reader reader(*parsed, {"type", "who", "exit_code", "signal"}, out_failure);
        std::int64_t exit_code = 0;
        std::int64_t signal    = 0;
        const bool ok =
            reader.ok()                              &&
            reader.string("who",        &exited.who) &&
            reader.integer("exit_code", &exit_code)  &&
            reader.integer("signal",    &signal);
        if (!ok) {
            return std::nullopt;
        }
        if (!detail::integer_in_int_range(exit_code) || !detail::integer_in_int_range(signal)) {
            reader.fail(cli_addon::reason::k_frame_violation, "exit status is out of range");
            return std::nullopt;
        }
        if (exited.who != "supervisor" && exited.who != "adapter") {
            reader.fail(cli_addon::reason::k_frame_violation, "member \"who\" must be \"supervisor\" or \"adapter\"");
            return std::nullopt;
        }
        exited.exit_code = static_cast<int>(exit_code);
        exited.signal    = static_cast<int>(signal);
        return Message(std::move(exited));
    }

    if (type_name == k_type_reaped) {
        Reaped reaped;
        detail::Object_reader reader(
            *parsed,
            {"type", "complete", "escalated_to_kill", "processes_reaped", "remaining",
             "darwin_custody", "idle_adapter_forced"},
            out_failure);
        std::int64_t processes_reaped = 0;
        const bool ok =
            reader.ok()                                                      &&
            reader.boolean("complete",          &reaped.complete)            &&
            reader.boolean("escalated_to_kill", &reaped.escalated_to_kill)   &&
            reader.integer("processes_reaped",  &processes_reaped)           &&
            reader.integer_array("remaining",   &reaped.remaining);
        if (!ok) {
            return std::nullopt;
        }
        if (!detail::integer_in_int_range(processes_reaped)) {
            reader.fail(cli_addon::reason::k_frame_violation, "member \"processes_reaped\" is out of range");
            return std::nullopt;
        }
        reaped.processes_reaped = static_cast<int>(processes_reaped);
        if (parsed->find("idle_adapter_forced") &&
            !reader.boolean("idle_adapter_forced", &reaped.idle_adapter_forced))
        {
            return std::nullopt;
        }
        detail::decode_darwin_custody(*parsed, reaped.darwin_custody);
        return Message(std::move(reaped));
    }

    if (type_name == k_type_error) {
        Error error;
        detail::Object_reader reader(*parsed, {"type", "reason", "detail"}, out_failure);
        const bool ok =
            reader.ok()                               &&
            reader.string("reason", &error.reason)    &&
            reader.string("detail", &error.detail);
        if (!ok) {
            return std::nullopt;
        }
        return Message(std::move(error));
    }

    out_failure->reason = cli_addon::reason::k_unknown_message_type;
    out_failure->detail = "control message type \"" + type_name + "\" is not known";
    return std::nullopt;
}

} // namespace logonomic::cli_addon_host::custodian_protocol
