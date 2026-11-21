#pragma once

#include "logonomic_cli_addon/json.h"

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The supervisor link: the byte stream between the desktop and the
// supervisor, plus the custodian -> supervisor request file and the
// supervisor -> custodian status report. All Logonomic-internal; only the
// adapter protocol the frames carry is SDK contract. Framing is
// [type:1][length:4 big-endian][payload], with length bounded by the
// caller's max (limits::k_max_link_frame_bytes on both ends).
namespace logonomic::cli_addon_host::link {

enum class Message_type : std::uint8_t
{
    ADAPTER_FRAME    = 1,   // both directions: one newline-terminated adapter frame, newline excluded
    STDERR_CHUNK     = 2,   // supervisor -> desktop: raw adapter stderr bytes
    STDERR_TRUNCATED = 3,   // supervisor -> desktop: canonical truncation marker plus the newest retained tail
    ADAPTER_EXITED   = 4,   // supervisor -> desktop: JSON {"exit_code":..,"signal":..}
    VIOLATION        = 5,   // supervisor -> desktop: JSON {"reason":..}; protocol I/O has stopped
    IDLE_SHUTDOWN    = 6,   // shutdown frame to adapter; empty reply after its complete stdin write
};

inline constexpr std::size_t k_frame_header_bytes = 5;

inline bool is_known_message_type(std::uint8_t in_value)
{
    return in_value >= static_cast<std::uint8_t>(Message_type::ADAPTER_FRAME) &&
           in_value <= static_cast<std::uint8_t>(Message_type::IDLE_SHUTDOWN);
}

inline const char* message_type_name(Message_type in_type)
{
    switch (in_type) {
        case Message_type::ADAPTER_FRAME:    return "ADAPTER_FRAME";
        case Message_type::STDERR_CHUNK:     return "STDERR_CHUNK";
        case Message_type::STDERR_TRUNCATED: return "STDERR_TRUNCATED";
        case Message_type::ADAPTER_EXITED:   return "ADAPTER_EXITED";
        case Message_type::VIOLATION:        return "VIOLATION";
        case Message_type::IDLE_SHUTDOWN:    return "IDLE_SHUTDOWN";
        default:                             return "unknown";
    }
}

struct Frame
{
    Message_type              type = Message_type::ADAPTER_FRAME;
    std::vector<std::uint8_t> payload;
};

inline void append_frame(
    std::vector<std::uint8_t>& in_out_stream,
    Message_type               in_type,
    const std::uint8_t*        in_payload,
    std::size_t                in_size)
{
    const auto length = static_cast<std::uint32_t>(in_size);
    in_out_stream.push_back(static_cast<std::uint8_t>(in_type));
    in_out_stream.push_back(static_cast<std::uint8_t>(length >> 24));
    in_out_stream.push_back(static_cast<std::uint8_t>(length >> 16));
    in_out_stream.push_back(static_cast<std::uint8_t>(length >> 8));
    in_out_stream.push_back(static_cast<std::uint8_t>(length));
    in_out_stream.insert(in_out_stream.end(), in_payload, in_payload + in_size);
}

inline std::vector<std::uint8_t> encode_frame(Message_type in_type, std::string_view in_payload)
{
    std::vector<std::uint8_t> stream;
    stream.reserve(k_frame_header_bytes + in_payload.size());
    append_frame(
        stream,
        in_type,
        reinterpret_cast<const std::uint8_t*>(in_payload.data()),
        in_payload.size());
    return stream;
}

/// Incremental decoder over the link byte stream. A length above the bound
/// or an unknown type is a link violation: nothing after it is decoded,
/// because the stream position is no longer trustworthy.
class Frame_reader
{
public:
    enum class Feed_status
    {
        OK,
        OVERSIZE_FRAME,
        UNKNOWN_TYPE,
    };

    explicit Frame_reader(std::size_t in_max_payload_bytes)
    :
        m_max_payload_bytes(in_max_payload_bytes)
    {}

    Feed_status feed(const std::uint8_t* in_data, std::size_t in_size)
    {
        if (m_violation != Feed_status::OK) {
            return m_violation;
        }
        m_buffer.insert(m_buffer.end(), in_data, in_data + in_size);
        return validate_head();
    }

    /// The next complete frame, if any; the reader keeps the remainder.
    std::optional<Frame> next()
    {
        if (m_violation != Feed_status::OK || !head_complete()) {
            return std::nullopt;
        }
        const std::size_t length = head_length();
        Frame frame;
        frame.type = static_cast<Message_type>(m_buffer[m_consumed]);
        frame.payload.assign(
            m_buffer.begin() + static_cast<std::ptrdiff_t>(m_consumed + k_frame_header_bytes),
            m_buffer.begin() + static_cast<std::ptrdiff_t>(m_consumed + k_frame_header_bytes + length));
        m_consumed += k_frame_header_bytes + length;
        compact();
        (void)validate_head();
        return frame;
    }

