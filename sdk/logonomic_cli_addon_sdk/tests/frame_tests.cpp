#include "sdk_test_support.h"

#include "logonomic_cli_addon/frame.h"
#include "logonomic_cli_addon/ids.h"
#include "logonomic_cli_addon/limits.h"

#include <string>

namespace sdk_tests {

namespace {

namespace addon = logonomic::cli_addon;

} // namespace

bool test_frame_decoder()
{
    bool ok = true;
    const char* id = __func__;

    {
        addon::Frame_decoder decoder(64);
        std::string frame;
        ok &= check(decoder.feed("{\"a\":1}\n{\"b\"") && decoder.next_frame(frame) && frame == "{\"a\":1}", id,
            "first complete frame is yielded without its terminator");
        ok &= check(!decoder.next_frame(frame), id, "partial frame is not yielded");
        ok &= check(decoder.pending_bytes() == 4, id, "partial bytes are pending");
        ok &= check(decoder.feed(":2}\n") && decoder.next_frame(frame) && frame == "{\"b\":2}", id,
            "frame split across feeds is reassembled");
        ok &= check(decoder.feed("x\ny\n") && decoder.next_frame(frame) && frame == "x" && decoder.next_frame(frame) && frame == "y", id,
            "two frames in one feed");
        ok &= check(decoder.finish() && !decoder.violated(), id, "clean end of stream");
    }
    {
        addon::Frame_decoder decoder(8);
        ok &= check(decoder.feed("1234567\n"), id, "frame of exactly max bytes including the newline is accepted");
        ok &= check(!decoder.feed("12345678\n") && decoder.violation() == addon::Frame_violation::OVERSIZE_FRAME, id,
            "frame one byte over the bound is oversize");
        ok &= check(std::string(addon::frame_violation_reason_code(decoder.violation())) == addon::reason::k_oversize_frame, id,
            "oversize reason code");
        std::string frame;
        ok &= check(!decoder.next_frame(frame), id, "violated decoder yields nothing");
        ok &= check(!decoder.feed("x\n"), id, "violated decoder stays violated");
    }
    {
        addon::Frame_decoder decoder(8);
        ok &= check(!decoder.feed("12345678") && decoder.violation() == addon::Frame_violation::OVERSIZE_FRAME, id,
            "pending bytes at the bound without a terminator are oversize");
    }
    {
        addon::Frame_decoder decoder(64);
        ok &= check(!decoder.feed("\n") && decoder.violation() == addon::Frame_violation::EMPTY_FRAME, id, "empty line");
        ok &= check(std::string(addon::frame_violation_reason_code(decoder.violation())) == addon::reason::k_frame_violation, id,
            "empty frame reason code");
    }
    {
        addon::Frame_decoder decoder(64);
        ok &= check(!decoder.feed("{}\r\n") && decoder.violation() == addon::Frame_violation::CARRIAGE_RETURN, id, "CRLF");
    }
    {
        addon::Frame_decoder decoder(64);
        ok &= check(!decoder.feed("{\"a\":\"\xFF\"}\n") && decoder.violation() == addon::Frame_violation::INVALID_UTF8, id,
            "invalid UTF-8");
        ok &= check(std::string(addon::frame_violation_reason_code(decoder.violation())) == addon::reason::k_invalid_utf8, id,
            "invalid UTF-8 reason code");
    }
    {
        addon::Frame_decoder decoder(64);
        ok &= check(decoder.feed("{\"a\":1}") && !decoder.finish() && decoder.violation() == addon::Frame_violation::UNTERMINATED_FRAME, id,
            "end of stream inside a frame");
    }
    {
        addon::Frame_decoder decoder(64);
        decoder.set_max_frame_bytes(4);
        ok &= check(!decoder.feed("12345\n") && decoder.violation() == addon::Frame_violation::OVERSIZE_FRAME, id,
            "lowered bound applies");
    }
    {
        // Below, at and above k_max_frame_bytes itself, the terminator counted.
        const std::size_t bound = (std::size_t)addon::limits::k_max_frame_bytes;
        std::string frame;
        addon::Frame_decoder decoder(addon::limits::k_max_frame_bytes);
        ok &= check(decoder.feed(std::string(bound / 2, 'b') + "\n") && decoder.next_frame(frame) && frame.size() == bound / 2, id,
            "frame below k_max_frame_bytes is accepted");
        ok &= check(decoder.feed(std::string(bound - 1, 'a') + "\n") && decoder.next_frame(frame) && frame.size() == bound - 1, id,
            "frame of exactly k_max_frame_bytes is accepted");
        ok &= check(!decoder.feed(std::string(bound, 'c') + "\n") && decoder.violation() == addon::Frame_violation::OVERSIZE_FRAME, id,
            "frame of k_max_frame_bytes + 1 is oversize");
    }

    addon::Json_value object = addon::Json_value::object();
    object.set("type", addon::Json_value("cancel"));
    object.set("note", addon::Json_value("line\nbreak"));
    const std::string encoded = addon::encode_frame(object);
    ok &= check(encoded == "{\"type\":\"cancel\",\"note\":\"line\\nbreak\"}\n", id, "encode_frame escapes and terminates");
    ok &= check(encoded.find('\n') == encoded.size() - 1, id, "encoded frame contains exactly one raw newline");
    return ok;
}

bool test_stderr_ring()
{
    bool ok = true;
    const char* id = __func__;

    addon::Stderr_ring ring(8);
    ring.append("abc");
    ok &= check(!ring.truncated() && ring.text() == "abc" && ring.total_bytes() == 3, id, "below capacity");
    ring.append("defgh");
    ok &= check(!ring.truncated() && ring.text() == "abcdefgh", id, "at capacity");
    ring.append("i");
    ok &= check(ring.truncated() && ring.text() == std::string(addon::limits::k_stderr_truncation_marker) + "bcdefghi", id,
        "one byte over keeps the tail and marks truncation");
    ring.append(std::string(20, 'z'));
    ok &= check(ring.text() == std::string(addon::limits::k_stderr_truncation_marker) + std::string(8, 'z') && ring.total_bytes() == 29, id,
        "large append keeps only the tail");

    addon::Stderr_ring defaults;
    defaults.append(std::string(addon::limits::k_stderr_ring_bytes, 'a'));
    ok &= check(!defaults.truncated(), id, "default capacity is k_stderr_ring_bytes");
    defaults.append("b");
    ok &= check(defaults.truncated() && defaults.text().size() ==
        std::string(addon::limits::k_stderr_truncation_marker).size() + addon::limits::k_stderr_ring_bytes, id,
        "default ring text is marker plus capacity");
    return ok;
}

} // namespace sdk_tests
