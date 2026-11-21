#include "logonomic_cli_addon_process_drain.h"

#include "logonomic_cli_addon/frame.h"

namespace logonomic::cli_addon_host::process {

// ---------------------------------------------------------------------------
// Stdout_frame_drain

struct Stdout_frame_drain::Impl
{
    cli_addon::Frame_decoder decoder;
    std::string              reason;

    explicit Impl(std::size_t max_frame_bytes)
    :
        decoder(max_frame_bytes)
    {}

    Outcome collect(bool accepted)
    {
        Outcome outcome;
        std::string frame;
        while (decoder.next_frame(frame)) {
            outcome.frames.push_back(std::move(frame));
            frame.clear();
        }
        if (!accepted && reason.empty()) {
            reason            = cli_addon::frame_violation_reason_code(decoder.violation());
            outcome.violation = true;
            outcome.reason    = reason;
        }
        return outcome;
    }
};

Stdout_frame_drain::Stdout_frame_drain(std::size_t in_max_frame_bytes)
:
    m_impl(std::make_unique<Impl>(in_max_frame_bytes))
{}

Stdout_frame_drain::~Stdout_frame_drain() = default;

Stdout_frame_drain::Outcome Stdout_frame_drain::feed(const std::uint8_t* in_data, std::size_t in_size)
{
    if (violated()) {
        // Draining to the void: the bytes are consumed so the adapter's pipe
        // never fills, and nothing is decoded from a poisoned stream.
        return Outcome();
    }
    const bool accepted = m_impl->decoder.feed(
        std::string_view(reinterpret_cast<const char*>(in_data), in_size));
    return m_impl->collect(accepted);
}

Stdout_frame_drain::Outcome Stdout_frame_drain::finish()
{
    if (violated()) {
        return Outcome();
    }
    return m_impl->collect(m_impl->decoder.finish());
}

bool Stdout_frame_drain::violated() const
{
    return !m_impl->reason.empty();
}

} // namespace logonomic::cli_addon_host::process
