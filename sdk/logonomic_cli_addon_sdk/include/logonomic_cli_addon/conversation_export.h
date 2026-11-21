#pragma once

#include "logonomic_cli_addon/accounts.h"
#include "logonomic_cli_addon/json.h"
#include "logonomic_cli_addon/launcher_discovery.h"
#include "logonomic_cli_addon/profile_storage.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace logonomic::cli_addon {

class Invocation_context;

inline constexpr const char* k_conversation_export_operation = "export";

enum class Conversation_export_format
{
    TEXT,
    MARKDOWN,
    CONVERSATION,
};

const char* conversation_export_format_name(Conversation_export_format format);
std::optional<Conversation_export_format> parse_conversation_export_format(std::string_view name);
const char* conversation_export_file_name(Conversation_export_format format);

struct Conversation_export_request
{
    std::string                            provider_session_id;
    std::optional<Json_value>              resume_record;
    std::string                            working_directory;
    Launch_environment                     launch_environment;
    std::string                            output_directory;
    Conversation_export_format             format = Conversation_export_format::MARKDOWN;
    std::optional<Profile_storage_context> profile_storage;
    std::optional<Account_context>         account_context;
    bool                                   compact = false;
};

struct Conversation_export_result
{
    Conversation_export_format             format = Conversation_export_format::MARKDOWN;
    std::string                            file_name;
    std::uint64_t                          bytes = 0;
    std::uint64_t                          message_count = 0;
    std::uint64_t                          attachment_count = 0;
    bool                                   incomplete_tail_omitted = false;
    std::optional<Profile_storage_context> profile_storage;
    std::optional<Account_context>         account_context;
    bool                                   compact = false;
};

// Both capability versions use this declaration and the same export codecs.
// Version 1 names optional source inputs; version 2 requires every declared
// name in the finalized prepared ambient view before the host saves a source.
std::optional<Launcher_discovery_declaration> decode_conversation_export_declaration(
    const Json_value& payload, Contract_failure* out_failure);
Json_value encode_conversation_export_request(const Conversation_export_request& request);
std::optional<Conversation_export_request> decode_conversation_export_request(
    const Json_value& payload, Contract_failure* out_failure);
Json_value encode_conversation_export_result(const Conversation_export_result& result);
std::optional<Conversation_export_result> decode_conversation_export_result(
    const Json_value& payload, Contract_failure* out_failure);

void handle_conversation_export(Invocation_context& context,
    const std::function<std::optional<Conversation_export_result>(
        const Conversation_export_request&, const std::function<bool()>&, Contract_failure*)>& exporter);

enum class Conversation_block_kind
{
    TEXT,
    TOOL_CALL,
    TOOL_RESULT,
    ATTACHMENT,
};

struct Conversation_block
{
    Conversation_block_kind kind = Conversation_block_kind::TEXT;
    std::string             text;
    std::string             label;
    bool                    compact_visible = true;
};

enum class Conversation_message_phase
{
    UNKNOWN,
    FINAL,
    COMMENTARY,
};

struct Conversation_message
{
    std::string                     role;
    std::vector<Conversation_block> content;
    Conversation_message_phase     phase = Conversation_message_phase::UNKNOWN;
};

// Provider readers own native ordering, branch selection and duplicate
// suppression. This writer only renders their selected messages and retains
// an output file after successful completion.
class Conversation_writer
{
public:
    explicit Conversation_writer(
        const Conversation_export_request& request, std::function<bool()> cancelled = {});
    ~Conversation_writer();
    Conversation_writer(const Conversation_writer&) = delete;
    Conversation_writer& operator=(const Conversation_writer&) = delete;

    bool open(Contract_failure* out_failure);
    bool append(const Conversation_message& message, Contract_failure* out_failure);
    std::optional<Conversation_export_result> finish(
        bool incomplete_tail_omitted, Contract_failure* out_failure);

private:
    bool write(std::string_view bytes, Contract_failure* out_failure);
    bool write_text(std::string_view text, Contract_failure* out_failure);
    bool write_literal(std::string_view text, Contract_failure* out_failure);
    bool write_conversation_block(const Conversation_message& message,
        const Conversation_block& block, Contract_failure* out_failure);
    bool check_cancelled(Contract_failure* out_failure);

    Conversation_export_request m_request;
    std::function<bool()>        m_cancelled;
    std::filesystem::path       m_partial_path;
    std::filesystem::path       m_output_path;
    std::ofstream               m_stream;
    Conversation_export_result  m_result;
    bool                        m_finished = false;
    bool                        m_failed = false;
};

struct Conversation_snapshot
{
    std::uint64_t bytes = 0;
    std::uint64_t complete_bytes = 0;
    std::uint64_t records = 0;
    bool          incomplete_tail_omitted = false;
};

// Native logs are append-only. Freeze the open file's size and consume only
// complete JSONL records within that prefix, so an active conversation never
// waits for its next turn. Native text is not subject to IPC string bounds.
std::optional<Conversation_snapshot> read_conversation_jsonl(
    const std::filesystem::path& path,
    const std::function<bool(const Json_value&, Contract_failure*)>& consume,
    const std::function<bool()>& cancelled,
    Contract_failure* out_failure);

} // namespace logonomic::cli_addon
