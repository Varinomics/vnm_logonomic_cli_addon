#pragma once

#include "logonomic_cli_addon/accounts.h"
#include "logonomic_cli_addon/host_simulator.h"
#include "logonomic_cli_addon/ids.h"
#include "logonomic_cli_addon/json.h"
#include "logonomic_cli_addon/usage_scan.h"

#include <cstdint>
#include <numeric>
#include <stdexcept>
#include <string>
#include <string_view>

namespace logonomic::cli_addon::test {

namespace detail {
inline void require_usage_scan(bool condition, std::string_view message)
{
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}
} // namespace detail

inline Host_outcome usage_scan_outcome(
    Host_simulator& host, const Usage_scan_request& input, std::uint16_t version)
{
    const auto id = host.invoke({"usage-scan", version}, "scan",
        encode_usage_scan_request(input, version), 30000);
    const auto outcome = host.wait_outcome(id, 31000);
    detail::require_usage_scan(outcome.has_value(), "packaged scan must finish");
    return *outcome;
}

// These assertions belong to every provider's packaged usage contract, including
// absent account context and the wire-level difference between usage2 and usage3.
inline Usage_scan_page usage_scan_page(
    Host_simulator& host, const Usage_scan_request& input, std::uint16_t version)
{
    using detail::require_usage_scan;
    const auto outcome = usage_scan_outcome(host, input, version);
    require_usage_scan(outcome.status == Host_status::SUCCESS,
        "packaged scan failed: " + outcome.reason_code + "; request=" +
            serialize_json(encode_usage_scan_request(input, version)));
    Contract_failure failure;
    const auto page = decode_usage_scan_result(outcome.payload, &failure, version);
    require_usage_scan(page.has_value(), failure.detail);
    if (version == 3) {
        require_usage_scan(page->profile_storage && input.context.profile_storage &&
            page->profile_storage->version == input.context.profile_storage->version &&
            page->profile_storage->root == input.context.profile_storage->root,
            "every usage3 page echoes the exact requested profile storage");
        require_usage_scan(page->account_context.has_value() == input.context.account_context.has_value(),
            "usage3 preserves account-context presence without synthesizing it");
        if (input.context.account_context) {
            require_usage_scan(encode_account_context(*page->account_context) ==
                encode_account_context(*input.context.account_context),
                "every usage3 page preserves account version, key and profile exactly");
        }
        else {
            require_usage_scan(!outcome.payload.find("account_context"),
                "absent account context is omitted on the wire, not null");
        }
    }
    else {
        require_usage_scan(!outcome.payload.find("account_context") && !outcome.payload.find("profile_storage"),
            "legacy usage pages do not acquire usage3 acknowledgments");
    }
    return *page;
}

// Keep the first page's snapshot/candidate metadata while collecting its records.
// The caller owns the host so restart and cancellation tests retain that control.
inline Usage_scan_page usage_scan_pages(
    Host_simulator& host, Usage_scan_request input, std::uint16_t version)
{
    auto first = usage_scan_page(host, input, version);
    auto page  = first;
    while (!page.complete) {
        input.snapshot_id = page.snapshot_id;
        input.cursor      = page.next_cursor;
        page = usage_scan_page(host, input, version);
        detail::require_usage_scan(page.cache_candidate == first.cache_candidate,
            "candidate descriptor stays identical across pages");
        first.records.insert(first.records.end(), page.records.begin(), page.records.end());
    }
    return first;
}

inline std::uint64_t usage_token_total(const Usage_scan_page& page)
{
    return std::accumulate(page.records.begin(), page.records.end(), std::uint64_t{0},
        [](std::uint64_t total, const Usage_record& record) { return total + record.total_tokens; });
}

} // namespace logonomic::cli_addon::test
