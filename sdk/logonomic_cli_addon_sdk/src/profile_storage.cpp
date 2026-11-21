#include "logonomic_cli_addon/profile_storage.h"
#include "logonomic_cli_addon/accounts.h"
#include "logonomic_cli_addon/adapter_runtime.h"
#include "logonomic_cli_addon/ids.h"
#include "logonomic_cli_addon/limits.h"
#include "logonomic_cli_addon/package_source.h"
#include "logonomic_cli_addon/sha256.h"
#include "native_io_path.h"
#include "strict_json.h"

#include <iterator>
#include <system_error>
#include <utility>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace logonomic::cli_addon {

namespace fs = std::filesystem;

namespace {

fs::path canonical_storage_base(const fs::path& absolute_base, std::error_code& error)
{
#ifdef _WIN32
    // weakly_canonical walks a missing UNC path through incomplete share names
    // and can lose the extended prefix when it opens a long existing ancestor.
    for (auto ancestor = absolute_base; ancestor.has_relative_path(); ancestor = ancestor.parent_path()) {
        const auto canonical = fs::canonical(native_io_path(ancestor), error);
        if (!error) {
            return ancestor == absolute_base
                ? canonical
                : (canonical / absolute_base.lexically_relative(ancestor)).lexically_normal();
        }
        if (error != std::errc::no_such_file_or_directory) {
            break;
        }
    }
#endif
    return fs::weakly_canonical(absolute_base, error);
}

bool is_link_like(const fs::path& path, const fs::file_status& status)
{
    if (fs::is_symlink(status)) {
        return true;
    }
#if defined(_WIN32)
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
           (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
#else
    (void)path;
    return false;
#endif
}

} // namespace

std::optional<std::string> expected_profile_storage_root(
    const fs::path&  profile_storage_root,
    std::string_view package_id,
    std::string*     out_error,
    std::optional<std::string_view> account_key)
{
    if (!is_valid_package_id(package_id)) {
        if (out_error != nullptr) {
            *out_error = "package_id is not a canonical package ID";
        }
        return std::nullopt;
    }
    if (account_key && !is_valid_adapter_account_key(*account_key)) {
        if (out_error != nullptr) {
            *out_error = "account key is outside the accounts contract";
        }
        return std::nullopt;
    }
    const std::string scope = account_key ? account_identity(package_id, *account_key) : "default";
    const fs::path package_segment  = path_from_utf8(package_id);
    const fs::path suffix           = package_segment / scope;
    const fs::path lexical_base     = profile_storage_root.lexically_normal();
    const fs::path lexical_root     = (lexical_base / suffix).lexically_normal();
    const fs::path lexical_relative = lexical_root.lexically_relative(lexical_base);
    if (package_segment.is_absolute() ||
        std::distance(package_segment.begin(), package_segment.end()) != 1 ||
        lexical_relative.empty() || lexical_relative.is_absolute() ||
        *lexical_relative.begin() == "..")
    {
        if (out_error != nullptr) {
            *out_error = "package_id is not a confined canonical package segment";
        }
        return std::nullopt;
    }

    std::error_code error;
    const fs::path absolute_base  = fs::absolute(profile_storage_root, error);
    const fs::path canonical_base = error
        ? fs::path{}
        : canonical_storage_base(absolute_base, error);
    if (error) {
        if (out_error != nullptr) {
            *out_error = "cannot resolve " + path_to_utf8(profile_storage_root) + ": " + error.message();
        }
        return std::nullopt;
    }
    const fs::path package_root = canonical_base / package_segment;
    const fs::path root         = package_root / scope;
    for (const fs::path& directory : {package_root, root}) {
        error.clear();
        const auto io_path = native_io_path(directory);
        const auto status  = fs::symlink_status(io_path, error);
        if ((error && error != std::errc::no_such_file_or_directory) ||
            (fs::exists(status) && (is_link_like(io_path, status) || !fs::is_directory(status))))
        {
            if (out_error != nullptr) {
                *out_error = path_to_utf8(directory) + " is not an owned directory";
            }
            return std::nullopt;
        }
    }
    return path_to_utf8(root);
}

std::string profile_storage_provenance_account_id(std::string_view package_id)
{
    Sha256 digest;
    for (const std::string_view member : {
        package_id, std::string_view("host-profile-scope"), std::string_view("default")})
    {
        digest.update(std::to_string(member.size()));
        digest.update(":");
        digest.update(member);
    }
    return "a" + to_hex(digest.finish());
}

std::optional<std::string> allocate_profile_storage_root(
    const fs::path&  profile_storage_root,
    std::string_view package_id,
    std::string*     out_error,
    std::optional<std::string_view> account_key)
{
    const auto expected = expected_profile_storage_root(profile_storage_root, package_id, out_error, account_key);
    if (!expected) {
        return std::nullopt;
    }
    const fs::path root           = path_from_utf8(*expected);
    const fs::path package_root   = root.parent_path();
    const fs::path canonical_base = package_root.parent_path();
    std::error_code error;
    const auto base_io_path = native_io_path(canonical_base);
    fs::create_directories(base_io_path, error);
    if (error) {
        if (out_error != nullptr) {
            *out_error = "cannot create " + path_to_utf8(canonical_base) + ": " + error.message();
        }
        return std::nullopt;
    }
    for (const fs::path& directory : {package_root, root}) {
        error.clear();
        const auto io_path = native_io_path(directory);
        const auto before  = fs::symlink_status(io_path, error);
        if (error && error != std::errc::no_such_file_or_directory) {
            if (out_error != nullptr) {
                *out_error = "cannot inspect " + path_to_utf8(directory) + ": " + error.message();
            }
            return std::nullopt;
        }
        error.clear();
        if (fs::exists(before) && (is_link_like(io_path, before) || !fs::is_directory(before))) {
            if (out_error != nullptr) {
                *out_error = path_to_utf8(directory) + " is not an owned directory";
            }
            return std::nullopt;
        }
        if (!fs::exists(before) && !fs::create_directory(io_path, error)) {
            if (out_error != nullptr) {
                *out_error = "cannot create " + path_to_utf8(directory) + ": " + error.message();
            }
            return std::nullopt;
        }
        if (error) {
            if (out_error != nullptr) {
                *out_error = "cannot create " + path_to_utf8(directory) + ": " + error.message();
            }
            return std::nullopt;
        }
        const fs::file_status after = fs::symlink_status(io_path, error);
        if (error || is_link_like(io_path, after) || !fs::is_directory(after)) {
            if (out_error != nullptr) {
                *out_error = path_to_utf8(directory) + " did not resolve to an owned directory";
            }
            return std::nullopt;
        }
#if !defined(_WIN32)
        if (!fs::exists(before)) {
            fs::permissions(directory, fs::perms::owner_all, fs::perm_options::replace, error);
            if (error) {
                if (out_error != nullptr) {
                    *out_error = "cannot initialize " + path_to_utf8(directory) + ": " + error.message();
                }
                return std::nullopt;
            }
        }
#endif
    }

    const fs::path canonical = fs::canonical(native_io_path(root), error);
    const fs::path relative = error
        ? fs::path{}
        : fs::relative(native_io_path(canonical), base_io_path, error);
    if (error || relative.empty() || relative.is_absolute() || *relative.begin() == "..") {
        if (out_error != nullptr) {
            *out_error = "profile-storage path escaped its host-owned root";
        }
        return std::nullopt;
    }
    return path_to_utf8(canonical);
}

Json_value profile_storage_member(const Profile_storage_context& context)
{
    Json_value member = Json_value::object();
    member.set("version", Json_value((std::int64_t)context.version));
    member.set("root",    Json_value(context.root));
    return member;
}

std::optional<Profile_storage_context> decode_profile_storage_context(
    const Json_value& value,
    std::string*      out_detail)
{
    if (!value.is_object() || strict::unexpected_member(value, {"version", "root"}).has_value()) {
        *out_detail = "profile_storage is not an object of version and root";
        return std::nullopt;
    }
    const std::optional<std::int64_t> version =
        strict::integer_member(value, "version", 0, limits::k_max_version);
    const std::string* root = strict::string_member(value, "root");
    if (!version.has_value() || root == nullptr || root->empty() || root->find('\0') != std::string::npos ||
        !is_valid_utf8(*root)) {
        *out_detail = "profile_storage version or root is missing, empty or mistyped";
        return std::nullopt;
    }
    return Profile_storage_context{(std::uint16_t)*version, *root};
}

std::optional<std::string> profile_storage_acknowledgment_failure(
    const Profile_storage_context& context,
    const Json_value&              result_payload)
{
    const Json_value* member = result_payload.find(k_profile_storage_member);
    if (member == nullptr || !member->is_object()) {
        return reason::k_profile_storage_unacknowledged;
    }
    const Json_value* version = member->find("version");
    if (version == nullptr || !version->is_integer() || version->as_integer() != (std::int64_t)context.version) {
        return reason::k_profile_storage_version_mismatch;
    }
    const Json_value* root = member->find("root");
    if (root == nullptr || !root->is_string() || root->as_string() != context.root) {
        return reason::k_profile_storage_mismatch;
    }
    return std::nullopt;
}

void acknowledge_profile_storage(Invocation_context& context)
{
    const Json_value& payload = context.request().payload;
    const Json_value* member  = payload.find(k_profile_storage_member);
    std::string detail;
    const auto storage = member ? decode_profile_storage_context(*member, &detail) : std::nullopt;
    if (!storage || !payload.is_object() || payload.as_object().size() != 1) {
        context.fail(Result_status::FAILED, reason::k_profile_storage_mismatch);
        return;
    }
    Json_value result = Json_value::object();
    result.set(k_profile_storage_member, profile_storage_member(*storage));
    context.succeed(std::move(result));
}

} // namespace logonomic::cli_addon
