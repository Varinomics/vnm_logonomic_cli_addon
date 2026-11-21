#include "logonomic_cli_addon/lifecycle_events.h"

#include <algorithm>

namespace logonomic::cli_addon {
namespace {

constexpr std::string_view k_whitespace = " \t\r\n";

std::string_view trim(std::string_view text)
{
    const auto first = text.find_first_not_of(k_whitespace);
    if (first == std::string_view::npos) {
        return {};
    }
    return text.substr(first, text.find_last_not_of(k_whitespace) - first + 1);
}

bool continuation_byte(char byte)
{
    return (static_cast<unsigned char>(byte) & 0xc0u) == 0x80u;
}

std::string_view prefix_edge(std::string_view text)
{
    auto size = std::min(text.size(), k_assistant_reply_edge_bytes);
    while (size < text.size() && continuation_byte(text[size])) {
        --size;
    }
    return text.substr(0, size);
}

std::string_view suffix_edge(std::string_view text)
{
    auto first = text.size() > k_assistant_reply_edge_bytes ? text.size() - k_assistant_reply_edge_bytes : 0;
    while (first < text.size() && continuation_byte(text[first])) {
        ++first;
    }
    return text.substr(first);
}

bool matches(const Json_value* value, std::string_view expected)
{
    return value && value->is_string() && value->as_string() == expected;
}

bool valid_edge(const Json_value* value)
{
    return value && value->is_string() && !value->as_string().empty() &&
        value->as_string().size() <= k_assistant_reply_edge_bytes &&
        value->as_string().find('\0') == std::string::npos && is_valid_utf8(value->as_string());
}

} // namespace

Json_value assistant_reply_evidence(std::string_view text)
{
    if (text.find('\0') != std::string_view::npos || !is_valid_utf8(text)) {
        return {};
    }

    // Inspect every line before bounding the evidence. In particular, blank lines
    // do not end a fence, and an unmarked lazy continuation remains in its quote.
    std::size_t paragraph_begin = 0;
    std::size_t paragraph_end = 0;
    bool next_paragraph = true;
    bool prose          = false;
    char fence          = 0;
    std::size_t fence_width = 0;
    for (std::size_t begin = 0; begin < text.size();) {
        const auto separator = text.find_first_of("\r\n", begin);
        const auto end   = separator == std::string_view::npos ? text.size() : separator;
        const auto line  = text.substr(begin, end - begin);
        const auto first = line.find_first_not_of(" \t");
        const bool blank = first == std::string_view::npos;
        const auto content = blank ? std::string_view{} : line.substr(first);
        const bool indented = !blank && (first >= 4 || line.substr(0, first).find('\t') != std::string_view::npos);
        const char marker = !indented && !content.empty() &&
            (content.front() == '`' || content.front() == '~') ? content.front() : 0;
        const auto marker_end = marker ? content.find_first_not_of(marker) : 0;
        const auto width      = marker_end == std::string_view::npos ? content.size() : marker_end;
        const auto rest       = content.substr(width);

        if (fence) {
            prose = false;
            if (marker == fence && width >= fence_width &&
                rest.find_first_not_of(" \t") == std::string_view::npos)
            {
                fence = 0;
            }
            next_paragraph = true;
        }
        else
        if (blank) {
            next_paragraph = true;
        }
        else
        if (marker && width >= 3 && (marker != '`' || rest.find('`') == std::string_view::npos)) {
            fence          = marker;
            fence_width    = width;
            prose          = false;
            next_paragraph = true;
        }
        else {
            if (next_paragraph) {
                paragraph_begin = begin;
                prose = !indented && !content.starts_with(">") && !content.starts_with("\"") &&
                    !content.starts_with("`");
            }
            // A block quote can interrupt a prose paragraph without a blank line.
            if (content.starts_with(">")) {
                prose = false;
            }
            paragraph_end = end;
            next_paragraph = false;
        }
        begin = end;
        if (begin < text.size() && text[begin++] == '\r' && begin < text.size() && text[begin] == '\n') {
            ++begin;
        }
    }
    if (!prose) {
        return {};
    }
    const auto paragraph = trim(text.substr(paragraph_begin, paragraph_end - paragraph_begin));
    if (paragraph.empty()) {
        return {};
    }
    auto evidence = Json_value::object();
    evidence.set("scope", Json_value("final_paragraph_edges"));
    evidence.set("context", Json_value("prose"));
    evidence.set("prefix", Json_value(prefix_edge(paragraph)));
    evidence.set("suffix", Json_value(suffix_edge(paragraph)));
    return evidence;
}

bool looks_like_reply_request_evidence(const Json_value& evidence)
{
    if (!evidence.is_object() || evidence.as_object().size() != 4 ||
        !matches(evidence.find("scope"), "final_paragraph_edges") ||
        !matches(evidence.find("context"), "prose") ||
        !valid_edge(evidence.find("prefix")) || !valid_edge(evidence.find("suffix")))
    {
        return false;
    }
    auto prefix = trim(evidence.find("prefix")->as_string());
    auto suffix = trim(evidence.find("suffix")->as_string());
    if (prefix.starts_with(">") || prefix.starts_with("\"") ||
        prefix.starts_with("`") || suffix.ends_with("```"))
    {
        return false;
    }
    // Markdown emphasis is presentation, not the closing question punctuation.
    constexpr std::string_view emphasis = "*_";
    prefix.remove_prefix(std::min(prefix.find_first_not_of(emphasis), prefix.size()));
    suffix = suffix.substr(0, suffix.find_last_not_of(emphasis) + 1);
    if (suffix.ends_with("?") || suffix.ends_with("？")) {
        return true;
    }
    std::string beginning(prefix);
    for (char& c : beginning) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c + ('a' - 'A'));
        }
    }
    return beginning.starts_with("please tell me ") || beginning.starts_with("please provide ") ||
        beginning.starts_with("let me know which ");
}

bool looks_like_reply_request(std::string_view text)
{
    return looks_like_reply_request_evidence(assistant_reply_evidence(text));
}

} // namespace logonomic::cli_addon
