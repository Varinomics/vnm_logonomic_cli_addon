#pragma once

#include "logonomic_cli_addon/accounts.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace logonomic::cli_addon {

/// An account-owned local sprite sheet and the animation used for its badge.
struct Account_pet
{
    std::string                spritesheet_path;
    std::uint32_t              frame_width = 0;
    std::uint32_t              frame_height = 0;
    std::uint32_t              columns = 0;
    std::uint32_t              rows = 0;
    std::vector<std::uint32_t> frames;
    std::vector<std::uint32_t> frame_durations_ms;
};

struct Account_pet_watch_discovery
{
    std::string directory;
    std::string relative_directory_pattern;
    std::string name_prefix;
    std::vector<std::string> suffixes;
};

struct Account_pet_inspect_result
{
    Account_context             account_context;
    std::optional<Account_pet> pet;
    std::vector<std::string>    watch_paths;
    std::string                 unavailable_reason;
    std::vector<Account_pet_watch_discovery> watch_discoveries;
};

struct Account_pet_request
{
    Account_context account_context;
    std::string     normalization_directory;
    std::string     home_directory;
};

bool decode_account_pet_declaration(const Json_value& payload, Contract_failure* out_failure);
Json_value encode_account_pet_request(
    const Account_context& account_context,
    const std::string& normalization_directory = {},
    const std::string& home_directory = {});
std::optional<Account_pet_request> decode_account_pet_request(
    const Json_value& payload, Contract_failure* out_failure);
Json_value encode_account_pet_inspect_result(const Account_pet_inspect_result& result);
std::optional<Account_pet_inspect_result> decode_account_pet_inspect_result(
    const Json_value& payload, Contract_failure* out_failure);

} // namespace logonomic::cli_addon
