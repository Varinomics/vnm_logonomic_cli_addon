#pragma once

#include "logonomic_cli_addon/json.h"

#include <cstddef>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace logonomic::cli_addon {

/**
 * @brief Values a transcript replaces with placeholders so that two runs of
 * the same script against the same adapter compare byte-equal: the
 * generation digest and nonce, and every host-allocated root.
 */
struct Transcript_placeholders
{
    std::string              generation_digest;
    std::string              generation_nonce;
    std::vector<std::string> roots;
};

/**
 * @brief Records the frames and host-side events of one adapter session in
 * normalized form (docs/conformance.md, "Transcripts"): request IDs become
 * ordinals in order of first appearance, digest, nonce and roots become
 * placeholders, informational host and adapter versions become `<version>`,
 * the host product becomes `<product>`, and no timestamps are recorded. The
 * host simulator and Logonomic's broker record through this one class, and
 * compare_transcripts decides whether their observations agree.
 */
class Transcript_recorder
{
public:
    void set_placeholders(Transcript_placeholders placeholders);

    /// A frame the host sent, as its JSON object.
    void record_host_frame(const Json_value& message);

    /// A frame the adapter sent, as its decoded JSON object.
    void record_adapter_frame(const Json_value& message);

    /// A host-side observation that is not a frame (exit, violation,
    /// deadline). Request IDs inside the detail are normalized.
    void record_event(std::string_view kind, std::string_view detail = {});

    const std::vector<std::string>& lines() const { return m_lines; }
    std::string text() const;
    void clear();

    /// The ordinal placeholder for a request ID, assigning the next ordinal
    /// on first sight.
    std::string normalize_request_id(std::string_view request_id);

private:
    Json_value  normalize(const Json_value& value, std::string_view member_name, bool private_content = false);
    std::string normalize_string(std::string_view text);
    void collect_private_authority(const Json_value& value);
    void collect_session_wake_bindings(const Json_value& value);
    void retain_session_wake_strings(const Json_value& value);
    bool redact_session_wake_values(Json_value& value) const;
    std::string normalize_private_content(std::string_view text);

    Transcript_placeholders            m_placeholders;
    std::map<std::string, std::string> m_request_ordinals;
    std::vector<std::string>           m_lines;
    std::vector<std::string>           m_startup_secrets;
    std::vector<std::string>           m_session_capabilities;
    std::vector<std::string>           m_session_wake_values;
};

struct Transcript_difference
{
    std::size_t line = 0;   // zero-based index of the first differing line
    std::string expected;
    std::string actual;
};

/// Line-by-line comparison of two normalized transcripts.
bool compare_transcripts(
    const std::vector<std::string>& expected,
    const std::vector<std::string>& actual,
    Transcript_difference*          out_difference);

/// Splits transcript text into its lines, dropping a trailing empty line.
std::vector<std::string> split_transcript_lines(std::string_view text);

} // namespace logonomic::cli_addon
