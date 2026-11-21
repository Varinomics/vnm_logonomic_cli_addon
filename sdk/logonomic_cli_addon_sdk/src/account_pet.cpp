#include "logonomic_cli_addon/account_pet.h"

#include "strict_json.h"

#include <cstddef>
#include <limits>
#include <utility>

namespace logonomic::cli_addon {
namespace {

void fail(Contract_failure* out_failure, const char* detail)
{
    if (out_failure) {
        out_failure->reason_code = "account_pet_invalid";
        out_failure->detail = detail;
    }
}

std::optional<Account_context> read_context(const Json_value& payload, Contract_failure* out_failure)
{
    const auto* value = payload.find(k_account_context_member);
    auto context = value ? decode_account_context(*value, out_failure) : std::nullopt;
    if (!context || context->version != 2) {
        fail(out_failure, "account pet requires an account_context with version 2");
        return std::nullopt;
    }
    return context;
}

bool filename_fragment(const std::string& value, std::size_t maximum)
{
    return !value.empty() && value.size() <= maximum && is_valid_utf8(value) &&
        value.find_first_of("/\\") == std::string::npos && value.find('\0') == std::string::npos;
}

bool directory_pattern(const std::string& value)
{
    if (value.empty() || value.size() > 1024) {
        return false;
    }
    std::size_t begin = 0;
    unsigned segments = 0;
    while (begin <= value.size()) {
        const auto end = value.find('/', begin);
        const auto segment = value.substr(begin, end == std::string::npos ? end : end - begin);
        if (!filename_fragment(segment, 256) || segment == "." || segment == ".." || ++segments > 4) {
            return false;
        }
        if (end == std::string::npos) {
            return true;
        }
        begin = end + 1;
    }
    return false;
}

} // namespace

bool decode_account_pet_declaration(const Json_value& payload, Contract_failure* out_failure)
{
    if (!payload.is_object() || !payload.as_object().empty()) {
        fail(out_failure, "account pet declaration must be an empty object");
        return false;
    }
    return true;
}

Json_value encode_account_pet_request(
    const Account_context& account_context,
    const std::string& normalization_directory,
    const std::string& home_directory)
{
    Json_value payload = Json_value::object();
    payload.set(k_account_context_member, encode_account_context(account_context));
    if (!normalization_directory.empty()) {
        payload.set("normalization_directory", Json_value(normalization_directory));
    }
    if (!home_directory.empty()) {
        payload.set("home_directory", Json_value(home_directory));
    }
    return payload;
}

std::optional<Account_pet_request> decode_account_pet_request(
    const Json_value& payload, Contract_failure* out_failure)
{
    if (!payload.is_object() || strict::unexpected_member(payload, {
            k_account_context_member, "normalization_directory", "home_directory"}))
    {
        fail(out_failure, "account pet request has invalid members");
        return std::nullopt;
    }
    auto context = read_context(payload, out_failure);
    if (!context) {
        return std::nullopt;
    }
    Account_pet_request request;
    request.account_context = std::move(*context);
    if (const auto* directory = payload.find("normalization_directory")) {
        if (!directory->is_string() || !strict::is_bounded_absolute_path(directory->as_string())) {
            fail(out_failure, "account pet normalization directory must be a bounded absolute path");
            return std::nullopt;
        }
        request.normalization_directory = directory->as_string();
    }
    if (const auto* directory = payload.find("home_directory")) {
        if (!directory->is_string() || !strict::is_bounded_absolute_path(directory->as_string())) {
            fail(out_failure, "account pet home directory must be a bounded absolute path");
            return std::nullopt;
        }
        request.home_directory = directory->as_string();
    }
    return request;
}

Json_value encode_account_pet_inspect_result(const Account_pet_inspect_result& result)
{
    Json_value payload = encode_account_pet_request(result.account_context);
    Json_value pet;
    if (result.pet) {
        const auto& source = *result.pet;
        pet = Json_value::object();
        pet.set("spritesheet_path", Json_value(source.spritesheet_path));
        pet.set("frame_width",  Json_value((std::int64_t)source.frame_width));
        pet.set("frame_height", Json_value((std::int64_t)source.frame_height));
        pet.set("columns", Json_value((std::int64_t)source.columns));
        pet.set("rows",    Json_value((std::int64_t)source.rows));
        Json_value frames    = Json_value::array();
        Json_value durations = Json_value::array();
        for (const auto frame : source.frames) {
            frames.push_back(Json_value((std::int64_t)frame));
        }
        for (const auto duration : source.frame_durations_ms) {
            durations.push_back(Json_value((std::int64_t)duration));
        }
        pet.set("frames", std::move(frames));
        pet.set("frame_durations_ms", std::move(durations));
    }
    payload.set("pet", std::move(pet));
    Json_value watch_paths = Json_value::array();
    for (const auto& path : result.watch_paths) {
        watch_paths.push_back(Json_value(path));
    }
    payload.set("watch_paths", std::move(watch_paths));
    if (!result.watch_discoveries.empty()) {
        Json_value discoveries = Json_value::array();
        for (const auto& discovery : result.watch_discoveries) {
            Json_value entry = Json_value::object();
            entry.set("directory", Json_value(discovery.directory));
            entry.set("relative_directory_pattern", Json_value(discovery.relative_directory_pattern));
            entry.set("name_prefix", Json_value(discovery.name_prefix));
            Json_value suffixes = Json_value::array();
            for (const auto& suffix : discovery.suffixes) {
                suffixes.push_back(Json_value(suffix));
            }
            entry.set("suffixes", std::move(suffixes));
            discoveries.push_back(std::move(entry));
        }
        payload.set("watch_discoveries", std::move(discoveries));
    }
    if (!result.unavailable_reason.empty()) {
        payload.set("unavailable_reason", Json_value(result.unavailable_reason));
    }
    return payload;
}

std::optional<Account_pet_inspect_result> decode_account_pet_inspect_result(
    const Json_value& payload, Contract_failure* out_failure)
{
    if (!payload.is_object() ||
        strict::unexpected_member(payload, {
            k_account_context_member, "pet", "watch_paths", "unavailable_reason", "watch_discoveries"}))
    {
        fail(out_failure, "account pet result has invalid members");
        return std::nullopt;
    }
    auto context = read_context(payload, out_failure);
    const auto* pet = payload.find("pet");
    const auto* paths = strict::member_of_type(payload, "watch_paths", Json_type::ARRAY);
    if (!context || !pet || !paths || paths->as_array().size() > 32) {
        fail(out_failure, "account pet result has invalid context, pet or watch paths");
        return std::nullopt;
    }
    Account_pet_inspect_result result;
    result.account_context = std::move(*context);
    if (const auto* unavailable = payload.find("unavailable_reason")) {
        if (!unavailable->is_string() || unavailable->as_string().empty() ||
            unavailable->as_string().size() > 4096 ||
            unavailable->as_string().find('\0') != std::string::npos ||
            !is_valid_utf8(unavailable->as_string()) || !pet->is_null())
        {
            fail(out_failure, "unavailable account pet must have a bounded reason and no sprite sheet");
            return std::nullopt;
        }
        result.unavailable_reason = unavailable->as_string();
    }
    for (const auto& path : paths->as_array()) {
        if (!path.is_string() || !strict::is_bounded_absolute_path(path.as_string())) {
            fail(out_failure, "account pet watch paths must be bounded absolute paths");
            return std::nullopt;
        }
        result.watch_paths.push_back(path.as_string());
    }
    if (const auto* discoveries = payload.find("watch_discoveries")) {
        if (!discoveries->is_array() || discoveries->as_array().size() > 8) {
            fail(out_failure, "account pet discovery inputs must be a bounded array");
            return std::nullopt;
        }
        for (const auto& entry : discoveries->as_array()) {
            if (!entry.is_object()) {
                fail(out_failure, "account pet discovery input must be an object");
                return std::nullopt;
            }
            const auto* directory = strict::string_member(entry, "directory");
            const auto* pattern = strict::string_member(entry, "relative_directory_pattern");
            const auto* prefix = strict::string_member(entry, "name_prefix");
            const auto* suffixes = strict::member_of_type(entry, "suffixes", Json_type::ARRAY);
            if (!entry.is_object() || strict::unexpected_member(entry, {
                    "directory", "relative_directory_pattern", "name_prefix", "suffixes"}) ||
                !directory || !strict::is_bounded_absolute_path(*directory) || !pattern || !directory_pattern(*pattern) ||
                !prefix || !filename_fragment(*prefix, 256) || !suffixes || suffixes->as_array().empty() ||
                suffixes->as_array().size() > 8)
            {
                fail(out_failure, "account pet discovery input is invalid");
                return std::nullopt;
            }
            Account_pet_watch_discovery discovery{*directory, *pattern, *prefix, {}};
            for (const auto& suffix : suffixes->as_array()) {
                if (!suffix.is_string() || !filename_fragment(suffix.as_string(), 32)) {
                    fail(out_failure, "account pet discovery suffix is invalid");
                    return std::nullopt;
                }
                discovery.suffixes.push_back(suffix.as_string());
            }
            result.watch_discoveries.push_back(std::move(discovery));
        }
    }
    if (pet->is_null()) {
        return result;
    }
    if (!pet->is_object() || strict::unexpected_member(*pet, {
            "spritesheet_path", "frame_width", "frame_height", "columns", "rows",
            "frames", "frame_durations_ms"}))
    {
        fail(out_failure, "account pet has invalid members");
        return std::nullopt;
    }
    const auto* path = strict::string_member(*pet, "spritesheet_path");
    const auto width   = strict::integer_member(*pet, "frame_width", 1, 4096);
    const auto height  = strict::integer_member(*pet, "frame_height", 1, 4096);
    const auto columns = strict::integer_member(*pet, "columns", 1, 256);
    const auto rows    = strict::integer_member(*pet, "rows", 1, 256);
    const auto* frames = strict::member_of_type(*pet, "frames", Json_type::ARRAY);
    const auto* durations = strict::member_of_type(*pet, "frame_durations_ms", Json_type::ARRAY);
    if (!path || !strict::is_bounded_absolute_path(*path) ||
        !width || !height || !columns || !rows || *columns * *rows > 256 ||
        *width * *height * *columns * *rows > 16 * 1024 * 1024 ||
        !frames || !durations || frames->as_array().empty() || frames->as_array().size() > 256 ||
        frames->as_array().size() != durations->as_array().size())
    {
        fail(out_failure, "account pet frame geometry or animation is invalid");
        return std::nullopt;
    }
    Account_pet decoded;
    decoded.spritesheet_path = *path;
    decoded.frame_width  = (std::uint32_t)*width;
    decoded.frame_height = (std::uint32_t)*height;
    decoded.columns      = (std::uint32_t)*columns;
    decoded.rows         = (std::uint32_t)*rows;
    for (std::size_t index = 0; index < frames->as_array().size(); ++index) {
        const auto& frame    = frames->as_array()[index];
        const auto& duration = durations->as_array()[index];
        if (!frame.is_integer() || frame.as_integer() < 0 || frame.as_integer() >= *columns * *rows ||
            !duration.is_integer() || duration.as_integer() < 17 ||
            duration.as_integer() > std::numeric_limits<int>::max())
        {
            fail(out_failure, "account pet frame index or duration is invalid");
            return std::nullopt;
        }
        decoded.frames.push_back((std::uint32_t)frame.as_integer());
        decoded.frame_durations_ms.push_back((std::uint32_t)duration.as_integer());
    }
    result.pet = std::move(decoded);
    return result;
}

} // namespace logonomic::cli_addon
