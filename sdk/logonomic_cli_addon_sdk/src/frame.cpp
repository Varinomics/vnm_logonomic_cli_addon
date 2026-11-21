#include "logonomic_cli_addon/frame.h"
#include "logonomic_cli_addon/ids.h"
#include "logonomic_cli_addon/limits.h"

namespace logonomic::cli_addon {

const char* frame_violation_reason_code(Frame_violation violation)
{
    switch (violation) {
        case Frame_violation::NONE:               return "";
        case Frame_violation::OVERSIZE_FRAME:     return reason::k_oversize_frame;
        case Frame_violation::INVALID_UTF8:       return reason::k_invalid_utf8;
        case Frame_violation::EMPTY_FRAME:
        case Frame_violation::CARRIAGE_RETURN:
        case Frame_violation::UNTERMINATED_FRAME:
        default:                                  return reason::k_frame_violation;
    }
}

Frame_decoder::Frame_decoder(std::uint64_t max_frame_bytes)
:
    m_max_frame_bytes(max_frame_bytes)
{}

bool Frame_decoder::fail(Frame_violation violation)
{
    m_violation = violation;
    m_pending.clear();
    m_frames.clear();
    return false;
}

bool Frame_decoder::feed(std::string_view bytes)
{
    if (violated()) {
        return false;
    }

    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const std::size_t newline = bytes.find('\n', offset);
        if (newline == std::string_view::npos) {
            m_pending.append(bytes.data() + offset, bytes.size() - offset);
            // A frame needs its terminator too, so pending bytes equal to the
            // limit can no longer complete within it.
            if (m_pending.size() >= m_max_frame_bytes) {
                return fail(Frame_violation::OVERSIZE_FRAME);
            }
            return true;
        }

        m_pending.append(bytes.data() + offset, newline - offset);
        offset = newline + 1;

        if (m_pending.size() + 1 > m_max_frame_bytes) {
            return fail(Frame_violation::OVERSIZE_FRAME);
        }
        if (m_pending.empty()) {
            return fail(Frame_violation::EMPTY_FRAME);
        }
        if (m_pending.find('\r') != std::string::npos) {
            return fail(Frame_violation::CARRIAGE_RETURN);
        }
        if (!is_valid_utf8(m_pending)) {
            return fail(Frame_violation::INVALID_UTF8);
        }
        m_frames.push_back(std::move(m_pending));
        m_pending.clear();
    }
    return true;
}

bool Frame_decoder::finish()
{
    if (violated()) {
        return false;
    }
    if (!m_pending.empty()) {
        return fail(Frame_violation::UNTERMINATED_FRAME);
    }
    return true;
}

bool Frame_decoder::next_frame(std::string& out_frame)
{
    if (m_frames.empty()) {
        return false;
    }
    out_frame = std::move(m_frames.front());
    m_frames.pop_front();
    return true;
}

std::string encode_frame(const Json_value& value)
{
    std::string frame = serialize_json(value);
    frame.push_back('\n');
    return frame;
}

Stderr_ring::Stderr_ring(std::size_t capacity)
:
    m_capacity(capacity)
{}

void Stderr_ring::append(std::string_view bytes)
{
    m_total_bytes += bytes.size();
    if (bytes.size() > m_capacity) {
        m_tail.assign(bytes.data() + (bytes.size() - m_capacity), m_capacity);
        m_truncated = true;
        return;
    }
    m_tail.append(bytes.data(), bytes.size());
    if (m_tail.size() > m_capacity) {
        m_tail.erase(0, m_tail.size() - m_capacity);
        m_truncated = true;
    }
}

std::string Stderr_ring::text() const
{
    if (!m_truncated) {
        return m_tail;
    }
    return std::string(limits::k_stderr_truncation_marker) + m_tail;
}

} // namespace logonomic::cli_addon
