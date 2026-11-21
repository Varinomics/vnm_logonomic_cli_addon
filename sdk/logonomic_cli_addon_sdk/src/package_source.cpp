#include "logonomic_cli_addon/package_source.h"

#include <algorithm>

namespace logonomic::cli_addon {

bool Directory_package_source::cancellation_requested(Source_error* out_error) const
{
    if (!m_cancellation.stop_requested()) {
        return false;
    }
    if (out_error) {
        out_error->kind   = Source_error_kind::IO_ERROR;
        out_error->detail = "package access was cancelled";
    }
    return true;
}

std::string path_to_utf8(const std::filesystem::path& path)
{
    const std::u8string utf8 = path.u8string();
    return std::string(reinterpret_cast<const char*>(utf8.data()), utf8.size());
}

std::filesystem::path path_from_utf8(std::string_view utf8)
{
    return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size()));
}

std::string file_time_ticks_text(std::filesystem::file_time_type modified)
{
    auto ticks = modified.time_since_epoch().count();
    const bool negative = ticks < 0;
    std::string text;
    do {
        const auto digit = ticks % 10;
        // Negate only one digit, so the native signed minimum stays representable.
        text.push_back(static_cast<char>('0' + static_cast<int>(digit < 0 ? -digit : digit)));
        ticks /= 10;
    } while (ticks != 0);
    if (negative) {
        text.push_back('-');
    }
    std::reverse(text.begin(), text.end());
    return text;
}

} // namespace logonomic::cli_addon
