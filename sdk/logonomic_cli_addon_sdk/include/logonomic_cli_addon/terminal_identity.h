#pragma once

#include "logonomic_cli_addon/ids.h"
#include "logonomic_cli_addon/json.h"

#include <cstddef>
#include <optional>
#include <string>

namespace logonomic::cli_addon {

inline constexpr const char* k_terminal_identity_member = "terminal_identity";
inline constexpr std::size_t k_max_terminal_identity_context_bytes = 16384;
inline constexpr std::size_t k_max_terminal_identity_event_bytes = 16384;
inline constexpr std::size_t k_max_terminal_identity_title_bytes = 1024;

struct Terminal_identity_offer
{};

/// The provider returns this only after configuring a qualified native title.
/// Context belongs to that prepared launch, not the helper event's author.
struct Terminal_identity_preparation
{
    Json_value context = Json_value::object();
};

/// The terminal owner supplies the latest scoped native title to classifiers.
/// A missing event means no native title has been observed for this launch.
struct Terminal_identity_context
{
    Terminal_identity_preparation preparation;
    std::optional<Json_value> terminal_event;
};

struct Terminal_identity_observation
{
    std::string provider_session_id;
    std::string title;
};

Json_value encode_terminal_identity_offer(const Terminal_identity_offer& offer = {});
std::optional<Terminal_identity_offer> decode_terminal_identity_offer(
    const Json_value& payload, Contract_failure* out_failure);
Json_value encode_terminal_identity_preparation(const Terminal_identity_preparation& preparation);
std::optional<Terminal_identity_preparation> decode_terminal_identity_preparation(
    const Json_value& payload, Contract_failure* out_failure);
Json_value encode_terminal_identity_context(const Terminal_identity_context& context);
std::optional<Terminal_identity_context> decode_terminal_identity_context(
    const Json_value& payload, Contract_failure* out_failure);
Json_value encode_terminal_identity_observation(const Terminal_identity_observation& observation);
std::optional<Terminal_identity_observation> decode_terminal_identity_observation(
    const Json_value& payload, Contract_failure* out_failure);

} // namespace logonomic::cli_addon
