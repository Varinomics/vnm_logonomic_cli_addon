#pragma once

#include <vnm_process_custody/process_pipe.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace logonomic::cli_addon_host::process {

inline constexpr std::size_t k_stream_drain_chunk_bytes = 64 * 1024;
inline constexpr std::size_t k_stream_drain_turn_chunks = 4;

// Yield without closing the stream or consuming unread bytes. The consumer
// owns framing and terminal status handling; false ends this turn. A shared
// post-exit deadline must not be restarted by a continuously readable stream.
template<typename Reader, typename Drain_deadline, typename Consumer>
void drain_stream_turn(Reader& in_reader, const Drain_deadline& in_deadline, Consumer&& in_consume)
{
    std::uint8_t chunk[k_stream_drain_chunk_bytes];
    for (std::size_t chunks = 0;
         chunks < k_stream_drain_turn_chunks && in_reader.valid() && !in_deadline.expired();
         ++chunks)
    {
        const vnm::process_custody::Io_result read = in_reader.read(chunk, sizeof(chunk));
        if (!in_consume(read, chunk) || read.status != vnm::process_custody::Io_status::TRANSFERRED || read.bytes == 0) {
            return;
        }
    }
}

/// Continuous stdout drain: every byte read from the adapter's stdout goes
/// through the SDK frame decoder, complete frames come out, and the first
/// framing violation (oversize frame, invalid UTF-8, bare bytes) is reported
/// once. After a violation the drain keeps accepting bytes and discards
/// them, so the adapter can never block on a full pipe.
class Stdout_frame_drain
{
public:
    struct Outcome
    {
        std::vector<std::string> frames;      // complete frames, newline excluded
        bool                     violation = false;
        std::string              reason;      // reason code of the first violation
    };

    explicit Stdout_frame_drain(std::size_t in_max_frame_bytes);
    ~Stdout_frame_drain();

    Stdout_frame_drain(const Stdout_frame_drain&)            = delete;
    Stdout_frame_drain& operator=(const Stdout_frame_drain&) = delete;

    Outcome feed(const std::uint8_t* in_data, std::size_t in_size);

    /// stdout reached EOF: bytes of an unterminated frame are a violation.
    Outcome finish();

    bool violated() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace logonomic::cli_addon_host::process
