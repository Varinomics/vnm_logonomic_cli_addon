#include "logonomic_cli_addon/protocol.h"
#include "logonomic_cli_addon/version.h"
#include "strict_json.h"

#include <algorithm>
#include <limits>

namespace logonomic::cli_addon {

namespace {

constexpr std::int64_t k_max_duration_ms = std::numeric_limits<std::int32_t>::max();

// Request IDs are canonical decimal strings. Comparing digit count and then
// bytes avoids a machine-integer ceiling even though the grammar permits a
// 63-digit suffix.
bool request_id_not_after(std::string_view candidate, std::string_view high_water)
{
    const std::string_view candidate_digits = candidate.substr(1);
    const std::string_view high_water_digits = high_water.substr(1);
    return candidate_digits.size() != high_water_digits.size()
        ? candidate_digits.size() < high_water_digits.size()
        : candidate_digits <= high_water_digits;
}

bool report(Contract_failure* out_failure, const char* reason_code, std::string detail)
{
    if (out_failure != nullptr) {
        out_failure->reason_code = reason_code;
        out_failure->detail      = std::move(detail);
    }
    return false;
}

template <typename Message>
std::optional<Message> fail(Contract_failure* out_failure, const char* reason_code, std::string detail)
{
    report(out_failure, reason_code, std::move(detail));
    return std::nullopt;
}

Json_value encode_capability_array(const std::vector<Capability_pair>& pairs)
{
    Json_value array = Json_value::array();
    for (const Capability_pair& pair : pairs) {
        Json_value entry = Json_value::object();
        entry.set("capability_id", Json_value(pair.capability_id));
        entry.set("version",       Json_value((std::int64_t)pair.version));
        array.push_back(std::move(entry));
    }
    return array;
}

Json_value encode_limits(const Protocol_limits& limits)
{
    Json_value object = Json_value::object();
    object.set("max_frame_bytes",                 Json_value((std::int64_t)limits.max_frame_bytes));
    object.set("max_json_depth",                  Json_value((std::int64_t)limits.max_json_depth));
    object.set("max_in_flight_requests",          Json_value((std::int64_t)limits.max_in_flight_requests));
    object.set("max_progress_frames_per_request", Json_value((std::int64_t)limits.max_progress_frames_per_request));
    return object;
}

Json_value encode_adapter_limits(const Adapter_limits& limits)
{
    Json_value object = Json_value::object();
    if (limits.max_frame_bytes)                 { object.set("max_frame_bytes",                 Json_value((std::int64_t)*limits.max_frame_bytes)); }
    if (limits.max_json_depth)                  { object.set("max_json_depth",                  Json_value((std::int64_t)*limits.max_json_depth)); }
    if (limits.max_in_flight_requests)          { object.set("max_in_flight_requests",          Json_value((std::int64_t)*limits.max_in_flight_requests)); }
    if (limits.max_progress_frames_per_request) { object.set("max_progress_frames_per_request", Json_value((std::int64_t)*limits.max_progress_frames_per_request)); }
    return object;
}

// Parses the frame into an object and reads its type; shared by both sides.
std::optional<Json_value> decode_envelope(
    std::string_view         frame,
    const Json_parse_limits& parse_limits,
    std::string&             out_type,
    Contract_failure*        out_failure)
{
    Json_parse_error parse_error;
    std::optional<Json_value> document = parse_json(frame, parse_limits, &parse_error);
    if (!document.has_value()) {
        report(out_failure, reason::k_frame_violation,
            "frame is not valid JSON at byte " + std::to_string(parse_error.offset) + ": " + parse_error.message);
        return std::nullopt;
    }
    if (!document->is_object()) {
        report(out_failure, reason::k_frame_violation, "frame is not a JSON object");
        return std::nullopt;
    }
    const std::string* type = strict::string_member(*document, "type");
    if (type == nullptr) {
        report(out_failure, reason::k_frame_violation, "frame has no string member type");
        return std::nullopt;
    }
    out_type = *type;
    return document;
}

bool check_members(
    const Json_value&                       object,
    std::string_view                        type,
    std::initializer_list<std::string_view> allowed,
    Contract_failure*                       out_failure)
{
    const std::optional<std::string> unexpected = strict::unexpected_member(object, allowed);
    if (unexpected.has_value()) {
        return report(out_failure, reason::k_unexpected_member, std::string(type) + "." + *unexpected);
    }
    return true;
}

bool read_request_id(const Json_value& object, std::string_view type, std::string& out_id, Contract_failure* out_failure)
{
    const std::string* request_id = strict::string_member(object, "request_id");
    if (request_id == nullptr || !is_valid_request_id(*request_id)) {
        return report(out_failure, reason::k_frame_violation, std::string(type) + ".request_id is missing or out of grammar");
    }
    out_id = *request_id;
    return true;
}

bool read_identity(
    const Json_value& object,
    std::string_view  type,
    std::string&      out_package_id,
    std::string&      out_digest,
    std::string&      out_nonce,
    Contract_failure* out_failure)
{
    const std::string* package_id = strict::string_member(object, "package_id");
    const std::string* digest     = strict::string_member(object, "generation_digest");
    const std::string* nonce      = strict::string_member(object, "generation_nonce");
    if (package_id == nullptr || !is_valid_package_id(*package_id)) {
        return report(out_failure, reason::k_frame_violation, std::string(type) + ".package_id is missing or out of grammar");
    }
    if (digest == nullptr || !is_valid_sha256_hex(*digest)) {
        return report(out_failure, reason::k_frame_violation, std::string(type) + ".generation_digest is missing or not 64 hex characters");
    }
    if (nonce == nullptr || !is_valid_generation_nonce_hex(*nonce)) {
        return report(out_failure, reason::k_frame_violation, std::string(type) + ".generation_nonce is missing or not 32 hex characters");
    }
    out_package_id = *package_id;
    out_digest     = *digest;
    out_nonce      = *nonce;
    return true;
}

bool read_informational_pair(
    const Json_value& object,
    std::string_view  member,
    std::string_view  first,
    std::string_view  second,
    std::string&      out_first,
    std::string&      out_second,
    Contract_failure* out_failure)
{
    const Json_value* pair = strict::member_of_type(object, member, Json_type::OBJECT);
    if (pair == nullptr) {
        return report(out_failure, reason::k_frame_violation, std::string(member) + " is missing or not an object");
    }
    if (!check_members(*pair, member, {first, second}, out_failure)) {
        return false;
    }
    const std::string* first_value  = strict::string_member(*pair, first);
    const std::string* second_value = strict::string_member(*pair, second);
    if (first_value == nullptr || second_value == nullptr ||
        !strict::has_no_control_characters(*first_value) || !strict::has_no_control_characters(*second_value))
    {
        return report(out_failure, reason::k_frame_violation, std::string(member) + " members must be strings");
    }
    out_first  = *first_value;
    out_second = *second_value;
    return true;
}

std::optional<std::uint64_t> read_limit(const Json_value& limits, std::string_view name, bool required, bool& ok)
{
    const Json_value* value = limits.find(name);
    if (value == nullptr) {
        if (required) {
            ok = false;
        }
        return std::nullopt;
    }
    if (!value->is_integer() || value->as_integer() < 1) {
        ok = false;
        return std::nullopt;
    }
    return (std::uint64_t)value->as_integer();
}

std::optional<Host_message> decode_host_hello(const Json_value& object, Contract_failure* out_failure)
{
    if (!check_members(object, "hello", {
            "type", "core_protocol_generations", "package_id", "generation_digest",
            "generation_nonce", "limits", "host"}, out_failure))
    {
        return std::nullopt;
    }

    Host_hello hello;
    const Json_value* generations = strict::member_of_type(object, "core_protocol_generations", Json_type::ARRAY);
    if (generations == nullptr || generations->as_array().empty() ||
        generations->as_array().size() > limits::k_max_core_generations_per_package)
    {
        return fail<Host_message>(out_failure, reason::k_frame_violation, "hello.core_protocol_generations must list 1-8 versions");
    }
    for (const Json_value& generation : generations->as_array()) {
        if (!generation.is_integer() || !is_valid_version(generation.as_integer())) {
            return fail<Host_message>(out_failure, reason::k_frame_violation, "hello.core_protocol_generations contains a non-version");
        }
        const auto version = (std::uint16_t)generation.as_integer();
        if (std::find(hello.core_protocol_generations.begin(), hello.core_protocol_generations.end(), version) !=
            hello.core_protocol_generations.end())
        {
            return fail<Host_message>(out_failure, reason::k_frame_violation, "hello.core_protocol_generations lists a version twice");
        }
        hello.core_protocol_generations.push_back(version);
    }
    if (!read_identity(object, "hello", hello.package_id, hello.generation_digest, hello.generation_nonce, out_failure)) {
        return std::nullopt;
    }

    const Json_value* limits_object = strict::member_of_type(object, "limits", Json_type::OBJECT);
    if (limits_object == nullptr) {
        return fail<Host_message>(out_failure, reason::k_frame_violation, "hello.limits is missing or not an object");
    }
    if (!check_members(*limits_object, "hello.limits", {
            "max_frame_bytes", "max_json_depth", "max_in_flight_requests", "max_progress_frames_per_request"}, out_failure))
    {
        return std::nullopt;
    }
    bool limits_ok = true;
    hello.limits.max_frame_bytes                 = read_limit(*limits_object, "max_frame_bytes",                 true, limits_ok).value_or(0);
    hello.limits.max_json_depth                  = read_limit(*limits_object, "max_json_depth",                  true, limits_ok).value_or(0);
    hello.limits.max_in_flight_requests          = read_limit(*limits_object, "max_in_flight_requests",          true, limits_ok).value_or(0);
    hello.limits.max_progress_frames_per_request = read_limit(*limits_object, "max_progress_frames_per_request", true, limits_ok).value_or(0);
    if (!limits_ok) {
        return fail<Host_message>(out_failure, reason::k_frame_violation, "hello.limits members must be positive integers");
    }

    if (!read_informational_pair(object, "host", "product", "version", hello.host_product, hello.host_version, out_failure)) {
        return std::nullopt;
    }
    return Host_message(std::move(hello));
}

std::optional<Host_message> decode_invoke(Json_value& object, Contract_failure* out_failure)
{
    if (!check_members(object, "invoke", {
            "type", "request_id", "capability_id", "version", "operation", "payload", "deadline_ms"}, out_failure))
    {
        return std::nullopt;
    }
    Invoke_request invoke;
    if (!read_request_id(object, "invoke", invoke.request_id, out_failure)) {
        return std::nullopt;
    }
    const std::string* capability_id = strict::string_member(object, "capability_id");
    if (capability_id == nullptr || !is_valid_capability_id(*capability_id)) {
        return fail<Host_message>(out_failure, reason::k_frame_violation, "invoke.capability_id is missing or out of grammar");
    }
    invoke.capability.capability_id = *capability_id;
    const std::optional<std::int64_t> version = strict::integer_member(object, "version", 0, limits::k_max_version);
    if (!version.has_value()) {
        return fail<Host_message>(out_failure, reason::k_frame_violation, "invoke.version is missing or not a version");
    }
    invoke.capability.version = (std::uint16_t)*version;
    const std::string* operation = strict::string_member(object, "operation");
    if (operation == nullptr || !is_valid_operation_name(*operation)) {
        return fail<Host_message>(out_failure, reason::k_frame_violation, "invoke.operation is missing or out of grammar");
    }
    invoke.operation = *operation;
    Json_value* payload = object.find("payload");
    if (payload == nullptr || !payload->is_object()) {
        return fail<Host_message>(out_failure, reason::k_frame_violation, "invoke.payload is missing or not an object");
    }
    const std::optional<std::int64_t> deadline = strict::integer_member(object, "deadline_ms", 1, k_max_duration_ms);
    if (!deadline.has_value()) {
        return fail<Host_message>(out_failure, reason::k_frame_violation, "invoke.deadline_ms is missing or not a positive integer");
    }
    invoke.deadline_ms = *deadline;
    invoke.payload = std::move(*payload);
    return Host_message(std::move(invoke));
}

std::optional<Host_message> decode_cancel(const Json_value& object, Contract_failure* out_failure)
{
    if (!check_members(object, "cancel", {"type", "request_id"}, out_failure)) {
        return std::nullopt;
    }
    Cancel_request cancel;
    if (!read_request_id(object, "cancel", cancel.request_id, out_failure)) {
        return std::nullopt;
    }
    return Host_message(std::move(cancel));
}

std::optional<Host_message> decode_shutdown(const Json_value& object, Contract_failure* out_failure)
{
    if (!check_members(object, "shutdown", {"type", "grace_ms"}, out_failure)) {
        return std::nullopt;
    }
    const std::optional<std::int64_t> grace = strict::integer_member(object, "grace_ms", 0, k_max_duration_ms);
    if (!grace.has_value()) {
        return fail<Host_message>(out_failure, reason::k_frame_violation, "shutdown.grace_ms is missing or not a non-negative integer");
    }
    return Host_message(Shutdown_request{*grace});
}

std::optional<Adapter_message> decode_adapter_hello(const Json_value& object, Contract_failure* out_failure)
{
    if (!check_members(object, "hello", {
            "type", "core_protocol_generation", "package_id", "generation_digest",
            "generation_nonce", "limits", "adapter"}, out_failure))
    {
        return std::nullopt;
    }
    Adapter_hello hello;
    const std::optional<std::int64_t> generation = strict::integer_member(object, "core_protocol_generation", 0, limits::k_max_version);
    if (!generation.has_value()) {
        return fail<Adapter_message>(out_failure, reason::k_frame_violation, "hello.core_protocol_generation is missing or not a version");
    }
    hello.core_protocol_generation = (std::uint16_t)*generation;
    if (!read_identity(object, "hello", hello.package_id, hello.generation_digest, hello.generation_nonce, out_failure)) {
        return std::nullopt;
    }
    if (const Json_value* limits_object = object.find("limits"); limits_object != nullptr) {
        if (!limits_object->is_object()) {
            return fail<Adapter_message>(out_failure, reason::k_frame_violation, "hello.limits is not an object");
        }
        if (!check_members(*limits_object, "hello.limits", {
                "max_frame_bytes", "max_json_depth", "max_in_flight_requests", "max_progress_frames_per_request"}, out_failure))
        {
            return std::nullopt;
        }
        bool limits_ok = true;
        hello.limits.max_frame_bytes                 = read_limit(*limits_object, "max_frame_bytes",                 false, limits_ok);
        hello.limits.max_json_depth                  = read_limit(*limits_object, "max_json_depth",                  false, limits_ok);
        hello.limits.max_in_flight_requests          = read_limit(*limits_object, "max_in_flight_requests",          false, limits_ok);
        hello.limits.max_progress_frames_per_request = read_limit(*limits_object, "max_progress_frames_per_request", false, limits_ok);
        if (!limits_ok) {
            return fail<Adapter_message>(out_failure, reason::k_frame_violation, "hello.limits members must be positive integers");
        }
    }
    if (!read_informational_pair(object, "adapter", "name", "version", hello.adapter_name, hello.adapter_version, out_failure)) {
        return std::nullopt;
    }
    return Adapter_message(std::move(hello));
}

std::optional<Adapter_message> decode_capabilities(const Json_value& object, Contract_failure* out_failure)
{
    if (!check_members(object, "capabilities", {"type", "capabilities"}, out_failure)) {
        return std::nullopt;
    }
    const Json_value* array = strict::member_of_type(object, "capabilities", Json_type::ARRAY);
    if (array == nullptr) {
        return fail<Adapter_message>(out_failure, reason::k_frame_violation, "capabilities.capabilities is missing or not an array");
    }
    if (array->as_array().size() > limits::k_max_handshake_capabilities) {
        return fail<Adapter_message>(out_failure, reason::k_frame_violation, "capabilities exceeds k_max_handshake_capabilities");
    }
    Capabilities_message capabilities;
    for (const Json_value& entry : array->as_array()) {
        if (!entry.is_object()) {
            return fail<Adapter_message>(out_failure, reason::k_frame_violation, "capabilities entry is not an object");
        }
        if (!check_members(entry, "capabilities[]", {"capability_id", "version"}, out_failure)) {
            return std::nullopt;
        }
        const std::string* capability_id = strict::string_member(entry, "capability_id");
        const std::optional<std::int64_t> version = strict::integer_member(entry, "version", 0, limits::k_max_version);
        if (capability_id == nullptr || !is_valid_capability_id(*capability_id) || !version.has_value()) {
            return fail<Adapter_message>(out_failure, reason::k_frame_violation, "capabilities entry is out of grammar");
        }
        const Capability_pair pair{*capability_id, (std::uint16_t)*version};
        if (std::find(capabilities.capabilities.begin(), capabilities.capabilities.end(), pair) != capabilities.capabilities.end()) {
            return fail<Adapter_message>(out_failure, reason::k_frame_violation,
                "capabilities lists " + pair.capability_id + "@" + std::to_string(pair.version) + " twice");
        }
        capabilities.capabilities.push_back(pair);
    }
    return Adapter_message(std::move(capabilities));
}

std::optional<Adapter_message> decode_progress(const Json_value& object, Contract_failure* out_failure)
{
    if (!check_members(object, "progress", {"type", "request_id", "completed", "total", "note"}, out_failure)) {
        return std::nullopt;
    }
    Progress_message progress;
    if (!read_request_id(object, "progress", progress.request_id, out_failure)) {
        return std::nullopt;
    }
    const std::optional<std::int64_t> completed = strict::integer_member(object, "completed", 0, std::numeric_limits<std::int64_t>::max());
    if (!completed.has_value()) {
        return fail<Adapter_message>(out_failure, reason::k_frame_violation, "progress.completed is missing or not a non-negative integer");
    }
    progress.completed = (std::uint64_t)*completed;
    if (const Json_value* total = object.find("total"); total != nullptr) {
        if (!total->is_integer() || total->as_integer() < *completed) {
            return fail<Adapter_message>(out_failure, reason::k_frame_violation, "progress.total must be an integer not below completed");
        }
        progress.total = (std::uint64_t)total->as_integer();
    }
    if (const Json_value* note = object.find("note"); note != nullptr) {
        if (!note->is_string() || note->as_string().size() > limits::k_max_progress_note_bytes) {
            return fail<Adapter_message>(out_failure, reason::k_frame_violation, "progress.note must be a string of at most 256 bytes");
        }
        progress.note = note->as_string();
    }
    return Adapter_message(std::move(progress));
}

std::optional<Adapter_message> decode_result(Json_value& object, Contract_failure* out_failure)
{
    if (!check_members(object, "result", {"type", "request_id", "status", "payload", "reason", "message"}, out_failure)) {
        return std::nullopt;
    }
    Result_message result;
    if (!read_request_id(object, "result", result.request_id, out_failure)) {
        return std::nullopt;
    }
    const std::string* status_name = strict::string_member(object, "status");
    const std::optional<Result_status> status = status_name == nullptr ? std::nullopt : parse_result_status(*status_name);
    if (!status.has_value()) {
        return fail<Adapter_message>(out_failure, reason::k_frame_violation, "result.status is missing or not in the vocabulary");
    }
    result.status = *status;

    Json_value*       payload = object.find("payload");
    const Json_value* reason  = object.find("reason");
    const Json_value* message = object.find("message");
    if (result.status == Result_status::SUCCESS) {
        if (payload == nullptr || !payload->is_object()) {
            return fail<Adapter_message>(out_failure, reason::k_frame_violation, "result.payload is required as an object for status success");
        }
        if (reason != nullptr || message != nullptr) {
            return fail<Adapter_message>(out_failure, reason::k_unexpected_member, "result.reason and result.message are not allowed with status success");
        }
        result.payload = std::move(*payload);
        return Adapter_message(std::move(result));
    }

    if (payload != nullptr) {
        return fail<Adapter_message>(out_failure, reason::k_unexpected_member, "result.payload is only allowed with status success");
    }
    if (reason == nullptr || !reason->is_string() || !is_valid_reason_code(reason->as_string())) {
        return fail<Adapter_message>(out_failure, reason::k_frame_violation, "result.reason is required as a reason code");
    }
    result.reason = reason->as_string();
    if (message != nullptr) {
        if (!message->is_string() || message->as_string().size() > limits::k_max_result_message_bytes) {
            return fail<Adapter_message>(out_failure, reason::k_frame_violation, "result.message must be a string of at most 1024 bytes");
        }
        result.message = message->as_string();
    }
    return Adapter_message(std::move(result));
}

} // namespace

const char* result_status_name(Result_status status)
{
    switch (status) {
        case Result_status::SUCCESS:     return "success";
        case Result_status::UNAVAILABLE: return "unavailable";
        case Result_status::CANCELLED:   return "cancelled";
        case Result_status::FAILED:
        default:                         return "error";
    }
}

std::optional<Result_status> parse_result_status(std::string_view name)
{
    if (name == "success")     { return Result_status::SUCCESS; }
    if (name == "unavailable") { return Result_status::UNAVAILABLE; }
    if (name == "cancelled")   { return Result_status::CANCELLED; }
    if (name == "error")       { return Result_status::FAILED; }
    return std::nullopt;
}

const char* host_message_type(const Host_message& message)
{
    switch (message.index()) {
        case 0:  return "hello";
        case 1:  return "invoke";
        case 2:  return "cancel";
        case 3:  return "shutdown";
        default: return "";
    }
}

const char* adapter_message_type(const Adapter_message& message)
{
    switch (message.index()) {
        case 0:  return "hello";
        case 1:  return "capabilities";
        case 2:  return "progress";
        case 3:  return "result";
        default: return "";
    }
}

Json_value encode_host_message(const Host_message& message)
{
    Json_value object = Json_value::object();
    object.set("type", Json_value(host_message_type(message)));

    if (const Host_hello* hello = std::get_if<Host_hello>(&message)) {
        Json_value generations = Json_value::array();
        for (const std::uint16_t generation : hello->core_protocol_generations) {
            generations.push_back(Json_value((std::int64_t)generation));
        }
        Json_value host = Json_value::object();
        host.set("product", Json_value(hello->host_product));
        host.set("version", Json_value(hello->host_version));

        object.set("core_protocol_generations", std::move(generations));
        object.set("package_id",                Json_value(hello->package_id));
        object.set("generation_digest",         Json_value(hello->generation_digest));
        object.set("generation_nonce",          Json_value(hello->generation_nonce));
        object.set("limits",                    encode_limits(hello->limits));
        object.set("host",                      std::move(host));
    }
    else
    if (const Invoke_request* invoke = std::get_if<Invoke_request>(&message)) {
        object.set("request_id",    Json_value(invoke->request_id));
        object.set("capability_id", Json_value(invoke->capability.capability_id));
        object.set("version",       Json_value((std::int64_t)invoke->capability.version));
        object.set("operation",     Json_value(invoke->operation));
        object.set("payload",       invoke->payload);
        object.set("deadline_ms",   Json_value(invoke->deadline_ms));
    }
    else
    if (const Cancel_request* cancel = std::get_if<Cancel_request>(&message)) {
        object.set("request_id", Json_value(cancel->request_id));
    }
    else
    if (const Shutdown_request* shutdown = std::get_if<Shutdown_request>(&message)) {
        object.set("grace_ms", Json_value(shutdown->grace_ms));
    }
    return object;
}

Json_value encode_adapter_message(const Adapter_message& message)
{
    Json_value object = Json_value::object();
    object.set("type", Json_value(adapter_message_type(message)));

    if (const Adapter_hello* hello = std::get_if<Adapter_hello>(&message)) {
        Json_value adapter = Json_value::object();
        adapter.set("name",    Json_value(hello->adapter_name));
        adapter.set("version", Json_value(hello->adapter_version));

        object.set("core_protocol_generation", Json_value((std::int64_t)hello->core_protocol_generation));
        object.set("package_id",               Json_value(hello->package_id));
        object.set("generation_digest",        Json_value(hello->generation_digest));
        object.set("generation_nonce",         Json_value(hello->generation_nonce));
        const Json_value limits = encode_adapter_limits(hello->limits);
        if (!limits.as_object().empty()) {
            object.set("limits", limits);
        }
        object.set("adapter", std::move(adapter));
    }
    else
    if (const Capabilities_message* capabilities = std::get_if<Capabilities_message>(&message)) {
        object.set("capabilities", encode_capability_array(capabilities->capabilities));
    }
    else
    if (const Progress_message* progress = std::get_if<Progress_message>(&message)) {
        object.set("request_id", Json_value(progress->request_id));
        object.set("completed",  Json_value((std::int64_t)progress->completed));
        if (progress->total.has_value()) {
            object.set("total", Json_value((std::int64_t)*progress->total));
        }
        if (!progress->note.empty()) {
            object.set("note", Json_value(progress->note));
        }
    }
    else
    if (const Result_message* result = std::get_if<Result_message>(&message)) {
        object.set("request_id", Json_value(result->request_id));
        object.set("status",     Json_value(result_status_name(result->status)));
        if (result->status == Result_status::SUCCESS) {
            object.set("payload", result->payload);
        }
        else {
            object.set("reason", Json_value(result->reason));
            if (!result->message.empty()) {
                object.set("message", Json_value(result->message));
            }
        }
    }
    return object;
}

std::optional<Host_message> decode_host_message(
    std::string_view         frame,
    const Json_parse_limits& parse_limits,
    Contract_failure*        out_failure)
{
    std::string type;
    std::optional<Json_value> object = decode_envelope(frame, parse_limits, type, out_failure);
    if (!object.has_value()) {
        return std::nullopt;
    }
    if (type == "hello")    { return decode_host_hello(*object, out_failure); }
    if (type == "invoke")   { return decode_invoke(*object, out_failure); }
    if (type == "cancel")   { return decode_cancel(*object, out_failure); }
    if (type == "shutdown") { return decode_shutdown(*object, out_failure); }
    return fail<Host_message>(out_failure, reason::k_unknown_message_type, type);
}

std::optional<Adapter_message> decode_adapter_message(
    std::string_view         frame,
    const Json_parse_limits& parse_limits,
    Contract_failure*        out_failure)
{
    std::string type;
    std::optional<Json_value> object = decode_envelope(frame, parse_limits, type, out_failure);
    if (!object.has_value()) {
        return std::nullopt;
    }
    if (type == "hello")        { return decode_adapter_hello(*object, out_failure); }
    if (type == "capabilities") { return decode_capabilities(*object, out_failure); }
    if (type == "progress")     { return decode_progress(*object, out_failure); }
    if (type == "result")       { return decode_result(*object, out_failure); }
    return fail<Adapter_message>(out_failure, reason::k_unknown_message_type, type);
}

// ---------------------------------------------------------------------------
// Session_tracker

Session_tracker::Session_tracker(Host_hello sent_hello)
:
    m_host_hello(std::move(sent_hello))
{}

bool Session_tracker::accept(const Adapter_message& message, Contract_failure* out_violation)
{
    if (const Adapter_hello* hello = std::get_if<Adapter_hello>(&message)) {
        return accept_hello(*hello, out_violation);
    }
    if (const Capabilities_message* capabilities = std::get_if<Capabilities_message>(&message)) {
        return accept_capabilities(*capabilities, out_violation);
    }
    if (m_phase != Phase::READY) {
        return report(out_violation, reason::k_out_of_order_message,
            std::string(adapter_message_type(message)) + " before the handshake completed");
    }
    if (const Progress_message* progress = std::get_if<Progress_message>(&message)) {
        return accept_progress(*progress, out_violation);
    }
    if (const Result_message* result = std::get_if<Result_message>(&message)) {
        return accept_result(*result, out_violation);
    }
    return report(out_violation, reason::k_frame_violation, "unhandled adapter message");
}

bool Session_tracker::accept_hello(const Adapter_hello& hello, Contract_failure* out_violation)
{
    if (m_phase != Phase::AWAITING_HELLO) {
        return report(out_violation, reason::k_out_of_order_message, "second hello");
    }
    const bool generation_offered =
        std::find(m_host_hello.core_protocol_generations.begin(), m_host_hello.core_protocol_generations.end(),
                  hello.core_protocol_generation) != m_host_hello.core_protocol_generations.end();
    if (!generation_offered) {
        return report(out_violation, reason::k_identity_mismatch,
            "core_protocol_generation " + std::to_string(hello.core_protocol_generation) + " was not offered");
    }
    if (hello.package_id != m_host_hello.package_id) {
        return report(out_violation, reason::k_identity_mismatch, "package_id differs from the host's");
    }
    if (hello.generation_digest != m_host_hello.generation_digest) {
        return report(out_violation, reason::k_identity_mismatch, "generation_digest differs from the host's");
    }
    if (hello.generation_nonce != m_host_hello.generation_nonce) {
        return report(out_violation, reason::k_identity_mismatch, "generation_nonce differs from the host's");
    }

    const Protocol_limits& host = m_host_hello.limits;
    const Adapter_limits& adapter = hello.limits;
    if ((adapter.max_frame_bytes && *adapter.max_frame_bytes > host.max_frame_bytes) ||
        (adapter.max_json_depth && *adapter.max_json_depth > host.max_json_depth) ||
        (adapter.max_in_flight_requests && *adapter.max_in_flight_requests > host.max_in_flight_requests) ||
        (adapter.max_progress_frames_per_request &&
         *adapter.max_progress_frames_per_request > host.max_progress_frames_per_request))
    {
        return report(out_violation, reason::k_frame_violation, "hello.limits exceed the host's limits");
    }

    m_adapter_hello = hello;
    m_phase         = Phase::AWAITING_CAPABILITIES;
    return true;
}

bool Session_tracker::accept_capabilities(const Capabilities_message& capabilities, Contract_failure* out_violation)
{
    if (m_phase != Phase::AWAITING_CAPABILITIES) {
        return report(out_violation, reason::k_out_of_order_message,
            m_phase == Phase::AWAITING_HELLO ? "capabilities before hello" : "second capabilities");
    }
    m_capabilities = capabilities.capabilities;
    m_phase        = Phase::READY;
    return true;
}

bool Session_tracker::accept_progress(const Progress_message& progress, Contract_failure* out_violation)
{
    const auto live = m_live.find(progress.request_id);
    if (live == m_live.end()) {
        return report(out_violation, reason::k_impossible_correlation, "progress for " + progress.request_id);
    }
    const auto bound = effective_limits().max_progress_frames_per_request;
    if (bound != limits::k_unlimited_progress_frames) {
        if (live->second >= bound) {
            return report(out_violation, reason::k_progress_limit_exceeded, progress.request_id);
        }
        ++live->second;
    }
    return true;
}

bool Session_tracker::accept_result(const Result_message& result, Contract_failure* out_violation)
{
    const auto live = m_live.find(result.request_id);
    if (live == m_live.end()) {
        if (is_finished(result.request_id)) {
            return report(out_violation, reason::k_duplicate_terminal_result, result.request_id);
        }
        return report(out_violation, reason::k_impossible_correlation, "result for " + result.request_id);
    }
    m_live.erase(live);
    return true;
}

void Session_tracker::note_invoke_sent(const std::string& request_id)
{
    m_live.emplace(request_id, 0);
    if (m_highest_issued_request_id.empty() ||
        !request_id_not_after(request_id, m_highest_issued_request_id))
    {
        m_highest_issued_request_id = request_id;
    }
}

bool Session_tracker::handshake_declares(const Capability_pair& pair) const
{
    return std::find(m_capabilities.begin(), m_capabilities.end(), pair) != m_capabilities.end();
}

Protocol_limits Session_tracker::effective_limits() const
{
    Protocol_limits effective = m_host_hello.limits;
    if (!m_adapter_hello.has_value()) {
        return effective;
    }
    const Adapter_limits& adapter = m_adapter_hello->limits;
    if (adapter.max_frame_bytes)                 { effective.max_frame_bytes                 = std::min(effective.max_frame_bytes,                 *adapter.max_frame_bytes); }
    if (adapter.max_json_depth)                  { effective.max_json_depth                  = std::min(effective.max_json_depth,                  *adapter.max_json_depth); }
    if (adapter.max_in_flight_requests)          { effective.max_in_flight_requests          = std::min(effective.max_in_flight_requests,          *adapter.max_in_flight_requests); }
    if (adapter.max_progress_frames_per_request) { effective.max_progress_frames_per_request = std::min(effective.max_progress_frames_per_request, *adapter.max_progress_frames_per_request); }
    return effective;
}

bool Session_tracker::is_live(std::string_view request_id) const
{
    return m_live.find(std::string(request_id)) != m_live.end();
}

bool Session_tracker::is_finished(std::string_view request_id) const
{
    return
        is_valid_request_id(request_id) &&
        !m_highest_issued_request_id.empty() &&
        !is_live(request_id) &&
        request_id_not_after(request_id, m_highest_issued_request_id);
}

} // namespace logonomic::cli_addon
