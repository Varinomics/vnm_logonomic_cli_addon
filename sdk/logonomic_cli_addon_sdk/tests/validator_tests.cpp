#include "sdk_test_support.h"

#include "logonomic_cli_addon/bootstrap.h"
#include "logonomic_cli_addon/generation_digest.h"
#include "logonomic_cli_addon/ids.h"
#include "logonomic_cli_addon/integrity_index.h"
#include "logonomic_cli_addon/json.h"
#include "logonomic_cli_addon/limits.h"
#include "logonomic_cli_addon/package_source.h"
#include "logonomic_cli_addon/package_validator.h"
#include "logonomic_cli_addon/sha256.h"
#include "logonomic_cli_addon/sidecar.h"
#include "logonomic_cli_addon/version.h"

#include <algorithm>
#include <filesystem>
#include <functional>
#include <limits>
#include <map>
#include <string>
#include <system_error>
#include <vector>

namespace sdk_tests {

namespace {

namespace addon = logonomic::cli_addon;
namespace fs    = std::filesystem;

// A package described as files; the index is derived from them unless a
// test edits it afterwards.
struct Package_files
{
    addon::Bootstrap_manifest          bootstrap;
    std::map<std::string, std::string> files;   // relative path -> bytes
    std::map<std::string, std::pair<std::string, std::string>> third_party;   // resource -> (notice, provenance)
};

Package_files conforming_package()
{
    Package_files package;
    package.bootstrap.bootstrap_version         = 1;
    package.bootstrap.package_id                = "org.example.validator-fixture";
    package.bootstrap.release_version           = "2.3.4";
    package.bootstrap.platforms                 = {addon::k_platform_linux_x86_64, addon::k_platform_windows_x86_64};
    package.bootstrap.adapter_path              = {{addon::k_platform_linux_x86_64, "adapter/fixture"}, {addon::k_platform_windows_x86_64, "adapter/fixture.exe"}};
    package.bootstrap.adapter_argv              = {"--behavior", "conformant"};
    package.bootstrap.adapter_environment       = {{"FIXTURE_MODE", "validator"}};
    package.bootstrap.core_protocol_generations = {1};
    package.bootstrap.integrity_index           = addon::k_index_file_name;

    package.files["adapter/fixture"]     = fake_adapter_bytes();
    package.files["adapter/fixture.exe"] = fake_adapter_bytes();
    package.files[addon::sidecar_relative_path("profile-storage", 1)] =
        addon::serialize_sidecar(addon::Capability_sidecar{"profile-storage", 1, addon::Json_value::object()});
    package.files[addon::sidecar_relative_path("org.example.unadopted", 7)] =
        addon::serialize_sidecar(addon::Capability_sidecar{"org.example.unadopted", 7, addon::Json_value("opaque")});
    package.files["assets/banner.txt"]         = "package-owned banner";
    package.files["assets/mark.svg"]           = "<svg/>";
    package.files["notices/mark-license.txt"]  = "MIT notice for the mark";
    package.files["provenance/mark.json"]      = R"({"origin":"https://example.org/mark.svg"})";
    package.files["helpers/probe-helper"]      = "#!/bin/sh\necho helper\n";
    package.third_party["assets/mark.svg"]     = {"notices/mark-license.txt", "provenance/mark.json"};
    return package;
}

addon::Integrity_index index_of(const Package_files& package)
{
    addon::Integrity_index index;
    index.index_version = addon::k_index_version;
    for (const auto& [path, bytes] : package.files) {
        addon::Index_entry entry;
        entry.path       = path;
        entry.bytes      = bytes.size();
        entry.sha256_hex = addon::to_hex(addon::Sha256::digest(bytes));
        entry.kind       = addon::resource_kind_for_path(path).value_or(addon::Resource_kind::ASSET);
        for (const auto& [resource, records] : package.third_party) {
            if (path == resource || path == records.first || path == records.second) {
                entry.origin     = addon::Resource_origin::THIRD_PARTY;
                entry.notice     = records.first;
                entry.provenance = records.second;
            }
        }
        index.files.push_back(entry);
    }
    return index;
}

// Writes the package to a fresh directory; `index_text` overrides the
// derived index when given.
fs::path materialize(const Package_files& package, std::optional<std::string> index_text = std::nullopt, bool write_bootstrap = true)
{
    const fs::path root = make_temp_directory("validator");
    if (write_bootstrap) {
        write_file(root / addon::k_bootstrap_file_name, addon::serialize_bootstrap(package.bootstrap));
    }
    for (const auto& [path, bytes] : package.files) {
        write_file(root / addon::path_from_utf8(path), bytes);
    }
    write_file(root / addon::k_index_file_name, index_text.value_or(addon::serialize_integrity_index(index_of(package))));
    return root;
}

// The byte fixtures declare Linux/Windows images independently of the host.
std::string validate_reason(
    const fs::path& root,
    const addon::Validation_options& options = {addon::k_platform_linux_x86_64, {1}})
{
    addon::Directory_package_source source(root);
    addon::Package_validator validator(options);
    addon::Contract_failure failure;
    const std::optional<addon::Validated_package> package = validator.validate(source, &failure);
    return package.has_value() ? std::string("ok") : failure.reason_code;
}

void remove_tree(const fs::path& root)
{
    std::error_code error;
    fs::remove_all(root, error);
}

} // namespace

bool test_validator_accepts_a_conforming_package()
{
    bool ok = true;
    const char* id = __func__;

    const Package_files package = conforming_package();
    const fs::path root = materialize(package);
    addon::Directory_package_source source(root);
    addon::Package_validator validator(addon::Validation_options{addon::k_platform_linux_x86_64, {1}});
    addon::Contract_failure failure;
    const std::optional<addon::Validated_package> validated = validator.validate(source, &failure);
    if (!check(validated.has_value(), id, "conforming package validates: " + failure.reason_code + " " + failure.detail)) {
        remove_tree(root);
        return false;
    }

    ok &= check(validated->bootstrap.package_id == "org.example.validator-fixture", id, "bootstrap read");
    ok &= check(validated->adapter_relative_path == "adapter/fixture", id, "platform adapter selected");
    ok &= check(validated->index.files.size() == package.files.size(), id, "every file indexed");
    ok &= check(validated->total_bytes == validated->index.total_bytes(), id, "total bytes from the index");
    const std::vector<addon::Capability_pair> declared = validated->declared_capabilities();
    ok &= check(declared.size() == 2 && declared[0] == addon::Capability_pair{"org.example.unadopted", 7} &&
        declared[1] == addon::Capability_pair{"profile-storage", 1}, id, "declared capabilities in index order");
    ok &= check(validated->generation_digest_hex.size() == 64, id, "generation digest rendered");
    ok &= check(validated->generation_digest_hex == addon::compute_generation_digest_hex(validated->bootstrap_bytes, validated->index_bytes, validated->index), id,
        "generation digest matches the definition over the bytes read");

    // The Windows platform selects the other adapter path.
    addon::Package_validator windows_validator(addon::Validation_options{addon::k_platform_windows_x86_64, {1}});
    const std::optional<addon::Validated_package> windows_package = windows_validator.validate(source, &failure);
    ok &= check(windows_package.has_value() && windows_package->adapter_relative_path == "adapter/fixture.exe", id, "windows adapter selected");
    ok &= check(windows_package.has_value() && windows_package->generation_digest_hex == validated->generation_digest_hex, id,
        "generation digest is platform independent");

    // Discovery reads the bootstrap alone.
    const std::optional<addon::Bootstrap_manifest> bootstrap = validator.read_bootstrap(source, &failure);
    ok &= check(bootstrap.has_value() && bootstrap->package_id == "org.example.validator-fixture", id, "read_bootstrap");

    // Resource classification.
    const addon::Index_entry* mark = validated->index.find("assets/mark.svg");
    ok &= check(mark != nullptr && mark->origin == addon::Resource_origin::THIRD_PARTY && mark->notice == "notices/mark-license.txt", id,
        "third-party classification retained");
    const addon::Index_entry* banner = validated->index.find("assets/banner.txt");
    ok &= check(banner != nullptr && banner->origin == addon::Resource_origin::PACKAGE && banner->kind == addon::Resource_kind::ASSET, id,
        "package-owned classification retained");

    // Determinism: a second validation yields the same digest.
    const std::optional<addon::Validated_package> again = validator.validate(source, &failure);
    ok &= check(again.has_value() && again->generation_digest_hex == validated->generation_digest_hex, id, "digest is stable");

    remove_tree(root);
    return ok;
}

bool test_macos_adapter_executable_format()
{
    // mach_header_64: MH_MAGIC_64, CPU_TYPE_ARM64, ARM64_ALL, MH_EXECUTE.
    std::string image(32, '\0');
    image.replace(0, 4, "\xcf\xfa\xed\xfe", 4);
    image.replace(4, 4, "\x0c\0\0\x01", 4);
    image[12] = 2;
    SDK_CHECK(addon::is_macos_arm64_executable(image), "ARM64 Mach-O executable admitted");
    auto wrong = image;
    wrong[12] = 6;
    SDK_CHECK(!addon::is_macos_arm64_executable(wrong), "Mach-O dynamic library is not an adapter");
    wrong = image;
    wrong[4] = 7;
    SDK_CHECK(!addon::is_macos_arm64_executable(wrong), "x86_64 image cannot claim macos-arm64");
    SDK_CHECK(!addon::is_macos_arm64_executable(image.substr(0, 31)), "truncated header refused");
    SDK_CHECK(!addon::is_macos_arm64_executable(fake_adapter_bytes()), "ELF cannot claim macos-arm64");
    std::string fat(28, '\0');
    fat.replace(0, 4, "\xca\xfe\xba\xbe", 4);
    fat[7] = 1;
    fat.replace(8, 4, "\x01\0\0\x0c", 4);
    fat[19] = 28;
    fat[23] = 32;
    fat += image;
    SDK_CHECK(addon::is_macos_arm64_executable(fat), "fat image carries an executable ARM64 slice");
    fat[19] = 60;
    SDK_CHECK(!addon::is_macos_arm64_executable(fat), "out-of-file native slice refused");
    auto package = conforming_package();
    package.bootstrap.platforms.push_back(addon::k_platform_macos_arm64);
    package.bootstrap.adapter_path.emplace_back(addon::k_platform_macos_arm64, "adapter/native-mac");
    package.files["adapter/native-mac"] = image;
    const auto root = materialize(package);
    addon::Directory_package_source source(root);
    addon::Contract_failure failure;
    addon::Package_validator validator({addon::k_platform_macos_arm64, {1}});
    const auto validated = validator.validate(source, &failure);
    remove_tree(root);
    SDK_CHECK(validated.has_value(), "Mac manifest and native-format validation agree: " + failure.detail);
    return true;
}

bool test_validator_reports_each_defect()
{
    bool ok = true;
    const char* id = __func__;

    struct Case
    {
        const char*                                         name;
        std::function<fs::path()>                           build;
        const char*                                         reason;
        addon::Validation_options                           options = {addon::k_platform_linux_x86_64, {1}};
    };

    const std::vector<Case> cases = {
        {"missing bootstrap", [] {
            return materialize(conforming_package(), std::nullopt, false);
        }, addon::reason::k_invalid_bootstrap},
        {"bootstrap not json", [] {
            const fs::path root = materialize(conforming_package());
            write_file(root / addon::k_bootstrap_file_name, "{not json");
            return root;
        }, addon::reason::k_invalid_bootstrap},
        {"bootstrap invalid utf-8", [] {
            const fs::path root = materialize(conforming_package());
            write_file(root / addon::k_bootstrap_file_name, "{\"bootstrap_version\":1,\"x\":\"\xFF\"}");
            return root;
        }, addon::reason::k_invalid_utf8},
        {"unsupported bootstrap version", [] {
            Package_files package = conforming_package();
            const fs::path root = materialize(package);
            std::string text = addon::serialize_bootstrap(package.bootstrap);
            text.replace(text.find("\"bootstrap_version\":1"), 21, "\"bootstrap_version\":2");
            write_file(root / addon::k_bootstrap_file_name, text);
            return root;
        }, addon::reason::k_unsupported_bootstrap_version},
        {"unexpected bootstrap member", [] {
            Package_files package = conforming_package();
            const fs::path root = materialize(package);
            std::string text = addon::serialize_bootstrap(package.bootstrap);
            text.insert(text.size() - 1, ",\"icon\":\"x\"");
            write_file(root / addon::k_bootstrap_file_name, text);
            return root;
        }, addon::reason::k_invalid_bootstrap},
        {"platform not supported", [] {
            return materialize(conforming_package());
        }, addon::reason::k_platform_not_supported, {"freebsd-x86_64", {1}}},
        {"unsupported core generation", [] {
            return materialize(conforming_package());
        }, addon::reason::k_unsupported_core_generation, {addon::k_platform_linux_x86_64, {2}}},
        {"reserved environment variable", [] {
            Package_files package = conforming_package();
            package.bootstrap.adapter_environment.emplace_back("LOGONOMIC_ADDON_SCRATCH_DIR", "/elsewhere");
            return materialize(package);
        }, addon::reason::k_reserved_environment_variable},
        {"shell script adapter path", [] {
            Package_files package = conforming_package();
            package.bootstrap.adapter_path[0].second = "adapter/run.sh";
            package.files["adapter/run.sh"] = "#!/bin/sh\n";
            return materialize(package);
        }, addon::reason::k_shell_adapter_entry_point},
        {"windows adapter without exe extension", [] {
            Package_files package = conforming_package();
            package.bootstrap.adapter_path[1].second = "adapter/fixture";
            return materialize(package);
        }, addon::reason::k_shell_adapter_entry_point},
        {"adapter is not an ELF image", [] {
            Package_files package = conforming_package();
            package.files["adapter/fixture"] = "#!/usr/bin/env python3\nprint('adapter')\n";
            return materialize(package);
        }, addon::reason::k_shell_adapter_entry_point},
        {"adapter not indexed", [] {
            Package_files package = conforming_package();
            addon::Integrity_index index = index_of(package);
            index.files.erase(std::remove_if(index.files.begin(), index.files.end(), [](const addon::Index_entry& entry) {
                return entry.path == "adapter/fixture";
            }), index.files.end());
            const fs::path root = materialize(package, addon::serialize_integrity_index(index));
            fs::remove(root / "adapter" / "fixture");
            return root;
        }, addon::reason::k_missing_file},
        {"unindexed file", [] {
            const fs::path root = materialize(conforming_package());
            write_file(root / "assets" / "extra.txt", "not indexed");
            return root;
        }, addon::reason::k_unindexed_file},
        {"missing indexed file", [] {
            const fs::path root = materialize(conforming_package());
            fs::remove(root / "assets" / "banner.txt");
            return root;
        }, addon::reason::k_missing_file},
        {"digest mismatch", [] {
            const fs::path root = materialize(conforming_package());
            write_file(root / "assets" / "banner.txt", "tampered after indexing");
            return root;
        }, addon::reason::k_digest_mismatch},
        {"size mismatch with same length prefix", [] {
            const fs::path root = materialize(conforming_package());
            write_file(root / "assets" / "banner.txt", "package-owned banner!");
            return root;
        }, addon::reason::k_digest_mismatch},
        {"escaped index path", [] {
            Package_files package = conforming_package();
            std::string text = addon::serialize_integrity_index(index_of(package));
            text.replace(text.find("\"assets/banner.txt\""), 19, "\"assets/../banner.txt\"");
            return materialize(package, text);
        }, addon::reason::k_path_escape},
        {"index not sorted", [] {
            Package_files package = conforming_package();
            addon::Integrity_index index = index_of(package);
            std::swap(index.files[0], index.files[1]);
            // serialize sorts; write the unsorted document by hand.
            std::string text = addon::serialize_integrity_index(index);
            const std::size_t first  = text.find("{\"path\":");
            const std::size_t second = text.find("{\"path\":", first + 1);
            const std::size_t third  = text.find("{\"path\":", second + 1);
            const std::string entry_a = text.substr(first, second - first - 1);
            const std::string entry_b = text.substr(second, third - second - 1);
            text.replace(first, third - first - 1, entry_b + "," + entry_a);
            return materialize(package, text);
        }, addon::reason::k_invalid_index},
        {"case-fold collision", [] {
            Package_files package = conforming_package();
            package.files["assets/Banner.txt"] = "collides on a case-insensitive file system";
            return materialize(package);
        }, addon::reason::k_invalid_index},
        {"third-party missing notice member", [] {
            Package_files package = conforming_package();
            std::string text = addon::serialize_integrity_index(index_of(package));
            const std::size_t mark = text.find("\"path\":\"assets/mark.svg\"");
            const std::size_t notice = text.find("\"notice\":\"notices/mark-license.txt\",", mark);
            text.erase(notice, std::string("\"notice\":\"notices/mark-license.txt\",").size());
            return materialize(package, text);
        }, addon::reason::k_third_party_record_invalid},
        {"third-party empty notice", [] {
            Package_files package = conforming_package();
            package.files["notices/mark-license.txt"] = "";
            return materialize(package);
        }, addon::reason::k_third_party_record_invalid},
        {"third-party escaped notice", [] {
            Package_files package = conforming_package();
            package.third_party["assets/mark.svg"].first = "notices/../mark-license.txt";
            return materialize(package);
        }, addon::reason::k_third_party_record_invalid},
        {"third-party notice outside notices/", [] {
            Package_files package = conforming_package();
            package.third_party["assets/mark.svg"].first = "assets/banner.txt";
            return materialize(package);
        }, addon::reason::k_third_party_record_invalid},
        {"third-party unindexed notice", [] {
            Package_files package = conforming_package();
            addon::Integrity_index index = index_of(package);
            index.files.erase(std::remove_if(index.files.begin(), index.files.end(), [](const addon::Index_entry& entry) {
                return entry.path == "notices/mark-license.txt";
            }), index.files.end());
            return materialize(package, addon::serialize_integrity_index(index));
        }, addon::reason::k_third_party_record_invalid},
        {"third-party digest mismatch", [] {
            const fs::path root = materialize(conforming_package());
            write_file(root / "notices" / "mark-license.txt", "a different notice");
            return root;
        }, addon::reason::k_third_party_record_invalid},
        {"third-party missing notice file", [] {
            const fs::path root = materialize(conforming_package());
            fs::remove(root / "notices" / "mark-license.txt");
            return root;
        }, addon::reason::k_third_party_record_invalid},
        {"package origin with notice", [] {
            Package_files package = conforming_package();
            std::string text = addon::serialize_integrity_index(index_of(package));
            const std::size_t banner = text.find("\"path\":\"assets/banner.txt\"");
            const std::size_t origin = text.find("\"origin\":\"package\"", banner);
            text.insert(origin + std::string("\"origin\":\"package\"").size(), ",\"notice\":\"notices/mark-license.txt\"");
            return materialize(package, text);
        }, addon::reason::k_invalid_index},
        {"kind disagrees with directory", [] {
            Package_files package = conforming_package();
            std::string text = addon::serialize_integrity_index(index_of(package));
            const std::size_t banner = text.find("\"path\":\"assets/banner.txt\"");
            const std::size_t kind = text.find("\"kind\":\"asset\"", banner);
            text.replace(kind, std::string("\"kind\":\"asset\"").size(), "\"kind\":\"helper\"");
            return materialize(package, text);
        }, addon::reason::k_invalid_index},
        {"sidecar name mismatch", [] {
            Package_files package = conforming_package();
            package.files["capabilities/profile-storage@2.json"] =
                addon::serialize_sidecar(addon::Capability_sidecar{"profile-storage", 1, addon::Json_value::object()});
            return materialize(package);
        }, addon::reason::k_sidecar_name_mismatch},
        {"sidecar file name out of grammar", [] {
            Package_files package = conforming_package();
            package.files["capabilities/profile-storage.json"] =
                addon::serialize_sidecar(addon::Capability_sidecar{"profile-storage", 1, addon::Json_value::object()});
            return materialize(package);
        }, addon::reason::k_sidecar_name_mismatch},
        {"invalid sidecar", [] {
            Package_files package = conforming_package();
            package.files[addon::sidecar_relative_path("profile-storage", 1)] = R"({"capability_id":"profile-storage","version":1})";
            return materialize(package);
        }, addon::reason::k_invalid_sidecar},
        {"sidecar with unexpected member", [] {
            Package_files package = conforming_package();
            package.files[addon::sidecar_relative_path("profile-storage", 1)] =
                R"({"capability_id":"profile-storage","version":1,"payload":{},"live":true})";
            return materialize(package);
        }, addon::reason::k_invalid_sidecar},
        {"index not json", [] {
            return materialize(conforming_package(), std::string("{"));
        }, addon::reason::k_invalid_index},
        {"index missing", [] {
            const fs::path root = materialize(conforming_package());
            fs::remove(root / addon::k_index_file_name);
            return root;
        }, addon::reason::k_invalid_index},
        {"index unsupported version", [] {
            Package_files package = conforming_package();
            std::string text = addon::serialize_integrity_index(index_of(package));
            text.replace(text.find("\"index_version\":1"), 17, "\"index_version\":2");
            return materialize(package, text);
        }, addon::reason::k_invalid_index},
    };

    for (const Case& test_case : cases) {
        const fs::path root = test_case.build();
        const std::string reason = validate_reason(root, test_case.options);
        ok &= check(reason == test_case.reason, id, std::string(test_case.name) + ": expected " + test_case.reason + ", got " + reason);
        remove_tree(root);
    }

    // Supply impossible directory trees as index bytes, not physical paths:
    // this must run on Windows too, where the conflicting names cannot coexist.
    struct Prefix_case
    {
        const char* name;
        std::vector<std::string> paths;
        bool accepted;
    };
    const std::vector<Prefix_case> prefix_cases = {
        {"file/directory alias", {"assets/Foo", "assets/foo/x.txt"}, false},
        {"directory spelling alias", {"assets/Foo/a.txt", "assets/foo/b.txt"}, false},
        {"identical file/directory prefix", {"assets/Foo", "assets/Foo/x.txt"}, false},
        {"nested directory alias", {"assets/Foo/A/x", "assets/Foo/a/y"}, false},
        {"consistent shared directory", {"assets/Foo/a.txt", "assets/Foo/b.txt"}, true},
        {"distinct directory names", {"assets/Foo/a.txt", "assets/Food/b.txt"}, true},
    };
    for (const Prefix_case& test_case : prefix_cases) {
        Package_files package = conforming_package();
        for (const std::string& path : test_case.paths) {
            package.files[path] = "prefix fixture";
        }
        addon::Contract_failure failure;
        const auto parsed = addon::parse_integrity_index(
            addon::serialize_integrity_index(index_of(package)), &failure);
        ok &= check(parsed.has_value() == test_case.accepted, id,
            std::string(test_case.name) + ": unexpected index acceptance");
        if (!test_case.accepted) {
            ok &= check(failure.reason_code == addon::reason::k_invalid_index, id,
                std::string(test_case.name) + ": expected invalid_index");
        }
    }

    // Symbolic links are rejected wherever the platform lets a test create one.
    {
        const fs::path root = materialize(conforming_package());
        std::error_code error;
        fs::create_symlink(root / "assets" / "banner.txt", root / "assets" / "link.txt", error);
        if (!error) {
            ok &= check(validate_reason(root) == addon::reason::k_link_entry, id, "symbolic link entry is link_entry");
        }
        remove_tree(root);
    }
    {
        const fs::path root = materialize(conforming_package());
        std::error_code error;
        fs::remove(root / "assets" / "banner.txt");
        fs::create_symlink(root / "assets" / "mark.svg", root / "assets" / "banner.txt", error);
        if (!error) {
            ok &= check(validate_reason(root) == addon::reason::k_link_entry, id, "indexed path that became a link is link_entry");
        }
        remove_tree(root);
    }
    {
        const fs::path root = materialize(conforming_package());
        std::error_code error;
        fs::create_directory_symlink(root / "assets", root / "helpers" / "linked", error);
        if (!error) {
            ok &= check(validate_reason(root) == addon::reason::k_link_entry, id, "directory link is link_entry and not descended");
        }
        remove_tree(root);
    }
    return ok;
}

bool test_file_time_ticks_text()
{
    using File_time = fs::file_time_type;
    using Ticks = File_time::duration::rep;
    const auto text = [](Ticks ticks) {
        return addon::file_time_ticks_text(File_time{File_time::duration{ticks}});
    };
    SDK_CHECK(text(0) == "0", "file-clock zero has one digit");
    SDK_CHECK(text(1) == "1" && text(-1) == "-1", "file-clock sign is exact");
    SDK_CHECK(text(1234567890) == "1234567890" && text(-1234567890) == "-1234567890",
        "file-clock ticks retain decimal precision");
    if constexpr (std::numeric_limits<Ticks>::digits == 63) {
        SDK_CHECK(text(std::numeric_limits<Ticks>::max()) == "9223372036854775807",
            "64-bit file-clock maximum is exact");
        SDK_CHECK(text(std::numeric_limits<Ticks>::min()) == "-9223372036854775808",
            "64-bit file-clock minimum is exact");
    }
    if constexpr (std::numeric_limits<Ticks>::digits == 127) {
        SDK_CHECK(text(std::numeric_limits<Ticks>::max()) == "170141183460469231731687303715884105727",
            "128-bit file-clock maximum is exact");
        SDK_CHECK(text(std::numeric_limits<Ticks>::min()) == "-170141183460469231731687303715884105728",
            "128-bit file-clock minimum is exact");
    }
    return true;
}

bool test_package_source_listing_bounds()
{
    bool ok = true;
    const char* id = __func__;

    {
        const fs::path root = materialize(conforming_package());
        for (std::size_t index = 0;
             index < addon::limits::k_max_files_per_package;
             ++index)
        {
            char name[24];
            std::snprintf(name, sizeof(name), "extra-%05zu", index);
            std::error_code error;
            fs::create_directory(root / name, error);
            if (error) {
                ok &= check(false, id, "cannot seed the entry-count fixture: " + error.message());
                break;
            }
        }
        addon::Directory_package_source source(root);
        addon::Source_error error;
        ok &= check(
            !source.list(&error).has_value() &&
                error.kind == addon::Source_error_kind::BOUND_EXCEEDED,
            id,
            "directory listing stops at k_max_files_per_package");
        ok &= check(
            validate_reason(root) == addon::reason::k_custody_bound_exceeded,
            id,
            "entry-count listing failure reports custody_bound_exceeded");
        remove_tree(root);
    }
    {
        const fs::path root = materialize(conforming_package());
        fs::path nested = root;
        for (std::size_t depth = 0;
             depth <= addon::limits::k_max_package_path_segments;
             ++depth)
        {
            nested /= "d";
            std::error_code error;
            fs::create_directory(nested, error);
            if (error) {
                ok &= check(false, id, "cannot seed the depth fixture: " + error.message());
                break;
            }
        }
        addon::Directory_package_source source(root);
        addon::Source_error error;
        ok &= check(
            !source.list(&error).has_value() &&
                error.kind == addon::Source_error_kind::BOUND_EXCEEDED,
            id,
            "directory listing stops beyond the path-segment bound");
        ok &= check(
            validate_reason(root) == addon::reason::k_custody_bound_exceeded,
            id,
            "depth listing failure reports custody_bound_exceeded");
        remove_tree(root);
    }
    return ok;
}

bool test_index_parser_static_rules()
{
    bool ok = true;
    const char* id = __func__;
    addon::Contract_failure failure;

    // Entry count above the bound is a custody bound, checked before content.
    {
        std::string text = R"({"index_version":1,"files":[)";
        for (std::size_t i = 0; i <= addon::limits::k_max_files_per_package; ++i) {
            if (i > 0) {
                text += ",";
            }
            char name[16];
            std::snprintf(name, sizeof(name), "%05zu", i);
            text += R"({"path":"assets/)" + std::string(name) + R"(","bytes":1,"sha256":")" + std::string(64, 'a') +
                    R"(","resource":{"kind":"asset","origin":"package"}})";
        }
        text += "]}";
        ok &= check(!addon::parse_integrity_index(text, &failure).has_value() && failure.reason_code == addon::reason::k_custody_bound_exceeded, id,
            "k_max_files_per_package + 1 entries");
    }
    // Per-file byte bound and generation byte bound.
    {
        const std::string text = R"({"index_version":1,"files":[{"path":"assets/a","bytes":)" +
            std::to_string(addon::limits::k_max_file_bytes + 1) + R"(,"sha256":")" + std::string(64, 'a') +
            R"(","resource":{"kind":"asset","origin":"package"}}]})";
        ok &= check(!addon::parse_integrity_index(text, &failure).has_value() && failure.reason_code == addon::reason::k_custody_bound_exceeded &&
            failure.detail.find("k_max_file_bytes") != std::string::npos, id, "k_max_file_bytes + 1");
    }
    {
        std::string text = R"({"index_version":1,"files":[)";
        for (int i = 0; i < 5; ++i) {
            if (i > 0) {
                text += ",";
            }
            text += R"({"path":"assets/f)" + std::to_string(i) + R"(","bytes":)" + std::to_string(addon::limits::k_max_file_bytes) +
                    R"(,"sha256":")" + std::string(64, 'a') + R"(","resource":{"kind":"asset","origin":"package"}})";
        }
        text += "]}";
        ok &= check(!addon::parse_integrity_index(text, &failure).has_value() && failure.reason_code == addon::reason::k_custody_bound_exceeded &&
            failure.detail.find("k_max_generation_bytes") != std::string::npos, id, "sum above k_max_generation_bytes");
    }
    // Digest grammar and origin vocabulary.
    ok &= check(!addon::parse_integrity_index(R"({"index_version":1,"files":[{"path":"assets/a","bytes":1,"sha256":"abc","resource":{"kind":"asset","origin":"package"}}]})", &failure).has_value() &&
        failure.reason_code == addon::reason::k_invalid_index, id, "short digest");
    ok &= check(!addon::parse_integrity_index(R"({"index_version":1,"files":[{"path":"assets/a","bytes":1,"sha256":")" + std::string(64, 'a') +
        R"(","resource":{"kind":"asset"}}]})", &failure).has_value() && failure.reason_code == addon::reason::k_invalid_index, id, "origin is required");
    ok &= check(!addon::parse_integrity_index(R"({"index_version":1,"files":[{"path":"assets/a","bytes":1,"sha256":")" + std::string(64, 'a') +
        R"(","resource":{"kind":"asset","origin":"vendor"}}]})", &failure).has_value() && failure.reason_code == addon::reason::k_invalid_index, id, "origin outside the vocabulary");
    ok &= check(!addon::parse_integrity_index(R"({"index_version":1,"files":[{"path":"assets/a","bytes":-1,"sha256":")" + std::string(64, 'a') +
        R"(","resource":{"kind":"asset","origin":"package"}}]})", &failure).has_value() && failure.reason_code == addon::reason::k_invalid_index, id, "negative bytes");
    ok &= check(!addon::parse_integrity_index("{\"index_version\":1,\"files\":[],\"x\":\"\xFF\"}", &failure).has_value() &&
        failure.reason_code == addon::reason::k_invalid_utf8, id, "invalid UTF-8 index");
    ok &= check(addon::parse_integrity_index(R"({"index_version":1,"files":[]})", &failure).has_value(), id, "empty index parses");
    ok &= check(!addon::parse_integrity_index(std::string(addon::limits::k_max_index_bytes + 1, ' '), &failure).has_value() &&
        failure.reason_code == addon::reason::k_custody_bound_exceeded, id, "index above k_max_index_bytes");

    // Sidecar bounds and bootstrap bounds.
    ok &= check(!addon::parse_sidecar(std::string(addon::limits::k_max_sidecar_bytes + 1, ' '), &failure).has_value() &&
        failure.reason_code == addon::reason::k_custody_bound_exceeded, id, "sidecar above k_max_sidecar_bytes");
    ok &= check(!addon::parse_bootstrap(std::string(addon::limits::k_max_bootstrap_bytes + 1, ' '), &failure).has_value() &&
        failure.reason_code == addon::reason::k_invalid_bootstrap, id, "bootstrap above k_max_bootstrap_bytes");
    ok &= check(addon::sidecar_file_name("profile-storage", 1) == "profile-storage@1.json", id, "sidecar file name");

    // Bootstrap argv and environment bounds.
    addon::Bootstrap_manifest manifest = conforming_package().bootstrap;
    manifest.adapter_argv.assign(addon::limits::k_max_argv_entries + 1, "a");
    ok &= check(!addon::parse_bootstrap(addon::serialize_bootstrap(manifest), &failure).has_value() && failure.reason_code == addon::reason::k_invalid_bootstrap, id,
        "argv above k_max_argv_entries");
    manifest = conforming_package().bootstrap;
    manifest.adapter_argv = {"a\tb"};
    ok &= check(!addon::parse_bootstrap(addon::serialize_bootstrap(manifest), &failure).has_value(), id, "argv with control character");
    const auto argv_with_total = [](std::size_t bytes) {
        std::vector<std::string> argv;
        while (bytes != 0) {
            const std::size_t entry_bytes = std::min(bytes, addon::limits::k_max_argv_entry_bytes);
            argv.emplace_back(entry_bytes, 'a');
            bytes -= entry_bytes;
        }
        return argv;
    };
    for (const std::size_t bytes : {
             addon::limits::k_max_argv_total_bytes - 1,
             addon::limits::k_max_argv_total_bytes,
         })
    {
        manifest = conforming_package().bootstrap;
        manifest.adapter_argv = argv_with_total(bytes);
        ok &= check(addon::parse_bootstrap(addon::serialize_bootstrap(manifest), &failure).has_value(), id,
            bytes == addon::limits::k_max_argv_total_bytes
                ? "bootstrap argv at k_max_argv_total_bytes"
                : "bootstrap argv one byte below k_max_argv_total_bytes");
    }
    manifest = conforming_package().bootstrap;
    manifest.adapter_argv = argv_with_total(addon::limits::k_max_argv_total_bytes + 1);
    ok &= check(!addon::parse_bootstrap(addon::serialize_bootstrap(manifest), &failure).has_value() &&
            failure.reason_code == addon::reason::k_invalid_bootstrap,
        id,
        "bootstrap argv one byte above k_max_argv_total_bytes");
    for (const std::size_t bytes : {
             addon::limits::k_max_environment_value_bytes - 1,
             addon::limits::k_max_environment_value_bytes,
         })
    {
        manifest = conforming_package().bootstrap;
        manifest.adapter_environment = {{"VALUE", std::string(bytes, 'x')}};
        ok &= check(addon::parse_bootstrap(addon::serialize_bootstrap(manifest), &failure).has_value(), id,
            bytes == addon::limits::k_max_environment_value_bytes
                ? "bootstrap environment value at k_max_environment_value_bytes"
                : "bootstrap environment value one byte below k_max_environment_value_bytes");
    }
    manifest = conforming_package().bootstrap;
    manifest.adapter_environment = {{"VALUE", std::string(addon::limits::k_max_environment_value_bytes + 1, 'x')}};
    ok &= check(!addon::parse_bootstrap(addon::serialize_bootstrap(manifest), &failure).has_value() &&
            failure.reason_code == addon::reason::k_invalid_bootstrap,
        id,
        "bootstrap environment value one byte above k_max_environment_value_bytes");
    manifest = conforming_package().bootstrap;
    manifest.adapter_environment.clear();
    for (std::size_t i = 0; i <= addon::limits::k_max_environment_entries; ++i) {
        manifest.adapter_environment.emplace_back("V" + std::to_string(i), "x");
    }
    ok &= check(!addon::parse_bootstrap(addon::serialize_bootstrap(manifest), &failure).has_value(), id, "environment above k_max_environment_entries");
    manifest = conforming_package().bootstrap;
    manifest.platforms.push_back("plan9-mips");
    manifest.adapter_path.emplace_back("plan9-mips", "adapter/fixture");
    ok &= check(addon::parse_bootstrap(addon::serialize_bootstrap(manifest), &failure).has_value(), id, "unknown platform id is ignored, not rejected");
    manifest = conforming_package().bootstrap;
    manifest.platforms.pop_back();
    ok &= check(!addon::parse_bootstrap(addon::serialize_bootstrap(manifest), &failure).has_value() && failure.reason_code == addon::reason::k_invalid_bootstrap, id,
        "adapter path for an unlisted platform");
    manifest = conforming_package().bootstrap;
    manifest.adapter_path.pop_back();
    ok &= check(!addon::parse_bootstrap(addon::serialize_bootstrap(manifest), &failure).has_value() && failure.reason_code == addon::reason::k_invalid_bootstrap, id,
        "listed platform without an adapter path");
    manifest = conforming_package().bootstrap;
    manifest.core_protocol_generations = {1, 1};
    ok &= check(!addon::parse_bootstrap(addon::serialize_bootstrap(manifest), &failure).has_value(), id, "duplicate core generation");
    manifest = conforming_package().bootstrap;
    manifest.integrity_index = "index.json";
    ok &= check(!addon::parse_bootstrap(addon::serialize_bootstrap(manifest), &failure).has_value(), id, "integrity index name is fixed");
    return ok;
}

} // namespace sdk_tests
