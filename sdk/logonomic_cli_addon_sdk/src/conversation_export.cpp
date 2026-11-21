#include "logonomic_cli_addon/conversation_export.h"

#include "logonomic_cli_addon/adapter_runtime.h"
#include "logonomic_cli_addon/limits.h"
#include "logonomic_cli_addon/package_source.h"
#include "logonomic_cli_addon/session_contracts.h"
#include "strict_json.h"

#include <algorithm>
#include <array>
#include <limits>
#include <utility>

namespace logonomic::cli_addon {
namespace {

constexpr strict::Name_entry<Conversation_export_format> k_formats[] = {
    {Conversation_export_format::TEXT,         "text"},
    {Conversation_export_format::MARKDOWN,     "markdown"},
    {Conversation_export_format::CONVERSATION, "conversation"},
};

bool fail(Contract_failure* out_failure, const char* reason, std::string detail)
{
    if (out_failure) {
        *out_failure = {reason, std::move(detail)};
    }
    return false;
}

void encode_context(Json_value& payload, const std::optional<Profile_storage_context>& profile,
    const std::optional<Account_context>& account)
{
    if (profile) {
        payload.set(k_profile_storage_member, profile_storage_member(*profile));
    }
    if (account) {
        payload.set(k_account_context_member, encode_account_context(*account));
    }
}

bool decode_context(const Json_value& payload, std::optional<Profile_storage_context>& profile,
    std::optional<Account_context>& account, Contract_failure* out_failure)
{
    if (const auto* value = payload.find(k_profile_storage_member)) {
        std::string detail;
        profile = decode_profile_storage_context(*value, &detail);
        if (!profile) {
            return fail(out_failure, "conversation_export_invalid", std::move(detail));
        }
    }
    if (const auto* value = payload.find(k_account_context_member)) {
        account = decode_account_context(*value, out_failure);
        if (!account) {
            return false;
        }
    }
    return true;
}

std::string markdown_label(std::string_view text)
{
    std::string result;
    for (char value : text) {
        if (std::string_view("\\`*_{}[]<>()#+-.!|").find(value) != std::string_view::npos) {
            result.push_back('\\');
        }
        result.push_back(value);
    }
    return result;
}

std::string_view role_label(std::string_view role)
{
    if (role == "user")      { return "User"; }
    if (role == "assistant") { return "Assistant"; }
    if (role == "system")    { return "System"; }
    if (role == "developer") { return "Developer"; }
    if (role == "tool")      { return "Tool"; }
    return role;
}

const char* block_label(Conversation_block_kind kind)
{
    switch (kind) {
        case Conversation_block_kind::TOOL_CALL:   return "Tool call";
        case Conversation_block_kind::TOOL_RESULT: return "Tool result";
        case Conversation_block_kind::ATTACHMENT:  return "Attachment";
        default:                                  return "Text";
    }
}

const char* conversation_block_name(const Conversation_block& block)
{
    switch (block.kind) {
        case Conversation_block_kind::TOOL_CALL:   return "tool-call";
        case Conversation_block_kind::TOOL_RESULT: return "tool-result";
        case Conversation_block_kind::ATTACHMENT:  return "attachment";
        default: return block.label == "Thinking" || block.label == "Reasoning" ? "reasoning" : "text";
    }
}

} // namespace

const char* conversation_export_format_name(Conversation_export_format format)
{
    return strict::name_of(k_formats, format);
}

std::optional<Conversation_export_format> parse_conversation_export_format(std::string_view name)
{
    return strict::value_of(k_formats, name);
}

const char* conversation_export_file_name(Conversation_export_format format)
{
    switch (format) {
        case Conversation_export_format::MARKDOWN:     return "conversation.md";
        case Conversation_export_format::CONVERSATION: return "conversation.vnm_chat";
        default:                                      return "conversation.txt";
    }
}

std::optional<Launcher_discovery_declaration> decode_conversation_export_declaration(
    const Json_value& payload, Contract_failure* out_failure)
{
    return decode_launcher_discovery_declaration(payload, out_failure);
}

Json_value encode_conversation_export_request(const Conversation_export_request& request)
{
    auto payload = Json_value::object();
    payload.set("provider_session_id", Json_value(request.provider_session_id));
    if (request.resume_record) {
        payload.set("resume_record", *request.resume_record);
    }
    payload.set("working_directory", Json_value(request.working_directory));
    payload.set("launch_environment", encode_launch_environment(request.launch_environment));
    payload.set("output_directory", Json_value(request.output_directory));
    payload.set("format", Json_value(conversation_export_format_name(request.format)));
    if (request.compact) {
        payload.set("compact", Json_value(true));
    }
    encode_context(payload, request.profile_storage, request.account_context);
    return payload;
}

std::optional<Conversation_export_request> decode_conversation_export_request(
    const Json_value& payload, Contract_failure* out_failure)
{
    const auto* identity = strict::string_member(payload, "provider_session_id");
    const auto* working = strict::string_member(payload, "working_directory");
    const auto* output = strict::string_member(payload, "output_directory");
    const auto* format = strict::string_member(payload, "format");
    const auto* environment = payload.find("launch_environment");
    const auto* compact = payload.find("compact");
    if (!payload.is_object() || strict::unexpected_member(payload,
            {"provider_session_id", "resume_record", "working_directory", "launch_environment",
                "output_directory", "format", "profile_storage", "account_context", "compact"}) ||
        !identity || identity->empty() || identity->size() > limits::k_max_json_string_bytes ||
        !strict::has_no_control_characters(*identity) || !is_valid_utf8(*identity) ||
        !working || working->size() > limits::k_max_working_directory_bytes ||
        working->find('\0') != std::string::npos || !is_valid_utf8(*working) ||
        !output || !strict::is_bounded_absolute_path(*output) || !format || !environment ||
        (compact && !compact->is_boolean()))
    {
        fail(out_failure, "conversation_export_invalid", "conversation export request has invalid members");
        return std::nullopt;
    }
    const auto parsed_format = parse_conversation_export_format(*format);
    auto parsed_environment = decode_launch_environment(*environment, out_failure);
    if (!parsed_format || !parsed_environment) {
        fail(out_failure, "conversation_export_invalid", "conversation export format or environment is invalid");
        return std::nullopt;
    }
    Conversation_export_request result;
    result.provider_session_id = *identity;
    result.working_directory = *working;
    result.output_directory = *output;
    result.format = *parsed_format;
    result.compact = compact && compact->as_boolean();
    result.launch_environment = std::move(*parsed_environment);
    if (const auto* record = payload.find("resume_record")) {
        result.resume_record = decode_resume_record(encode_resume_record(*record), out_failure);
        if (!result.resume_record) {
            return std::nullopt;
        }
    }
    if (!decode_context(payload, result.profile_storage, result.account_context, out_failure)) {
        return std::nullopt;
    }
    return result;
}

Json_value encode_conversation_export_result(const Conversation_export_result& result)
{
    auto payload = Json_value::object();
    payload.set("format", Json_value(conversation_export_format_name(result.format)));
    payload.set("file_name", Json_value(result.file_name));
    payload.set("bytes", Json_value(result.bytes));
    payload.set("message_count", Json_value(result.message_count));
    payload.set("attachment_count", Json_value(result.attachment_count));
    payload.set("incomplete_tail_omitted", Json_value(result.incomplete_tail_omitted));
    if (result.compact) {
        payload.set("compact", Json_value(true));
    }
    encode_context(payload, result.profile_storage, result.account_context);
    return payload;
}

std::optional<Conversation_export_result> decode_conversation_export_result(
    const Json_value& payload, Contract_failure* out_failure)
{
    const auto* format = strict::string_member(payload, "format");
    const auto* file_name = strict::string_member(payload, "file_name");
    const auto bytes = strict::integer_member(payload, "bytes", 1, std::numeric_limits<std::int64_t>::max());
    const auto messages = strict::integer_member(payload, "message_count", 1, std::numeric_limits<std::int64_t>::max());
    const auto attachments = strict::integer_member(payload, "attachment_count", 0,
        std::numeric_limits<std::int64_t>::max());
    const auto* tail = strict::member_of_type(payload, "incomplete_tail_omitted", Json_type::BOOLEAN);
    const auto* compact = payload.find("compact");
    const auto parsed_format = format ? parse_conversation_export_format(*format) : std::nullopt;
    if (!payload.is_object() || strict::unexpected_member(payload,
            {"format", "file_name", "bytes", "message_count", "attachment_count",
                "incomplete_tail_omitted", "profile_storage", "account_context", "compact"}) ||
        !parsed_format || !file_name || *file_name != conversation_export_file_name(*parsed_format) ||
        !bytes || !messages || !attachments || !tail || (compact && !compact->is_boolean()))
    {
        fail(out_failure, "conversation_export_invalid", "conversation export result has invalid members");
        return std::nullopt;
    }
    Conversation_export_result result;
    result.format = *parsed_format;
    result.file_name = *file_name;
    result.bytes = static_cast<std::uint64_t>(*bytes);
    result.message_count = static_cast<std::uint64_t>(*messages);
    result.attachment_count = static_cast<std::uint64_t>(*attachments);
    result.incomplete_tail_omitted = tail->as_boolean();
    result.compact = compact && compact->as_boolean();
    if (!decode_context(payload, result.profile_storage, result.account_context, out_failure)) {
        return std::nullopt;
    }
    return result;
}

void handle_conversation_export(Invocation_context& context,
    const std::function<std::optional<Conversation_export_result>(
        const Conversation_export_request&, const std::function<bool()>&, Contract_failure*)>& exporter)
{
    if (context.request().operation != k_conversation_export_operation) {
        context.reject_unknown_operation();
        return;
    }
    Contract_failure failure;
    const auto request = decode_conversation_export_request(context.request().payload, &failure);
    if (!request) {
        context.fail(Result_status::FAILED, failure.reason_code, failure.detail);
        return;
    }
    const std::function<bool()> cancelled = [&context] { return context.cancel_requested(); };
    std::optional<Conversation_export_result> result;
    try {
        result = exporter(*request, cancelled, &failure);
    }
    catch (const std::filesystem::filesystem_error&) {
        failure = {"conversation_export_failed", "conversation files could not be accessed"};
    }
    if (cancelled() || failure.reason_code == "cancelled") {
        context.fail(Result_status::CANCELLED, "cancelled");
    }
    else
    if (result) {
        context.succeed(encode_conversation_export_result(*result));
    }
    else {
        const bool unavailable = failure.reason_code == "conversation_unavailable" ||
            failure.reason_code == "conversation_not_found" || failure.reason_code == "conversation_empty" ||
            failure.reason_code == "conversation_source_unavailable" ||
            failure.reason_code == "conversation_format_unavailable" ||
            failure.reason_code == "conversation_content_unavailable" ||
            failure.reason_code == "conversation_branch_unavailable";
        context.fail(unavailable ? Result_status::UNAVAILABLE : Result_status::FAILED,
            failure.reason_code.empty() ? "conversation_export_failed" : failure.reason_code, failure.detail);
    }
}

Conversation_writer::Conversation_writer(
    const Conversation_export_request& request, std::function<bool()> cancelled)
:
    m_request(request),
    m_cancelled(std::move(cancelled))
{
    m_result.format = request.format;
    m_result.compact = request.compact;
    m_result.file_name = conversation_export_file_name(request.format);
    m_result.profile_storage = request.profile_storage;
    m_result.account_context = request.account_context;
}

Conversation_writer::~Conversation_writer()
{
    if (m_stream.is_open()) {
        m_stream.close();
    }
    if (!m_finished && !m_partial_path.empty()) {
        std::error_code error;
        std::filesystem::remove(m_partial_path, error);
    }
}

bool Conversation_writer::check_cancelled(Contract_failure* out_failure)
{
    if (m_cancelled && m_cancelled()) {
        m_failed = true;
        return fail(out_failure, "cancelled", "conversation export was cancelled");
    }
    return true;
}

bool Conversation_writer::open(Contract_failure* out_failure)
{
    if (!check_cancelled(out_failure)) {
        return false;
    }
    if (m_stream.is_open() || m_finished || m_failed) {
        return fail(out_failure, "conversation_export_failed", "conversation output is already open or finished");
    }
    const auto directory = path_from_utf8(m_request.output_directory);
    std::error_code error;
    const auto canonical = std::filesystem::canonical(directory, error);
    if (error || canonical != directory.lexically_normal() || !std::filesystem::is_directory(canonical)) {
        m_failed = true;
        return fail(out_failure, "conversation_export_failed", "conversation output directory is not canonical");
    }
    m_output_path = directory / m_result.file_name;
    const auto partial = directory / (m_result.file_name + ".partial");
    if (std::filesystem::exists(std::filesystem::symlink_status(m_output_path, error)) ||
        std::filesystem::exists(std::filesystem::symlink_status(partial, error)))
    {
        m_failed = true;
        return fail(out_failure, "conversation_export_failed", "conversation output already exists");
    }
    m_stream.open(partial, std::ios::binary | std::ios::out | std::ios::trunc);
    if (!m_stream) {
        m_failed = true;
        return fail(out_failure, "conversation_export_failed", "conversation output could not be created");
    }
    m_partial_path = partial;
    std::filesystem::permissions(partial,
        std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
        std::filesystem::perm_options::replace, error);
    if (error) {
        m_failed = true;
        return fail(out_failure, "conversation_export_failed", "conversation output permissions could not be set");
    }
    return m_request.format != Conversation_export_format::CONVERSATION ||
        write("#!vnm-chat 1\n\n", out_failure);
}

bool Conversation_writer::write(std::string_view bytes, Contract_failure* out_failure)
{
    if (m_failed || m_finished || !m_stream.is_open()) {
        return fail(out_failure, "conversation_export_failed", "conversation output is not writable");
    }
    while (!bytes.empty()) {
        if (!check_cancelled(out_failure)) {
            return false;
        }
        const auto count = std::min<std::size_t>(bytes.size(), 65536);
        m_stream.write(bytes.data(), static_cast<std::streamsize>(count));
        if (!m_stream) {
            m_failed = true;
            return fail(out_failure, "conversation_export_failed", "conversation output could not be written");
        }
        m_result.bytes += count;
        bytes.remove_prefix(count);
    }
    return true;
}

bool Conversation_writer::write_text(std::string_view text, Contract_failure* out_failure)
{
    // The editor accepts UTF-8 text, not terminal control bytes. Keep their
    // value visible without erasing content or letting an escape act on it.
    constexpr char hex[] = "0123456789abcdef";
    std::size_t start = 0;
    for (std::size_t offset = 0; offset < text.size(); ++offset) {
        const auto byte = static_cast<unsigned char>(text[offset]);
        const bool control = byte < 0x20 && byte != '\t' && byte != '\n' && byte != '\r' && byte != '\f';
        if (control || offset - start == 65536) {
            if (!write(text.substr(start, offset - start), out_failure)) {
                return false;
            }
            start = offset;
        }
        if (control) {
            const char escape[] = {'\\', 'u', '0', '0', hex[byte >> 4], hex[byte & 15]};
            if (!write(std::string_view(escape, sizeof(escape)), out_failure)) {
                return false;
            }
            start = offset + 1;
        }
    }
    return write(text.substr(start), out_failure);
}

bool Conversation_writer::write_literal(std::string_view text, Contract_failure* out_failure)
{
    if (m_request.format == Conversation_export_format::TEXT) {
        return write_text(text, out_failure) && write("\n\n", out_failure);
    }
    std::size_t longest = 0;
    std::size_t current = 0;
    for (char value : text) {
        current = value == '`' ? current + 1 : 0;
        longest = std::max(longest, current);
    }
    const std::string fence(std::max<std::size_t>(3, longest + 1), '`');
    return write(fence, out_failure) && write("\n", out_failure) && write_text(text, out_failure) &&
        write("\n", out_failure) && write(fence, out_failure) && write("\n\n", out_failure);
}

bool Conversation_writer::write_conversation_block(const Conversation_message& message,
    const Conversation_block& block, Contract_failure* out_failure)
{
    std::size_t longest = 0;
    std::size_t current = 0;
    for (char value : block.text) {
        current = value == ':' ? current + 1 : 0;
        longest = std::max(longest, current);
    }
    const std::string fence(std::max<std::size_t>(3, longest + 1), ':');
    const bool plain_role = std::all_of(message.role.begin(), message.role.end(), [](char value) {
        return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') || value == '_' || value == '-';
    });
    std::string header = fence + " " + (plain_role ? message.role : serialize_json(Json_value(message.role)));
    if (message.phase == Conversation_message_phase::FINAL) {
        header += " final";
    }
    else
    if (message.phase == Conversation_message_phase::COMMENTARY) {
        header += " commentary";
    }
    if (block.kind != Conversation_block_kind::TEXT || !block.label.empty()) {
        header += " /";
        header += conversation_block_name(block);
        if (!block.label.empty()) {
            header += " " + serialize_json(Json_value(block.label));
        }
    }
    return write(header, out_failure) && write("\n", out_failure) && write_text(block.text, out_failure) &&
        write("\n", out_failure) && write(fence, out_failure) && write("\n\n", out_failure);
}

bool Conversation_writer::append(const Conversation_message& message, Contract_failure* out_failure)
{
    if (message.role.empty() || !is_valid_utf8(message.role) ||
        !strict::has_no_control_characters(message.role) || message.content.empty())
    {
        m_failed = true;
        return fail(out_failure, "conversation_source_invalid", "conversation message has no role or content");
    }
    for (const auto& block : message.content) {
        if (!is_valid_utf8(block.text) || !is_valid_utf8(block.label) ||
            !strict::has_no_control_characters(block.label))
        {
            m_failed = true;
            return fail(out_failure, "conversation_source_invalid", "conversation content is not valid text");
        }
    }
    if (m_request.compact && std::none_of(message.content.begin(), message.content.end(),
            [](const auto& block) { return block.compact_visible; }))
    {
        return true;
    }
    const bool conversation = m_request.format == Conversation_export_format::CONVERSATION;
    const bool markdown = m_request.format == Conversation_export_format::MARKDOWN;
    const auto label = role_label(message.role);
    if (!conversation && (!write(markdown ? "## " : "", out_failure) ||
        !write(markdown ? markdown_label(label) : std::string(label), out_failure) ||
        !write("\n\n", out_failure)))
    {
        return false;
    }
    for (const auto& block : message.content) {
        if (m_request.compact && !block.compact_visible) {
            continue;
        }
        if (conversation) {
            if (!write_conversation_block(message, block, out_failure)) {
                return false;
            }
            if (block.kind == Conversation_block_kind::ATTACHMENT) {
                ++m_result.attachment_count;
            }
            continue;
        }
        if (block.kind == Conversation_block_kind::TEXT) {
            if (!block.label.empty() && (!write(markdown ? "### " : "", out_failure) ||
                !write(markdown ? markdown_label(block.label) : block.label, out_failure) ||
                !write("\n\n", out_failure)))
            {
                return false;
            }
            if (!write_text(block.text, out_failure) || !write("\n\n", out_failure)) {
                return false;
            }
            continue;
        }
        std::string heading = block_label(block.kind);
        if (!block.label.empty()) {
            heading += ": " + block.label;
        }
        if (!write(markdown ? "### " : "", out_failure) ||
            !write(markdown ? markdown_label(heading) : heading, out_failure) ||
            !write("\n\n", out_failure) || !write_literal(block.text, out_failure))
        {
            return false;
        }
        if (block.kind == Conversation_block_kind::ATTACHMENT) {
            ++m_result.attachment_count;
        }
    }
    ++m_result.message_count;
    return true;
}

std::optional<Conversation_export_result> Conversation_writer::finish(
    bool incomplete_tail_omitted, Contract_failure* out_failure)
{
    if (!check_cancelled(out_failure)) {
        return std::nullopt;
    }
    if (m_failed || m_finished || !m_stream.is_open() || m_result.message_count == 0) {
        fail(out_failure, "conversation_export_failed", "conversation has no completed export");
        return std::nullopt;
    }
    m_stream.close();
    if (m_stream.fail()) {
        m_failed = true;
        fail(out_failure, "conversation_export_failed", "conversation output could not be closed");
        return std::nullopt;
    }
    std::error_code error;
    std::filesystem::rename(m_partial_path, m_output_path, error);
    if (error) {
        m_failed = true;
        fail(out_failure, "conversation_export_failed", "conversation output could not be finalized");
        return std::nullopt;
    }
    m_finished = true;
    m_result.incomplete_tail_omitted = incomplete_tail_omitted;
    return m_result;
}

std::optional<Conversation_snapshot> read_conversation_jsonl(
    const std::filesystem::path& path,
    const std::function<bool(const Json_value&, Contract_failure*)>& consume,
    const std::function<bool()>& cancelled,
    Contract_failure* out_failure)
{
    const auto stopped = [&] {
        if (cancelled && cancelled()) {
            fail(out_failure, "cancelled", "conversation export was cancelled");
            return true;
        }
        return false;
    };
    if (stopped()) {
        return std::nullopt;
    }
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    const auto end = stream.tellg();
    if (!stream || end < 0) {
        fail(out_failure, "conversation_unavailable", "conversation source could not be opened");
        return std::nullopt;
    }
    Conversation_snapshot snapshot;
    snapshot.bytes = static_cast<std::uint64_t>(end);
    stream.seekg(0);
    std::array<char, 65536> buffer;
    std::string line;
    std::uint64_t read = 0;
    std::uint64_t line_number = 0;
    while (read < snapshot.bytes) {
        if (stopped()) {
            return std::nullopt;
        }
        const auto count = std::min<std::uint64_t>(snapshot.bytes - read, buffer.size());
        stream.read(buffer.data(), static_cast<std::streamsize>(count));
        if (stream.gcount() != static_cast<std::streamsize>(count)) {
            fail(out_failure, "conversation_source_changed", "conversation source changed while being read");
            return std::nullopt;
        }
        std::size_t start = 0;
        for (std::size_t offset = 0; offset < count; ++offset) {
            if (buffer[offset] != '\n') {
                continue;
            }
            line.append(buffer.data() + start, offset - start);
            ++line_number;
            snapshot.complete_bytes = read + offset + 1;
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            if (!line.empty()) {
                if (stopped()) {
                    return std::nullopt;
                }
                Json_parse_limits parse_limits;
                parse_limits.max_bytes = line.size();
                parse_limits.max_string_bytes = line.size();
                parse_limits.max_members = line.size();
                Json_parse_error error;
                auto record = parse_json(line, parse_limits, &error);
                if (!record) {
                    fail(out_failure, "conversation_source_invalid",
                        "conversation record " + std::to_string(line_number) + " is invalid: " + error.message);
                    return std::nullopt;
                }
                if (!consume(*record, out_failure)) {
                    return std::nullopt;
                }
                ++snapshot.records;
            }
            line.clear();
            start = offset + 1;
        }
        line.append(buffer.data() + start, static_cast<std::size_t>(count) - start);
        read += count;
    }
    if (stopped()) {
        return std::nullopt;
    }
    snapshot.incomplete_tail_omitted = !line.empty();
    return snapshot;
}

} // namespace logonomic::cli_addon