    bool        violated() const   { return m_violation != Feed_status::OK; }
    Feed_status violation() const  { return m_violation; }
    std::size_t buffered() const   { return m_buffer.size() - m_consumed; }

private:
    bool head_has_header() const { return buffered() >= k_frame_header_bytes; }

    std::size_t head_length() const
    {
        const std::uint8_t* head = m_buffer.data() + m_consumed;
        return
            (static_cast<std::size_t>(head[1]) << 24) |
            (static_cast<std::size_t>(head[2]) << 16) |
            (static_cast<std::size_t>(head[3]) << 8)  |
            static_cast<std::size_t>(head[4]);
    }

    bool head_complete() const
    {
        return head_has_header() && buffered() >= k_frame_header_bytes + head_length();
    }

    Feed_status validate_head()
    {
        if (!head_has_header()) {
            return Feed_status::OK;
        }
        if (!is_known_message_type(m_buffer[m_consumed])) {
            m_violation = Feed_status::UNKNOWN_TYPE;
        }
        else
        if (head_length() > m_max_payload_bytes) {
            m_violation = Feed_status::OVERSIZE_FRAME;
        }
        return m_violation;
    }

    // Consumed bytes are dropped from the front only once they dominate the
    // buffer, so a stream of small frames does not shift memory per frame.
    void compact()
    {
        if (m_consumed > 0 && m_consumed >= m_buffer.size() / 2) {
            m_buffer.erase(m_buffer.begin(), m_buffer.begin() + static_cast<std::ptrdiff_t>(m_consumed));
            m_consumed = 0;
        }
    }

