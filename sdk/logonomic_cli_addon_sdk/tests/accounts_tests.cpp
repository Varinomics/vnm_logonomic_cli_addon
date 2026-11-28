#include "sdk_test_support.h"

#include "logonomic_cli_addon/accounts.h"
#include "logonomic_cli_addon/authoring_availability.h"
#include "logonomic_cli_addon/launch_preparation.h"
#include "logonomic_cli_addon/package_source.h"
#include "logonomic_cli_addon/presentation_record.h"
#include "logonomic_cli_addon/profile_storage.h"
#include "logonomic_cli_addon/session_contracts.h"

#ifdef _WIN32
#include <windows.h>
#endif

#include <cstdio>
#include <filesystem>
#include <stdexcept>
#include <string>

namespace sdk_tests {

#ifdef _WIN32
namespace {

bool set_directory_case(const std::filesystem::path& path, bool sensitive)
{
    const HANDLE directory = CreateFileW(path.c_str(), FILE_WRITE_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (directory == INVALID_HANDLE_VALUE) return false;
    FILE_CASE_SENSITIVE_INFO mode{};
    mode.Flags = sensitive ? FILE_CS_FLAG_CASE_SENSITIVE_DIR : 0;
    const bool changed = SetFileInformationByHandle(directory, FileCaseSensitiveInfo, &mode, sizeof(mode)) != 0;
    CloseHandle(directory);
    return changed;
}

} // namespace
#endif

bool test_accounts_contracts()
{
    namespace addon = logonomic::cli_addon;
    namespace fs    = std::filesystem;

    // Oracle: accounts@2's adopted opaque identity, optional real path and
    // strict result rules, independent of authentication and usage support.
    addon::Contract_failure failure;
    const auto root = make_temp_directory("accounts-contracts");
    const std::string path = addon::path_to_utf8(root);
    // Native profile identity applies the containing directory's lookup rules,
    // including for profiles which a launcher selects before they exist.
    const auto upper = root / "Work";
    const auto lower = root / "work";
    const auto identity = addon::native_account_profile_identity(upper);
    SDK_CHECK(identity && addon::same_native_account_profile(upper, upper), "a profile retains its exact spelling");
    SDK_CHECK(!addon::same_native_account_profile(upper, root / "other"), "two missing profiles cannot rebind one key");
#ifdef _WIN32
    SDK_CHECK(identity == addon::native_account_profile_identity(lower) && addon::same_native_account_profile(upper, lower),
        "a missing leaf in an insensitive directory accepts its native spelling alias");
    SDK_CHECK(!addon::same_native_account_profile(upper / "A", lower / "a"),
        "a missing parent cannot establish that two different child spellings identify one grant");
    SDK_CHECK(addon::same_native_account_profile(upper / "A", lower / "A") &&
        !addon::historical_account_profile_identity(upper / "A", lower / "A"),
        "an observed spelling alias preserves an identical missing suffix without validating an older folded key");
    SDK_CHECK(addon::account_profile_allows_key_only(upper) && addon::account_profile_allows_key_only(upper / "123"),
        "an observed parent or an uncased suffix leaves an ordinary matching key unambiguous");
    fs::create_directory(upper);
    SDK_CHECK(identity == addon::native_account_profile_identity(lower) && addon::same_native_account_profile(upper, lower),
        "creating an insensitive directory preserves its pre-creation identity");
    fs::remove(upper);
    SDK_CHECK(identity == addon::native_account_profile_identity(lower) && addon::same_native_account_profile(upper, lower),
        "removing an insensitive leaf preserves its original grant");

    const auto deseret_upper = root / L"Test\U00010400";
    const auto deseret_lower = root / L"test\U00010428";
    const auto capital_key   = addon::native_account_profile_identity(deseret_upper);
    const auto small_key     = addon::native_account_profile_identity(deseret_lower);
    SDK_CHECK(capital_key && small_key && capital_key != small_key &&
        capital_key->starts_with('\0') && small_key->starts_with('\0') &&
        addon::native_account_profile_identity(root / L"TEST\U00010400") == capital_key,
        "native-distinct Unicode case peers have separate keys even before creation; surrounding ASCII still folds");
    fs::create_directory(deseret_upper);
    fs::create_directory(deseret_lower);
    SDK_CHECK(!fs::equivalent(deseret_upper, deseret_lower) && !addon::same_native_account_profile(deseret_upper, deseret_lower),
        "the two native Unicode directory entries have distinct file identities and grants");
    const auto original = addon::historical_account_profile_identity(deseret_upper, deseret_upper);
    SDK_CHECK(original && original == addon::historical_account_profile_identity(deseret_lower, deseret_lower) &&
        original != capital_key && original != small_key &&
        !addon::historical_account_profile_identity(deseret_upper, deseret_lower),
        "a former folding collision can authorize only the native source identified by its original path-bearing grant");
    for (const auto& peer : {deseret_upper, deseret_lower}) {
        SDK_CHECK(addon::native_account_profile_identity(peer, addon::Account_profile_case_fold::ASCII) ==
            addon::historical_account_profile_identity(peer, peer, addon::Account_profile_case_fold::ASCII),
            "providers with ASCII-only folding retain their previously distinct Unicode keys");
    }

    const DWORD drives = GetLogicalDrives();
    for (wchar_t letter = L'Z'; drives && letter >= L'A'; --letter) {
        if ((drives & (1u << (letter - L'A'))) != 0) continue;
        const fs::path drive(std::wstring(1, letter) + L":\\");
        const auto unknown = addon::native_account_profile_identity(drive / "Work");
        SDK_CHECK(unknown && unknown != addon::native_account_profile_identity(drive / "work") &&
            addon::account_profile_allows_key_only(drive / "Work") &&
            !addon::same_native_account_profile(drive / "Work", drive / "work") &&
            !addon::historical_account_profile_identity(drive / "Work", drive / "Work") &&
            !addon::historical_account_profile_identity(drive, drive),
            "an unmounted drive permits exact spelling without authorizing an unverified alias");
        break;
    }

    const auto case_root = root / "sensitive";
    fs::create_directory(case_root);
    if (set_directory_case(case_root, true)) {
        const auto mixed = case_root / "Work";
        const auto plain = case_root / "work";
        const auto a = addon::native_account_profile_identity(mixed);
        const auto b = addon::native_account_profile_identity(plain);
        SDK_CHECK(a && b && a != b && a->starts_with('\0') && b->starts_with('\0'),
            "both case-sensitive spellings stay outside the previously shared folded key domain");
        SDK_CHECK(!addon::same_native_account_profile(mixed, plain), "missing siblings under case-sensitive lookup are distinct");
        SDK_CHECK(addon::account_profile_allows_key_only(case_root / "missing" / "Work"),
            "a prospective key in the separated sensitive domain cannot be an older folded key");
        fs::create_directory(mixed);
        SDK_CHECK(!addon::same_native_account_profile(mixed, plain), "one existing sensitive sibling cannot supply the other");
        fs::create_directory(plain);
        SDK_CHECK(!fs::equivalent(mixed, plain) && !addon::same_native_account_profile(mixed, plain),
            "two existing case-sensitive sibling directories remain separate grants");
        const auto old_key = addon::historical_account_profile_identity(mixed, mixed);
        SDK_CHECK(old_key && old_key == addon::historical_account_profile_identity(plain, plain) &&
            !addon::historical_account_profile_identity(mixed, plain),
            "a former insensitive key needs its exact granted source to select a case-sensitive profile");
        fs::remove(mixed);
        fs::remove(plain);

        const auto branch = case_root / "Branch";
        const auto nested = branch / "nested";
        const auto work   = nested / "Work";
        const auto peer   = nested / "work";
        fs::create_directory(branch);
        SDK_CHECK(set_directory_case(branch, false), "the observed outer branch uses insensitive lookup");
        fs::create_directory(nested);
        SDK_CHECK(set_directory_case(nested, false), "the nested profile parent starts insensitive");
        fs::create_directory(work);
        const auto single = addon::native_account_profile_identity(work);
        SDK_CHECK(single && single == addon::native_account_profile_identity(peer),
            "the current key covers both verified spellings of the single real directory");
        fs::remove(work);
        fs::remove(nested);
        const auto pending_work = addon::native_account_profile_identity(work);
        const auto pending_peer = addon::native_account_profile_identity(peer);
        SDK_CHECK(pending_work && pending_peer && pending_work != pending_peer && pending_work != single && pending_peer != single,
            "a separated key cannot transfer its verified-insensitive edge to an inferred suffix after the parent disappears");
        fs::create_directory(nested);
        SDK_CHECK(set_directory_case(nested, true), "the recreated inner directory has sensitive lookup");
        fs::create_directory(work);
        fs::create_directory(peer);
        const auto new_work = addon::native_account_profile_identity(work);
        const auto new_peer = addon::native_account_profile_identity(peer);
        SDK_CHECK(!fs::equivalent(work, peer) && new_work && new_peer && new_work != new_peer &&
            new_work != single && new_peer != single && new_work != pending_work && new_peer != pending_peer,
            "recreating a containing directory with a different case policy cannot redirect the previous current key");
        fs::remove(work);
        fs::remove(peer);
        fs::remove(nested);
        SDK_CHECK(addon::native_account_profile_identity(work) == pending_work &&
            addon::native_account_profile_identity(peer) == pending_peer,
            "removing the sensitive parent restores the exact prospective identities for its missing children");
        fs::create_directory(nested);
        SDK_CHECK(set_directory_case(nested, false) && addon::native_account_profile_identity(work) == single &&
            addon::native_account_profile_identity(peer) == single,
            "reinstating insensitive lookup restores only its own previously separated identity");
        fs::remove(nested);
        fs::remove(branch);

        fs::remove(case_root);
        SDK_CHECK(old_key == addon::native_account_profile_identity(plain) && !addon::account_profile_allows_key_only(plain) &&
            !addon::same_native_account_profile(mixed, plain) && !addon::historical_account_profile_identity(mixed, mixed),
            "removing a case-sensitive parent cannot authorize switching a retained grant to its other spelling");
    }
    else {
        std::fputs("Native account profile check: the temporary volume did not enable per-directory case sensitivity.\n", stderr);
    }
#else
    SDK_CHECK(identity != addon::native_account_profile_identity(lower) && !addon::same_native_account_profile(upper, lower),
        "non-Windows profile keys retain their native path bytes");
#endif

    const addon::Account_record first{"Primary", "Primary Account", addon::Account_status::AVAILABLE, std::nullopt};
    const addon::Account_record second{"primary", "Filesystem Account", addon::Account_status::UNKNOWN, path};
    const auto encoded = addon::encode_accounts({first, second});
    const auto accounts = addon::decode_accounts_result(encoded, &failure);
    SDK_CHECK(accounts && accounts->size() == 2 && !accounts->front().profile_path &&
        accounts->back().profile_path == path, "case-distinct accounts preserve a truly optional real profile path");
    SDK_CHECK(!encoded.find("accounts")->as_array().front().find("profile_path"),
        "an absent profile path is omitted from the account record");

    auto invalid = addon::encode_accounts({first, first});
    SDK_CHECK(!addon::decode_accounts_result(invalid, &failure), "duplicate account keys invalidate the whole result");
    for (const auto& field : {std::string("password"), std::string("token"), std::string("credentials")}) {
        invalid = encoded;
        invalid.find("accounts")->as_array().front().set(field, addon::Json_value("private-fixture-value"));
        SDK_CHECK(!addon::decode_accounts_result(invalid, &failure), "credentials are not account record fields");
        SDK_CHECK(failure.detail.find("private-fixture-value") == std::string::npos,
            "decoder diagnostics must not echo rejected account values");
    }
    for (const auto& malformed : {addon::Json_value(), addon::Json_value(""), addon::Json_value("relative/path")}) {
        invalid = encoded;
        invalid.find("accounts")->as_array().front().set("profile_path", malformed);
        SDK_CHECK(!addon::decode_accounts_result(invalid, &failure), "present profile paths must be real absolute paths");
    }
    invalid = encoded;
    invalid.find("accounts")->as_array().front().set("status", addon::Json_value("signed-in"));
    SDK_CHECK(!addon::decode_accounts_result(invalid, &failure), "account status is its own finite generic vocabulary");
    SDK_CHECK(!addon::is_valid_adapter_account_key(std::string("a\0b", 3)) &&
        !addon::is_valid_adapter_account_key(std::string(addon::limits::k_max_adapter_account_key_bytes + 1, 'x')),
        "opaque account keys remain bounded nonempty UTF-8 without NUL");

    const auto primary_id = addon::account_identity("org.example.accounts", "Primary");
    SDK_CHECK(primary_id != addon::account_identity("org.example.accounts", "primary") &&
        primary_id != addon::account_identity("org.example.sibling", "Primary") &&
        primary_id != addon::profile_storage_provenance_account_id("org.example.accounts"),
        "account identity separates key case, package ownership and historical default scopes");
    std::string error;
    const auto default_root = addon::allocate_profile_storage_root(root, "org.example.accounts", &error);
    const auto account_root = addon::allocate_profile_storage_root(root, "org.example.accounts", &error, "../Primary");
    const auto sibling_root = addon::allocate_profile_storage_root(root, "org.example.sibling", &error, "../Primary");
    SDK_CHECK(default_root && account_root && sibling_root && default_root != account_root && account_root != sibling_root,
        "account-scoped directories are isolated even when opaque keys contain path syntax");
    SDK_CHECK(fs::path(*account_root).parent_path() == fs::path(*default_root).parent_path(),
        "opaque path syntax cannot escape the package profile root");
    SDK_CHECK(addon::expected_profile_storage_root(root, "org.example.accounts", &error, "../Primary") == account_root,
        "allocation and historical scope validation agree");

#if !defined(_WIN32)
    const fs::path package_root = fs::path(*default_root).parent_path();
    SDK_CHECK((fs::status(package_root).permissions() & fs::perms::all) == fs::perms::owner_all &&
        (fs::status(*default_root).permissions() & fs::perms::all) == fs::perms::owner_all,
        "new package and profile directories default to owner access");
    fs::permissions(root, fs::perms::all, fs::perm_options::replace);
    fs::permissions(package_root, fs::perms::all, fs::perm_options::replace);
    fs::permissions(*default_root, fs::perms::all, fs::perm_options::replace);
    SDK_CHECK(addon::allocate_profile_storage_root(root, "org.example.accounts", &error) == default_root,
        "existing shared package and profile directories remain usable");
    SDK_CHECK((fs::status(root).permissions() & fs::perms::all) == fs::perms::all &&
        (fs::status(package_root).permissions() & fs::perms::all) == fs::perms::all &&
        (fs::status(*default_root).permissions() & fs::perms::all) == fs::perms::all,
        "allocation preserves user-selected permissions on every existing directory");
    const auto new_shared_account = addon::allocate_profile_storage_root(
        root, "org.example.accounts", &error, "Shared");
    SDK_CHECK(new_shared_account &&
        (fs::status(*new_shared_account).permissions() & fs::perms::all) == fs::perms::owner_all &&
        (fs::status(package_root).permissions() & fs::perms::all) == fs::perms::all,
        "a new profile gets its default permissions without changing the existing shared package directory");
#endif

    const auto resolution = addon::decode_account_resolution_result(addon::encode_account_resolution({"Primary"}), &failure);
    const auto absent     = addon::decode_account_resolution_result(addon::encode_account_resolution({}), &failure);
    SDK_CHECK(resolution && resolution->adapter_account_key == "Primary" && absent && !absent->adapter_account_key,
        "resolution separates a selected opaque key from no selected account");

    addon::Prepare_request prepare;
    prepare.launcher        = {"unknown-fixture-cli", addon::k_runner_direct, {}};
    prepare.launcher_argv = {"unknown-fixture-cli"};
    prepare.launch_environment = addon::Launch_environment{addon::host_platform_id(),
        addon::Json_value::object(), std::nullopt};
    prepare.account_context = addon::Account_context{1, "Primary"};
    const auto prepared = addon::decode_prepare_request(addon::encode_prepare_request(prepare), &failure);
    SDK_CHECK(prepared && prepared->account_context && prepared->account_context->adapter_account_key == "Primary",
        "launch preparation carries its independently versioned account context");
    addon::Launch_patch patch;
    patch.disposition = addon::Launch_disposition{true, false, false};
    SDK_CHECK(addon::account_context_acknowledgment_failure(*prepare.account_context, addon::encode_launch_patch(patch)).has_value(),
        "successful preparation cannot silently ignore account selection");
    patch.account_context = prepare.account_context;
    SDK_CHECK(!addon::account_context_acknowledgment_failure(*prepare.account_context, addon::encode_launch_patch(patch)),
        "exact selected-account acknowledgment succeeds");
    patch.account_context->adapter_account_key = "primary";
    SDK_CHECK(addon::account_context_acknowledgment_failure(*prepare.account_context, addon::encode_launch_patch(patch)).has_value(),
        "account acknowledgment compares exact case-sensitive bytes");
    auto unknown_context = addon::encode_account_context(*prepare.account_context);
    unknown_context.set("version", addon::Json_value(3));
    SDK_CHECK(!addon::decode_account_context(unknown_context, &failure),
        "unknown accounts context versions cannot prepare a selected account");
    prepare.account_context.reset();
    SDK_CHECK(addon::decode_prepare_request(addon::encode_prepare_request(prepare), &failure).has_value(),
        "account absence leaves ordinary preparation available");

    addon::Resume_prepare_request resume;
    resume.context.launcher        = prepare.launcher;
    resume.context.launcher_argv   = {"unknown-fixture-cli"};
    resume.context.launch_environment = prepare.launch_environment;
    resume.context.account_context = addon::Account_context{1, "Primary"};
    resume.context.profile_storage = addon::Profile_storage_context{1, *account_root};
    resume.resume_record.set("provider_session_id", addon::Json_value("session-history"));
    const auto historical = addon::decode_resume_prepare_request(addon::encode_resume_prepare_request(resume), &failure);
    SDK_CHECK(historical && historical->context.account_context &&
        historical->context.account_context->adapter_account_key == "Primary" &&
        historical->context.profile_storage->root == *account_root,
        "historical resume keeps the selected account separate from its exact managed profile root");

    addon::Accounts_launcher_request configured;
    configured.launcher = {"unknown-fixture-cli", addon::k_runner_direct, "PROFILE_HOME=/profiles/project"};
    configured.launch_environment.platform_id = "linux-x86_64";
    configured.launch_environment.effective_environment.set("PROFILE_HOME", addon::Json_value("/profiles/project"));
    configured.default_home_fallback = "/home/user";
    configured.normalization_directory = path;
    const auto selected = addon::decode_accounts_launcher_request(
        addon::encode_accounts_launcher_request(configured), &failure);
    SDK_CHECK(selected && selected->launch_environment.effective_environment.find("PROFILE_HOME")->as_string() ==
        "/profiles/project" && selected->launcher.environment_text == configured.launcher.environment_text,
        "accounts discovery and resolution share the configured profile rather than ambient-only inventory");
    invalid = addon::encode_accounts_launcher_request(configured);
    invalid.erase("launch_environment");
    SDK_CHECK(!addon::decode_accounts_launcher_request(invalid, &failure),
        "accounts launcher discovery requires its frozen environment");

    addon::Account_context located{2, "profile-sha256-key", path};
    auto check = addon::encode_authoring_availability_request({configured, located});
    const auto availability = addon::decode_authoring_availability_request(check, &failure);
    SDK_CHECK(availability && availability->account_context && availability->account_context->profile_path == path &&
        availability->launcher.normalization_directory == configured.normalization_directory &&
        !addon::decode_accounts_launcher_request(check, &failure), "eligibility carries a separate, scoped account grant");
    check.set("prompt", addon::Json_value("a model turn"));
    SDK_CHECK(!addon::decode_authoring_availability_request(check, &failure), "eligibility has no model prompt");
    const auto denied = addon::encode_authoring_availability_result({false, "Hooks are enabled.", located});
    const auto denial = addon::decode_authoring_availability_result(denied, located, &failure);
    SDK_CHECK(denial && !denial->available && denial->reason == "Hooks are enabled.", "profile-specific refusal");
    auto other = located;
    other.profile_path = path + "/other";
    SDK_CHECK(!addon::decode_authoring_availability_result(denied, other, &failure) &&
        !addon::decode_authoring_availability_result(denied, std::nullopt, &failure), "reject mismatched or invented grants");
    const auto allowed = addon::encode_authoring_availability_result({true, {}, std::nullopt});
    SDK_CHECK(addon::decode_authoring_availability_result(allowed, std::nullopt, &failure) &&
        !addon::decode_authoring_availability_result(allowed, located, &failure), "require the selected account on an eligible result");
    for (const std::string reason : {std::string{}, std::string("  "), std::string("bad\nreason"), std::string(1025, 'x')}) {
        const auto invalid_result = addon::encode_authoring_availability_result({false, reason, located});
        SDK_CHECK(!addon::decode_authoring_availability_result(invalid_result, located, &failure), "refusal requires a bounded single-line reason");
    }
    patch.account_context = located;
    SDK_CHECK(!addon::account_context_acknowledgment_failure(located, addon::encode_launch_patch(patch)),
        "profile locator acknowledgment preserves identity independent of the opaque key length");
    patch.account_context->profile_path = path + "/other";
    SDK_CHECK(addon::account_context_acknowledgment_failure(located, addon::encode_launch_patch(patch)).has_value(),
        "a matching opaque key cannot acknowledge a different canonical profile path");

    std::error_code cleanup_error;
    fs::remove_all(root, cleanup_error);
    SDK_CHECK(!cleanup_error, "account test directories are removed");
    return true;
}

bool test_account_display_names()
{
    namespace addon = logonomic::cli_addon;

    // Oracle: accounts@2 names an account with a filesystem profile after the
    // profile's folder, fitted to the presentation@1 display-name grammar.
#ifdef _WIN32
    const std::string home = "C:/Users/Jane Doe/";
    const std::string root = "C:/";
    const std::string root_name = "C:\\";
#else
    const std::string home = "/home/jane/";
    const std::string root = "/";
    const std::string root_name = "/";
#endif
    const std::string ellipsis = "\xE2\x80\xA6";
    const auto repeated = [](const std::string& text, int count) {
        std::string result;
        for (int index = 0; index < count; ++index) {
            result += text;
        }
        return result;
    };
    const auto conforms = [](const std::string& name) {
        std::string detail;
        return addon::normalized_display_name(name, &detail) == name;
    };

    SDK_CHECK(addon::account_display_name(home + ".example-cli") == ".example-cli",
        "the default profile is named after its folder");
    SDK_CHECK(addon::account_display_name(home + ".example-cli-work/") == ".example-cli-work",
        "a trailing separator does not hide the folder name");
    SDK_CHECK(addon::account_display_name(home + "profiles/../.example-cli-work/.") == ".example-cli-work",
        "dot components do not name the folder");
    SDK_CHECK(addon::account_display_name(root) == root_name, "a root profile is named by the whole path");
#ifdef _WIN32
    // A share is the root of a UNC path, although the filesystem library
    // parses \\server as the root name and the share as a relative component.
    SDK_CHECK(addon::account_display_name("\\\\server\\share\\") == "\\\\server\\share",
        "a share root with a trailing separator is named by its whole path");
    SDK_CHECK(addon::account_display_name("//server/share") == "\\\\server\\share",
        "a share root without a trailing separator is named by its whole path");
    SDK_CHECK(addon::account_display_name("\\\\server\\share\\.example-cli\\") == ".example-cli",
        "a profile below a share root is named after its folder");
#endif
    SDK_CHECK(addon::account_display_name(home + "work\tprofile") == "work\xEF\xBF\xBDprofile",
        "a control character in the folder name becomes U+FFFD");

    const std::string long_name = ".codex-account-for-the-varinomics-nightly-release-builds";
    const auto shortened = addon::account_display_name(home + long_name);
    SDK_CHECK(shortened == ".codex-account-for-t" + ellipsis + "htly-release-builds" && conforms(shortened),
        "an over-long folder name keeps its first 20 and last 19 characters around an ellipsis");
    const std::string wide = "\xE6\xA1\xA3";   // U+6863, three UTF-8 bytes
    const auto wide_name = addon::account_display_name(home + repeated(wide, 41));
    SDK_CHECK(wide_name == repeated(wide, 20) + ellipsis + repeated(wide, 19) && conforms(wide_name),
        "forty-one three-byte characters shorten to exactly the scalar and byte bounds");
    const std::string folder = "\xF0\x9F\x93\x81";   // U+1F4C1, four UTF-8 bytes
    const auto byte_bound = addon::account_display_name(home + repeated(folder, 35));
    SDK_CHECK(byte_bound == repeated(folder, 15) + ellipsis + repeated(folder, 14) && conforms(byte_bound),
        "four-byte characters shorten to the byte bound, the leading part the longer");

    bool refused = false;
    try {
        addon::account_display_name("");
    }
    catch (const std::invalid_argument&) {
        refused = true;
    }
    SDK_CHECK(refused, "an empty profile path names no folder");
    return true;
}

} // namespace sdk_tests
