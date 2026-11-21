#include "logonomic_cli_addon/transcript.h"

#include "logonomic_cli_addon/capability_ids.h"
#include "logonomic_cli_addon/ids.h"
#include "logonomic_cli_addon/session_wake.h"

#include <algorithm>

namespace logonomic::cli_addon {

namespace {

constexpr const char* k_host_prefix    = "> ";
constexpr const char* k_adapter_prefix = "< ";
constexpr const char* k_event_prefix   = "! ";

} // namespace

void Transcript_recorder::set_placeholders(Transcript_placeholders placeholders)
{
    m_placeholders = std::move(placeholders);
}

std::string Transcript_recorder::normalize_request_id(std::string_view request_id)
{
    const auto existing = m_request_ordinals.find(std::string(request_id));
    if (existing != m_request_ordinals.end()) {
        return existing->second;
    }
    const std::string ordinal = "<r#" + std::to_string(m_request_ordinals.size() + 1) + ">";
    m_request_ordinals.emplace(std::string(request_id), ordinal);
    return ordinal;
}

std::string Transcript_recorder::normalize_string(std::string_view text)
{
    std::string redacted(text);
    for (const auto& secret : m_startup_secrets) {
        std::size_t offset = 0;
        while ((offset = redacted.find(secret, offset)) != std::string::npos) {
            redacted.replace(offset, secret.size(), "<startup-secret>");
            offset += std::string_view("<startup-secret>").size();
        }
    }
    for (const auto& secret : m_session_capabilities) {
        std::size_t offset = 0;
        while ((offset = redacted.find(secret, offset)) != std::string::npos) {
            redacted.replace(offset, secret.size(), "<session-authority>");
            offset += std::string_view("<session-authority>").size();
        }
    }
    text = redacted;
    if (!m_placeholders.generation_digest.empty() && text == m_placeholders.generation_digest) {
        return "<digest>";
    }
    if (!m_placeholders.generation_nonce.empty() && text == m_placeholders.generation_nonce) {
        return "<nonce>";
    }
    for (const std::string& root : m_placeholders.roots) {
        const std::string_view prefix = text.substr(0, root.size());
#if defined(_WIN32)
        // Qt and native filesystem APIs can spell the same root with different separators.
        const bool matches = std::equal(
            prefix.begin(), prefix.end(), root.begin(), root.end(),
            [](char left, char right) {
                return left == right ||
                    ((left == '/' || left == '\\') && (right == '/' || right == '\\'));
            });
#else
        const bool matches = prefix == root;
#endif
        if (!root.empty() && matches) {
            return "<root>" + std::string(text.substr(root.size()));
        }
    }
    return std::string(text);
}

void Transcript_recorder::collect_private_authority(const Json_value& value)
{
    if (value.is_object()) {
        const auto* mechanism = value.find("mechanism");
        const auto* authorization = value.find("authorization");
        if (mechanism && mechanism->is_string() && mechanism->as_string() == "http-json-post" &&
            authorization && authorization->is_string() && authorization->as_string().starts_with("Bearer ") &&
            authorization->as_string().size() == 71)
        {
            const auto token = authorization->as_string().substr(7);
            if (std::find(m_startup_secrets.begin(), m_startup_secrets.end(), token) == m_startup_secrets.end()) {
                m_startup_secrets.push_back(token);
            }
        }
        const auto* capability = value.find("session_capability");
        if (capability && capability->is_string() && is_valid_sha256_hex(capability->as_string()) &&
            std::find(m_session_capabilities.begin(), m_session_capabilities.end(), capability->as_string()) ==
                m_session_capabilities.end())
        {
            m_session_capabilities.push_back(capability->as_string());
        }
        for (const auto& member : value.as_object()) collect_private_authority(member.value);
    }
    else if (value.is_array()) {
        for (const auto& item : value.as_array()) collect_private_authority(item);
    }
}

void Transcript_recorder::retain_session_wake_strings(const Json_value& value)
{
    if (value.is_string()) {
        const auto& text = value.as_string();
        if (!text.empty() && std::find(m_session_wake_values.begin(), m_session_wake_values.end(), text) ==
            m_session_wake_values.end())
        {
            m_session_wake_values.push_back(text);
        }
    }
    else
    if (value.is_object()) {
        for (const auto& member : value.as_object()) {
            retain_session_wake_strings(member.value);
        }
    }
    else
    if (value.is_array()) {
        for (const auto& item : value.as_array()) {
            retain_session_wake_strings(item);
        }
    }
}

void Transcript_recorder::collect_session_wake_bindings(const Json_value& value)
{
    if (value.is_object()) {
        if (const auto* preparation = value.find(k_session_wake_member)) {
            if (const auto* binding = preparation->find("binding")) {
                retain_session_wake_strings(*binding);
            }
        }
        const auto* capability = value.find("capability_id");
        const auto* payload    = value.find("payload");
        if (capability != nullptr && capability->is_string() &&
            capability->as_string() == k_session_wake_capability_id && payload != nullptr)
        {
            if (const auto* binding = payload->find("binding")) {
                retain_session_wake_strings(*binding);
            }
        }
        for (const auto& member : value.as_object()) {
            collect_session_wake_bindings(member.value);
        }
    }
    else
    if (value.is_array()) {
        for (const auto& item : value.as_array()) {
            collect_session_wake_bindings(item);
        }
    }
}

bool Transcript_recorder::redact_session_wake_values(Json_value& value) const
{
    if (value.is_string()) {
        if (std::find(m_session_wake_values.begin(), m_session_wake_values.end(), value.as_string()) !=
            m_session_wake_values.end())
        {
            value = Json_value("<session-wake-private>");
            return true;
        }
        return false;
    }
    bool changed = false;
    if (value.is_object()) {
        for (auto& member : value.as_object()) {
            changed |= redact_session_wake_values(member.value);
        }
    }
    else
    if (value.is_array()) {
        for (auto& item : value.as_array()) {
            changed |= redact_session_wake_values(item);
        }
    }
    return changed;
}

std::string Transcript_recorder::normalize_private_content(std::string_view text)
{
    if (m_session_wake_values.empty()) {
        return normalize_string(text);
    }
    Json_value value(text);
    if (redact_session_wake_values(value)) {
        return value.as_string();
    }
    // Providers may copy a binding into an environment value as JSON. Redact
    // typed string values rather than replacing bytes such as '/' or 'true'
    // throughout that document or the common protocol envelope.
    auto encoded = parse_json(text, {}, nullptr);
    if (encoded && redact_session_wake_values(*encoded)) {
        return normalize_string(serialize_json_canonical(*encoded));
    }
    return normalize_string(text);
}

Json_value Transcript_recorder::normalize(
    const Json_value& value, std::string_view member_name, bool private_content)
{
    switch (value.type()) {
        case Json_type::STRING:
            if (member_name == "request_id") {
                return Json_value(normalize_request_id(value.as_string()));
            }
            return Json_value(private_content
                ? normalize_private_content(value.as_string()) : normalize_string(value.as_string()));
        case Json_type::ARRAY: {
            Json_value array = Json_value::array();
            for (const Json_value& item : value.as_array()) {
                array.push_back(normalize(item, {}, private_content));
            }
            return array;
        }
        case Json_type::OBJECT: {
            Json_value object = Json_value::object();
            const auto* capability = value.find("capability_id");
            const bool wake_invocation = capability != nullptr && capability->is_string() &&
                capability->as_string() == k_session_wake_capability_id;
            for (const Json_member& member : value.as_object()) {
                if (member.name == k_session_wake_member || (wake_invocation && member.name == "payload")) {
                    object.set(member.name, Json_value("<session-wake-private>"));
                }
                else
                if (member.name == "content_base64" && value.find("grant")) {
                    object.set(member.name, Json_value("<managed-configuration-content>"));
                }
                else
                if ((member_name == "host" || member_name == "adapter") && member.name == "version") {
                    object.set(member.name, Json_value("<version>"));
                }
                else
                if (member_name == "host" && member.name == "product") {
                    object.set(member.name, Json_value("<product>"));
                }
                else {
                    const bool content = private_content || member.name == "payload" ||
                        member.name == "environment" || member.name == "argv" || member.name == "stdin_payload";
                    object.set(member.name, normalize(member.value, member.name, content));
                }
            }
            return object;
        }
        default:
            return value;
    }
}

void Transcript_recorder::record_host_frame(const Json_value& message)
{
    collect_private_authority(message);
    collect_session_wake_bindings(message);
    m_lines.push_back(k_host_prefix + serialize_json_canonical(normalize(message, {})));
}

void Transcript_recorder::record_adapter_frame(const Json_value& message)
{
    collect_private_authority(message);
    collect_session_wake_bindings(message);
    m_lines.push_back(k_adapter_prefix + serialize_json_canonical(normalize(message, {})));
}

void Transcript_recorder::record_event(std::string_view kind, std::string_view detail)
{
    std::string line = k_event_prefix + std::string(kind);
    if (!detail.empty()) {
        // Request IDs appear in event details as whole words; normalize each.
        std::string normalized;
        std::size_t start = 0;
        while (start <= detail.size()) {
            const std::size_t space = detail.find(' ', start);
            const std::string_view word = detail.substr(start, space == std::string_view::npos ? std::string_view::npos : space - start);
            if (!normalized.empty() || start > 0) {
                normalized.push_back(' ');
            }
            if (m_request_ordinals.count(std::string(word)) != 0 ||
                (word.size() >= 2 && word.front() == 'r' && word.find_first_not_of("0123456789", 1) == std::string_view::npos))
            {
                normalized += normalize_request_id(word);
            }
            else {
                normalized += normalize_string(word);
            }
            if (space == std::string_view::npos) {
                break;
            }
            start = space + 1;
        }
        line += " " + normalized;
    }
    m_lines.push_back(std::move(line));
}

std::string Transcript_recorder::text() const
{
    std::string text;
    for (const std::string& line : m_lines) {
        text += line;
        text.push_back('\n');
    }
    return text;
}

void Transcript_recorder::clear()
{
    m_lines.clear();
    m_request_ordinals.clear();
}

bool compare_transcripts(
    const std::vector<std::string>& expected,
    const std::vector<std::string>& actual,
    Transcript_difference*          out_difference)
{
    const std::size_t common = std::min(expected.size(), actual.size());
    for (std::size_t i = 0; i < common; ++i) {
        if (expected[i] != actual[i]) {
            if (out_difference != nullptr) {
                out_difference->line     = i;
                out_difference->expected = expected[i];
                out_difference->actual   = actual[i];
            }
            return false;
        }
    }
    if (expected.size() != actual.size()) {
        if (out_difference != nullptr) {
            out_difference->line     = common;
            out_difference->expected = common < expected.size() ? expected[common] : std::string("<end>");
            out_difference->actual   = common < actual.size() ? actual[common] : std::string("<end>");
        }
        return false;
    }
    return true;
}

std::vector<std::string> split_transcript_lines(std::string_view text)
{
    std::vector<std::string> lines;
    std::size_t start = 0;
    while (start < text.size()) {
        const std::size_t newline = text.find('\n', start);
        if (newline == std::string_view::npos) {
            lines.emplace_back(text.substr(start));
            break;
        }
        lines.emplace_back(text.substr(start, newline - start));
        start = newline + 1;
    }
    return lines;
}

} // namespace logonomic::cli_addon