    std::size_t               m_max_payload_bytes;
    std::vector<std::uint8_t> m_buffer;
    std::size_t               m_consumed  = 0;
    Feed_status               m_violation = Feed_status::OK;
};

/// What the custodian writes for the supervisor into the scratch root: the
/// validated adapter launch. argv is complete (argv[0] included) and the
/// environment is the complete NAME=VALUE vector, reserved variables
/// included; the supervisor applies it verbatim.
struct Supervisor_request
{
    std::string              adapter_executable;
    std::vector<std::string> adapter_argv;
    std::vector<std::string> environment;
    std::string              working_directory;
    std::uint64_t            max_frame_bytes       = 0;
    std::uint64_t            stderr_ring_bytes     = 0;
    std::uint64_t            max_link_frame_bytes  = 0;
    std::uint64_t            max_stdin_queue_bytes = 0;
    std::optional<std::int64_t> darwin_attachment_version;
};

inline constexpr int k_darwin_stdin_pipe  = 5;
inline constexpr int k_darwin_stdout_pipe = 6;
inline constexpr int k_darwin_stderr_pipe = 7;
inline constexpr std::string_view k_darwin_attached = "darwin_attached 1";

inline std::string encode_supervisor_request(const Supervisor_request& in_request)
{
    using cli_addon::Json_value;
    const auto string_array = [](const std::vector<std::string>& values) {
        Json_value array = Json_value::array();
        for (const std::string& value : values) {
            array.push_back(Json_value(value));
        }
        return array;
    };
    Json_value object = Json_value::object();
    object.set("adapter_executable",    Json_value(in_request.adapter_executable));
    object.set("adapter_argv",          string_array(in_request.adapter_argv));
    object.set("environment",           string_array(in_request.environment));
    object.set("working_directory",     Json_value(in_request.working_directory));
    object.set("max_frame_bytes",       Json_value(static_cast<std::int64_t>(in_request.max_frame_bytes)));
    object.set("stderr_ring_bytes",     Json_value(static_cast<std::int64_t>(in_request.stderr_ring_bytes)));
    object.set("max_link_frame_bytes",  Json_value(static_cast<std::int64_t>(in_request.max_link_frame_bytes)));
    object.set("max_stdin_queue_bytes", Json_value(static_cast<std::int64_t>(in_request.max_stdin_queue_bytes)));
    if (in_request.darwin_attachment_version) {
        auto attachment = Json_value::object();
        attachment.set("version", Json_value(*in_request.darwin_attachment_version));
        object.set("darwin_attachment", std::move(attachment));
    }
    return cli_addon::serialize_json(object);
}

inline bool decode_supervisor_request(
    std::string_view    in_text,
    Supervisor_request* out_request,
    std::string*        out_error)
{
    using cli_addon::Json_value;
    cli_addon::Json_parse_limits parse_limits;
    parse_limits.max_bytes = cli_addon::limits::k_max_supervisor_request_bytes;
    cli_addon::Json_parse_error parse_error;
    const std::optional<Json_value> parsed = cli_addon::parse_json(in_text, parse_limits, &parse_error);
    if (!parsed || !parsed->is_object()) {
        *out_error = "supervisor request is not a JSON object: " + parse_error.message;
        return false;
    }
    const auto read_string = [&](const char* name, std::string* out) {
        const Json_value* value = parsed->find(name);
        if (!value || !value->is_string()) {
            *out_error = std::string("supervisor request member \"") + name + "\" is missing or not a string";
            return false;
        }
        *out = value->as_string();
        return true;
    };
    const auto read_strings = [&](const char* name, std::vector<std::string>* out) {
        const Json_value* value = parsed->find(name);
        if (!value || !value->is_array()) {
            *out_error = std::string("supervisor request member \"") + name + "\" is missing or not an array";
            return false;
        }
        out->clear();
        for (const Json_value& item : value->as_array()) {
            if (!item.is_string()) {
                *out_error = std::string("supervisor request member \"") + name + "\" holds a non-string";
                return false;
            }
            out->push_back(item.as_string());
        }
        return true;
    };
    const auto read_limit = [&](const char* name, std::uint64_t* out) {
        const Json_value* value = parsed->find(name);
        if (!value || !value->is_integer() || value->as_integer() < 0) {
            *out_error = std::string("supervisor request member \"") + name + "\" is missing or not a non-negative integer";
            return false;
        }
        *out = static_cast<std::uint64_t>(value->as_integer());
        return true;
    };
    if (const auto* attachment = parsed->find("darwin_attachment"); attachment && attachment->is_object()) {
        const auto* version = attachment->find("version");
        if (version && version->is_integer()) {
            out_request->darwin_attachment_version = version->as_integer();
        }
    }
    return
        read_string("adapter_executable",   &out_request->adapter_executable)     &&
        read_strings("adapter_argv",        &out_request->adapter_argv)           &&
        read_strings("environment",         &out_request->environment)            &&
        read_string("working_directory",    &out_request->working_directory)      &&
        read_limit("max_frame_bytes",       &out_request->max_frame_bytes)        &&
        read_limit("stderr_ring_bytes",     &out_request->stderr_ring_bytes)      &&
        read_limit("max_link_frame_bytes",  &out_request->max_link_frame_bytes)   &&
        read_limit("max_stdin_queue_bytes", &out_request->max_stdin_queue_bytes)  &&
        !out_request->adapter_argv.empty();
}

/// The supervisor's one report to the custodian over the status channel:
/// either the adapter's pid (on Linux the message also carries the adapter's
/// pidfd as a transferred descriptor) or why the adapter could not start.
struct Status_report
{
    bool         adapter_started = false;
    std::int64_t adapter_pid     = 0;
    // Source-process handle number, not a handle in the receiver. Windows
    // duplicates it while the exact supervisor retains it until receipt.
    std::uint64_t adapter_handle_number = 0;
    std::string  failure;   // when !adapter_started
};

inline std::string encode_status_report(const Status_report& in_report)
{
    if (in_report.adapter_started) {
        const std::string pid = "adapter_pid " + std::to_string(in_report.adapter_pid);
        return in_report.adapter_handle_number == 0 ? pid : pid + " handle " +
            std::to_string(in_report.adapter_handle_number);
    }
    return "spawn_failed " + in_report.failure;
}

inline std::string encode_status_receipt(const Status_report& in_report)
{
    return "adapter_recorded " + std::to_string(in_report.adapter_pid) + " handle " +
        std::to_string(in_report.adapter_handle_number);
}

inline bool decode_status_report(std::string_view in_text, Status_report* out_report)
{
    constexpr std::string_view k_started = "adapter_pid ";
    constexpr std::string_view k_failed  = "spawn_failed ";
    if (in_text.substr(0, k_started.size()) == k_started) {
        const std::string_view fields = in_text.substr(k_started.size());
        const auto separator = fields.find(" handle ");
        const std::string_view digits = fields.substr(0, separator);
        std::uint64_t handle = 0;
        if (separator != std::string_view::npos) {
            const auto value = fields.substr(separator + 8);
            const auto parsed = std::from_chars(value.data(), value.data() + value.size(), handle);
            if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() ||
                handle == 0 || std::to_string(handle) != value)
            {
                return false;
            }
        }
        if (digits.empty() || digits.size() > 18) {
            return false;
        }
        std::int64_t pid = 0;
        for (const char c : digits) {
            if (c < '0' || c > '9') {
                return false;
            }
            pid = pid * 10 + (c - '0');
        }
        out_report->adapter_started = true;
        out_report->adapter_pid     = pid;
        out_report->adapter_handle_number = handle;
        return pid > 0;
    }
    if (in_text.substr(0, k_failed.size()) == k_failed) {
        out_report->adapter_started = false;
        out_report->failure         = std::string(in_text.substr(k_failed.size()));
        return true;
    }
    return false;
}

} // namespace logonomic::cli_addon_host::link
