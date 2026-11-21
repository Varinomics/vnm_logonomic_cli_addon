#pragma once

#include "logonomic_cli_addon/json.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <string_view>

namespace logonomic::cli_addon {

enum class Frame_violation
{
    NONE,
    OVERSIZE_FRAME,       // a frame, terminator included, exceeds max_frame_bytes
    INVALID_UTF8,
    EMPTY_FRAME,          // a bare newline
    CARRIAGE_RETURN,      // raw 0x0D inside a frame (\r\n included)
    UNTERMINATED_FRAME,   // end of stream inside a frame
};

/// `oversize_frame`, `invalid_utf8`, or `frame_violation`; empty for NONE.
const char* frame_violation_reason_code(Frame_violation violation);

/**
 * @brief Splits a byte stream into newline-terminated frames
 * (docs/core_protocol.md, "Transport"). Feed bytes as they arrive; complete
 * frames become available without their terminator. The first violation
 * poisons the decoder: feed() returns false from then on and no further
 * frame is produced, because a violation retires the generation.
 */
class Frame_decoder
{
public:
    explicit Frame_decoder(std::uint64_t max_frame_bytes);

    bool feed(std::string_view bytes);

    /// Signals end of stream; pending partial bytes are a violation.
    bool finish();

    bool next_frame(std::string& out_frame);

    /// Lowers the bound once the handshake fixed the effective limit.
    void set_max_frame_bytes(std::uint64_t max_frame_bytes) { m_max_frame_bytes = max_frame_bytes; }

    Frame_violation violation() const { return m_violation; }
    bool violated() const { return m_violation != Frame_violation::NONE; }
    std::size_t pending_bytes() const { return m_pending.size(); }

private:
    bool fail(Frame_violation violation);

    std::uint64_t           m_max_frame_bytes;
    std::string             m_pending;
    std::deque<std::string> m_frames;
    Frame_violation         m_violation = Frame_violation::NONE;
};

/// Compact serialization plus the terminating newline: the bytes written to
/// the transport for one message.
std::string encode_frame(const Json_value& value);

/**
 * @brief Bounded tail of an adapter's stderr (docs/process_contract.md,
 * "Standard streams"). Bytes beyond the capacity drop from the front and the
 * text then begins with the truncation marker. Overflow is a diagnostic
 * condition, never a failure.
 */
class Stderr_ring
{
public:
    explicit Stderr_ring(std::size_t capacity = limits::k_stderr_ring_bytes);

    void append(std::string_view bytes);

    bool truncated() const { return m_truncated; }
    std::uint64_t total_bytes() const { return m_total_bytes; }

    /// The retained tail, prefixed with the marker once truncated.
    std::string text() const;

private:
    std::size_t   m_capacity;
    std::string   m_tail;
    bool          m_truncated   = false;
    std::uint64_t m_total_bytes = 0;
};

} // namespace logonomic::cli_addon
