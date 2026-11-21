#include "sdk_test_support.h"

#include "logonomic_cli_addon/conversation_export.h"
#include "logonomic_cli_addon/limits.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace sdk_tests {
namespace {

namespace addon = logonomic::cli_addon;

class Temporary_directory
{
public:
    Temporary_directory()
    :
        m_path(std::filesystem::canonical(make_temp_directory("conversation_export")))
    {}

    ~Temporary_directory()
    {
        std::error_code error;
        std::filesystem::remove_all(m_path, error);
    }

    const std::filesystem::path& path() const { return m_path; }

private:
    std::filesystem::path m_path;
};

std::string utf8_path(const std::filesystem::path& path)
{
    const auto bytes = path.u8string();
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

addon::Conversation_export_request request_for(const std::filesystem::path& directory)
{
    addon::Conversation_export_request request;
    request.provider_session_id = "native-conversation-1";
    request.working_directory = utf8_path(directory);
    request.launch_environment.platform_id = "linux-x86_64";
    request.launch_environment.effective_environment.set("HOME", addon::Json_value(utf8_path(directory)));
    request.output_directory = utf8_path(directory);
    return request;
}

bool contract_round_trips(const std::filesystem::path& directory)
{
    addon::Contract_failure failure;
    auto request = request_for(directory);
    request.profile_storage = addon::Profile_storage_context{1, utf8_path(directory)};
    request.account_context = addon::Account_context{2, "selected-account", utf8_path(directory)};
    request.resume_record = addon::Json_value::object();
    request.resume_record->set("provider_session_id", addon::Json_value(request.provider_session_id));
    const auto encoded = addon::encode_conversation_export_request(request);
    const auto decoded = addon::decode_conversation_export_request(encoded, &failure);
    SDK_CHECK(decoded && addon::encode_conversation_export_request(*decoded) == encoded,
        "export preserves the native identity, opaque record and exact source account/environment");
    SDK_CHECK(!encoded.find("compact") && decoded && !decoded->compact,
        "absent compact remains a full export for existing readers");
    request.compact = true;
    const auto compact_request = addon::decode_conversation_export_request(
        addon::encode_conversation_export_request(request), &failure);
    SDK_CHECK(compact_request && compact_request->compact, "compact selection survives the request boundary");

    auto invalid = encoded;
    invalid.set("format", addon::Json_value("html"));
    SDK_CHECK(!addon::decode_conversation_export_request(invalid, &failure),
        "export rejects unsupported formats");
    invalid = encoded;
    invalid.set("compact", addon::Json_value("true"));
    SDK_CHECK(!addon::decode_conversation_export_request(invalid, &failure),
        "compact must be a Boolean rather than a truthy string");
    invalid = encoded;
    invalid.set("output_directory", addon::Json_value("relative-output"));
    SDK_CHECK(!addon::decode_conversation_export_request(invalid, &failure),
        "the output allocation must identify an absolute host directory");
    invalid = encoded;
    invalid.erase("launch_environment");
    SDK_CHECK(!addon::decode_conversation_export_request(invalid, &failure),
        "source lookup cannot silently replace a missing source environment with the adapter's environment");
    addon::Conversation_export_result result;
    result.file_name = "../foreign.md";
    result.bytes = 12;
    result.message_count = 1;
    SDK_CHECK(!addon::decode_conversation_export_result(addon::encode_conversation_export_result(result), &failure),
        "a result may only identify its format's fixed output file");
    result.file_name = "conversation.md";
    const auto full_result = addon::encode_conversation_export_result(result);
    const auto decoded_result = addon::decode_conversation_export_result(full_result, &failure);
    SDK_CHECK(!full_result.find("compact") && decoded_result && !decoded_result->compact,
        "an existing result without compact continues to describe a full export");
    result.compact = true;
    const auto compact_result = addon::decode_conversation_export_result(
        addon::encode_conversation_export_result(result), &failure);
    SDK_CHECK(compact_result && compact_result->compact, "the result echoes the compact projection");
    invalid = full_result;
    invalid.set("compact", addon::Json_value(1));
    SDK_CHECK(!addon::decode_conversation_export_result(invalid, &failure),
        "the projection echo must be Boolean");
    return true;
}

bool content_preservation(const std::filesystem::path& directory)
{
    const std::string prompt = "Preserve café, 東京, and this code:\n```cpp\nint n = 2;\n```";
    const std::string answer(addon::limits::k_max_json_string_bytes + 19, 'a');
    const std::string tool_text = "a tool result containing ``` and ````` fences";
    const std::string controls = std::string("control:") + '\0' + '\x1b' + "[31mred" + '\x07';
    addon::Contract_failure failure;
    for (auto format : {addon::Conversation_export_format::TEXT, addon::Conversation_export_format::MARKDOWN,
             addon::Conversation_export_format::CONVERSATION})
    {
        auto request = request_for(directory);
        request.format = format;
        addon::Conversation_writer writer(request);
        SDK_CHECK(writer.open(&failure), failure.detail);
        SDK_CHECK(writer.append({"user", {{addon::Conversation_block_kind::TEXT, prompt, {}}}}, &failure),
            failure.detail);
        SDK_CHECK(writer.append({"assistant", {
            {addon::Conversation_block_kind::TEXT, answer, {}},
            {addon::Conversation_block_kind::TOOL_RESULT, tool_text, "shell"},
            {addon::Conversation_block_kind::TOOL_RESULT, controls, "terminal"},
            {addon::Conversation_block_kind::ATTACHMENT, "[image data omitted]", "image.png"}}}, &failure),
            failure.detail);
        const auto result = writer.finish(true, &failure);
        SDK_CHECK(result && result->message_count == 2 && result->attachment_count == 1 &&
            result->incomplete_tail_omitted, "metadata describes the selected messages and omitted native tail");
        const auto bytes = read_file(directory / result->file_name);
        SDK_CHECK(bytes && addon::is_valid_utf8(*bytes) && bytes->size() == result->bytes,
            "the completed file contains exactly the declared UTF-8 bytes");
        SDK_CHECK(bytes->find(prompt) != std::string::npos && bytes->find(answer) != std::string::npos &&
            bytes->find(tool_text) != std::string::npos && bytes->find("[image data omitted]") != std::string::npos,
            "message text, large bodies, tools and explicit attachment markers are preserved");
        SDK_CHECK(bytes->find("control:\\u0000\\u001b[31mred\\u0007") != std::string::npos &&
            bytes->find('\0') == std::string::npos && bytes->find('\x1b') == std::string::npos,
            "terminal control bytes remain visible in a document the text editor can open");
        SDK_CHECK(bytes->find(prompt) < bytes->find(answer) && bytes->find(answer) < bytes->find(tool_text),
            "provider-selected conversation order is preserved");
        SDK_CHECK(addon::decode_conversation_export_result(
                addon::encode_conversation_export_result(*result), &failure).has_value(),
            "the completed descriptor round trips without carrying any transcript body");
    }
    return true;
}

bool text_label_preservation()
{
    using Kind = addon::Conversation_block_kind;
    const std::vector<addon::Conversation_message> messages{
        {"assistant", {{Kind::TEXT, "Thought body", "Thinking", false}}},
        {"assistant", {{Kind::TEXT, "Analysis body", "Reasoning", false}}},
        {"assistant", {{Kind::TEXT, "Generated image description", "Image prompt", false}}},
        {"agent", {{Kind::TEXT, "[Encrypted message]", {}}}},
    };
    bool labels_preserved = true;
    std::string observed;
    addon::Contract_failure failure;
    for (const auto format : {addon::Conversation_export_format::TEXT, addon::Conversation_export_format::MARKDOWN}) {
        for (const bool compact : {false, true}) {
            Temporary_directory directory;
            auto request = request_for(directory.path());
            request.format = format;
            request.compact = compact;
            addon::Conversation_writer writer(request);
            SDK_CHECK(writer.open(&failure), failure.detail);
            for (const auto& message : messages) {
                SDK_CHECK(writer.append(message, &failure), failure.detail);
            }
            const auto result = writer.finish(false, &failure);
            SDK_CHECK(result && result->message_count == (compact ? 1 : 4),
                "labels do not change which native messages are exported");
            const auto bytes = read_file(directory.path() / result->file_name);
            SDK_CHECK(bytes.has_value(), "read the labelled export");
            const std::string encrypted = std::string(format == addon::Conversation_export_format::MARKDOWN ? "## " : "") +
                "agent\n\n[Encrypted message]\n\n";
            SDK_CHECK(bytes->ends_with(encrypted) && (!compact || *bytes == encrypted),
                "encrypted messages contain only their existing placeholder without a new label or explanation");
            if (!compact) {
                observed += std::string(addon::conversation_export_format_name(format)) + ":\n" + *bytes;
                for (const auto& message : messages) {
                    const auto& block = message.content.front();
                    if (!block.label.empty()) {
                        const auto label = bytes->find(block.label);
                        const auto body = bytes->find(block.text);
                        labels_preserved &= label != std::string::npos && body != std::string::npos && label < body;
                    }
                }
            }
        }
    }
    SDK_CHECK(labels_preserved, "full Text and Markdown retain provider text labels; observed exports:\n" + observed);
    return true;
}

bool compact_projection()
{
    using Kind = addon::Conversation_block_kind;
    using Phase = addon::Conversation_message_phase;
    const std::vector<addon::Conversation_message> messages{
        {"user", {{Kind::TEXT, "Original prompt", {}}, {Kind::ATTACHMENT, "image.png", {}}}},
        {"assistant", {{Kind::TEXT, "Intermediate update", {}, false}}, Phase::COMMENTARY},
        {"tool", {{Kind::TOOL_RESULT, "Tool output", {}, false}}},
        {"agent", {{Kind::TEXT, "[Encrypted message]", {}}}},
        {"assistant", {{Kind::TEXT, "Private reasoning", "Thinking", false},
                          {Kind::TEXT, "Final reply\n::: user\n::::", {}}}, Phase::FINAL},
    };
    addon::Contract_failure failure;
    for (const auto format : {addon::Conversation_export_format::TEXT, addon::Conversation_export_format::MARKDOWN,
             addon::Conversation_export_format::CONVERSATION})
    {
        for (const bool compact : {false, true}) {
            Temporary_directory directory;
            auto request = request_for(directory.path());
            request.format = format;
            request.compact = compact;
            addon::Conversation_writer writer(request);
            SDK_CHECK(writer.open(&failure), failure.detail);
            for (const auto& message : messages) {
                SDK_CHECK(writer.append(message, &failure), failure.detail);
            }
            const auto result = writer.finish(false, &failure);
            SDK_CHECK(result && result->compact == compact && result->message_count == (compact ? 3 : 5) &&
                result->attachment_count == 1, "counts and projection describe only exported messages and blocks");
            const auto bytes = read_file(directory.path() / result->file_name);
            SDK_CHECK(bytes && bytes->find("Original prompt") != std::string::npos &&
                bytes->find("Final reply\n::: user\n::::") != std::string::npos &&
                bytes->find("[Encrypted message]") != std::string::npos,
                "compact preserves user content, final replies, and encrypted-message placeholders");
            for (const auto* hidden : {"Intermediate update", "Tool output", "Private reasoning"}) {
                SDK_CHECK((bytes->find(hidden) == std::string::npos) == compact,
                    "full export retains internal content while compact filters each marked block");
            }
            if (format == addon::Conversation_export_format::CONVERSATION) {
                SDK_CHECK(bytes->starts_with("#!vnm-chat 1\n\n") &&
                    bytes->find("::::: assistant final\nFinal reply\n::: user\n::::\n:::::\n") != std::string::npos,
                    "conversation framing preserves phase and body delimiter text without creating extra records");
            }
        }
    }
    return true;
}

bool snapshot_prefix(const std::filesystem::path& directory)
{
    const auto path = directory / "native.jsonl";
    const std::string body(addon::limits::k_max_json_string_bytes + 23, 'x');
    const std::string complete = "{\"text\":\"" + body + "\"}\n{\"text\":\"second\"}\n";
    SDK_CHECK(write_file(path, complete + "{\"text\":\"in-flight"), "write native conversation fixture");
    std::vector<std::string> messages;
    addon::Contract_failure failure;
    const auto snapshot = addon::read_conversation_jsonl(path, [&](const addon::Json_value& record,
            addon::Contract_failure*)
        {
            messages.push_back(record.find("text")->as_string());
            if (messages.size() == 1) {
                std::ofstream append(path, std::ios::binary | std::ios::app);
                append << "\"}\n{\"text\":\"future\"}\n";
            }
            return true;
        }, {}, &failure);
    SDK_CHECK(snapshot && snapshot->records == 2 && snapshot->complete_bytes == complete.size() &&
        snapshot->incomplete_tail_omitted && messages == std::vector<std::string>({body, "second"}),
        "a snapshot preserves large complete records and stops at its original prefix despite concurrent append");

    SDK_CHECK(write_file(path, "{\"text\":\"first\"}\ninvalid\n"), "write malformed native fixture");
    const auto invalid = addon::read_conversation_jsonl(path,
        [](const addon::Json_value&, addon::Contract_failure*) { return true; }, {}, &failure);
    SDK_CHECK(!invalid && failure.reason_code == "conversation_source_invalid",
        "malformed complete records fail explicitly rather than silently dropping conversation content");
    bool consumed = false;
    const auto cancelled = addon::read_conversation_jsonl(path,
        [&](const addon::Json_value&, addon::Contract_failure*) { consumed = true; return true; },
        [] { return true; }, &failure);
    SDK_CHECK(!cancelled && !consumed && failure.reason_code == "cancelled",
        "cancelled snapshots deliver no source records");
    return true;
}

bool partial_cleanup(const std::filesystem::path& directory)
{
    auto request = request_for(directory);
    const auto output = directory / addon::conversation_export_file_name(request.format);
    std::filesystem::remove(output);
    bool cancelled = false;
    addon::Contract_failure failure;
    {
        addon::Conversation_writer writer(request, [&] { return cancelled; });
        SDK_CHECK(writer.open(&failure), failure.detail);
        SDK_CHECK(writer.append({"user", {{addon::Conversation_block_kind::TEXT, "unsaved", {}}}}, &failure),
            failure.detail);
        cancelled = true;
        SDK_CHECK(!writer.finish(false, &failure) && failure.reason_code == "cancelled",
            "cancellation cannot publish a partial conversation as a successful export");
    }
    SDK_CHECK(!std::filesystem::exists(output) &&
        !std::filesystem::exists(directory / "conversation.md.partial"),
        "unsuccessful writers reclaim the partial file they own");
    return true;
}

} // namespace

bool test_conversation_export_contracts()
{
    // Content equality and snapshot boundaries are the oracles; presentation
    // choices such as heading spacing are deliberately not pinned by this test.
    Temporary_directory directory;
    return contract_round_trips(directory.path()) && content_preservation(directory.path()) &&
        text_label_preservation() && compact_projection() &&
        snapshot_prefix(directory.path()) && partial_cleanup(directory.path());
}

} // namespace sdk_tests
