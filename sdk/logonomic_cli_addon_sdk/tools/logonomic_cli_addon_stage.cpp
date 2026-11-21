// Offline payload assembly. The package contract, including every optional
// sidecar, is validated by the same SDK implementation the host uses.

#include "logonomic_cli_addon/ids.h"
#include "logonomic_cli_addon/json.h"
#include "logonomic_cli_addon/limits.h"
#include "logonomic_cli_addon/package_source.h"
#include "logonomic_cli_addon/package_validator.h"
#include "logonomic_cli_addon/sha256.h"
#include "logonomic_cli_addon/version.h"
#include "strict_json.h"

#ifdef _WIN32
#include <windows.h>
#endif

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <optional>
#include <random>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

namespace addon  = logonomic::cli_addon;
namespace strict = logonomic::cli_addon::strict;
namespace fs     = std::filesystem;

struct Artifact
{
    fs::path                 directory;
    std::string              expected_digest;
    addon::Validated_package package;
};

int fail(const std::string& reason, const std::string& detail)
{
    std::fprintf(stderr, "FAIL %s %s\n", reason.c_str(), detail.c_str());
    return 1;
}

int usage()
{
    std::fprintf(stderr, "usage: logonomic_cli_addon_stage --output <fresh-directory> "
        "[--input <artifact-set.json>] [--evidence <receipt.json>] [--platform <id>]\n"
        "       logonomic_cli_addon_stage --verify <directory> --evidence <receipt.json> [--platform <id>]\n");
    return 2;
}

bool read_artifacts(const fs::path& input, std::vector<Artifact>* out_artifacts)
{
    const fs::path absolute_input = fs::absolute(input);
    addon::Directory_package_source source(absolute_input.parent_path());
    addon::Source_error read_error;
    const auto bytes = source.read(
        addon::path_to_utf8(absolute_input.filename()), addon::limits::k_max_index_bytes, &read_error);
    if (!bytes) {
        fail("invalid_artifact_set", read_error.detail);
        return false;
    }
    addon::Json_parse_error parse_error;
    const auto document = addon::parse_json(*bytes, {}, &parse_error);
    if (!document || !document->is_object() ||
        strict::unexpected_member(*document, {"artifact_set_version", "packages"}) ||
        strict::integer_member(*document, "artifact_set_version", 1, 1) != 1)
    {
        fail("invalid_artifact_set", "expected artifact_set_version 1 and packages array");
        return false;
    }
    const auto* packages = strict::member_of_type(*document, "packages", addon::Json_type::ARRAY);
    if (!packages || packages->as_array().size() > addon::limits::k_max_candidates_per_root) {
        fail("invalid_artifact_set", "packages array is missing or exceeds k_max_candidates_per_root");
        return false;
    }
    for (const addon::Json_value& entry : packages->as_array()) {
        if (!entry.is_object() || strict::unexpected_member(entry, {"directory", "generation_digest"})) {
            fail("invalid_artifact_set", "package entry must contain directory and generation_digest");
            return false;
        }
        const auto* directory = strict::string_member(entry, "directory");
        const auto* digest    = strict::string_member(entry, "generation_digest");
        if (!directory || !digest || !strict::has_no_control_characters(*directory) ||
            directory->size() > addon::limits::k_max_start_absolute_path_bytes ||
            !addon::path_from_utf8(*directory).is_absolute() || !addon::sha256_from_hex(*digest))
        {
            fail("invalid_artifact_set", "package entry needs an absolute directory and lowercase generation digest");
            return false;
        }
        out_artifacts->push_back({fs::canonical(addon::path_from_utf8(*directory)), *digest, {}});
    }
    return true;
}

// Only this invocation's newly-created sibling is owned. Existing payloads
// are never removed or overwritten; their publication belongs to the caller.
class Temporary_directory
{
public:
    explicit Temporary_directory(const fs::path& parent)
    {
        std::mt19937_64 random(std::random_device{}());
        for (int attempt = 0; attempt < 16; ++attempt) {
            const fs::path candidate = parent / (".cli-addon-stage-" + std::to_string(random()));
            if (fs::create_directory(candidate)) {
                m_path = candidate;
                return;
            }
        }
        throw std::runtime_error("cannot create a fresh package staging directory");
    }

    ~Temporary_directory()
    {
        if (!m_path.empty()) {
            std::error_code error;
            fs::remove_all(m_path, error);
            if (error) {
                fail("staging_cleanup_failed", addon::path_to_utf8(m_path) + ": " + error.message());
            }
        }
    }

    const fs::path& path() const { return m_path; }
    void release() { m_path.clear(); }

private:
    fs::path m_path;
};

void set_mode(const fs::path& path, bool executable)
{
#ifndef _WIN32
    fs::perms permissions = fs::perms::owner_read | fs::perms::owner_write |
        fs::perms::group_read | fs::perms::others_read;
    if (executable) {
        permissions |= fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec;
    }
    fs::permissions(path, permissions);
#else
    (void)path;
    (void)executable;
#endif
}

bool write_file(const fs::path& path, std::string_view bytes, bool executable)
{
    fs::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(bytes.data(), (std::streamsize)bytes.size());
    output.close();
    if (!output) {
        fail("staging_write_failed", addon::path_to_utf8(path));
        return false;
    }
    set_mode(path, executable);
    return true;
}

bool validate_generation(
    const addon::Package_validator& validator,
    const fs::path&                 directory,
    const std::string&              expected_digest,
    addon::Validated_package*      out_package)
{
    addon::Directory_package_source source(directory);
    addon::Contract_failure failure;
    auto package = validator.validate(source, &failure);
    if (!package) {
        fail(failure.reason_code, addon::path_to_utf8(directory) + ": " + failure.detail);
        return false;
    }
    if (package->generation_digest_hex != expected_digest) {
        fail(addon::reason::k_digest_mismatch, addon::path_to_utf8(directory) + ": expected " +
            expected_digest + ", found " + package->generation_digest_hex);
        return false;
    }
    *out_package = std::move(*package);
    return true;
}

std::string evidence_bytes(std::vector<Artifact> artifacts)
{
    std::sort(artifacts.begin(), artifacts.end(), [](const Artifact& left, const Artifact& right) {
        return left.package.bootstrap.package_id < right.package.bootstrap.package_id;
    });
    addon::Json_value packages = addon::Json_value::array();
    for (const Artifact& artifact : artifacts) {
        addon::Json_value package = addon::Json_value::object();
        package.set("package_id", addon::Json_value(artifact.package.bootstrap.package_id));
        package.set("generation_digest", addon::Json_value(artifact.package.generation_digest_hex));
        const auto index = addon::parse_json(addon::serialize_integrity_index(artifact.package.index), {}, nullptr);
        addon::Json_value files = *index->find("files");
        const std::pair<const char*, const std::string*> root_files[] = {
            {addon::k_bootstrap_file_name, &artifact.package.bootstrap_bytes},
            {addon::k_index_file_name,     &artifact.package.index_bytes},
        };
        for (const auto& [path, bytes] : root_files) {
            addon::Json_value file = addon::Json_value::object();
            file.set("path",     addon::Json_value(path));
            file.set("bytes",    addon::Json_value((std::uint64_t)bytes->size()));
            file.set("sha256",   addon::Json_value(addon::to_hex(addon::Sha256::digest(*bytes))));
            file.set("resource", addon::Json_value());
            files.push_back(std::move(file));
        }
        std::sort(files.as_array().begin(), files.as_array().end(),
            [](const addon::Json_value& left, const addon::Json_value& right) {
                return left.find("path")->as_string() < right.find("path")->as_string();
            });
        package.set("files", std::move(files));
        packages.push_back(std::move(package));
    }
    addon::Json_value evidence = addon::Json_value::object();
    evidence.set("prebuilt_package_evidence_version", addon::Json_value(1));
    evidence.set("packages", std::move(packages));
    return addon::serialize_json(evidence) + "\n";
}

bool is_link(const fs::path& path)
{
#ifdef _WIN32
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        throw std::runtime_error("cannot inspect " + addon::path_to_utf8(path));
    }
    return (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
#else
    return fs::is_symlink(fs::symlink_status(path));
#endif
}

int verify(const fs::path& root, const fs::path& evidence, const addon::Validation_options& options)
{
    addon::Package_validator validator(options);
    std::vector<Artifact> artifacts;
    fs::directory_iterator entries;
    if (fs::exists(fs::symlink_status(root))) {
        if (is_link(root) || !fs::is_directory(root)) {
            return fail("invalid_staged_root", "package root is not an ordinary directory");
        }
        entries = fs::directory_iterator(fs::canonical(root));
    }
    // File-only archives omit an empty cli_addons directory. Reconstructing
    // zero packages is valid only if the receipt is exactly the empty set.
    for (const auto& entry : entries) {
        if (artifacts.size() == addon::limits::k_max_candidates_per_root ||
            is_link(entry.path()) || !entry.is_directory())
        {
            return fail("invalid_staged_root", "expected only bounded package directories: " +
                addon::path_to_utf8(entry.path()));
        }
        addon::Directory_package_source source(entry.path());
        addon::Contract_failure failure;
        auto package = validator.validate(source, &failure);
        if (!package) {
            return fail(failure.reason_code, source.describe() + ": " + failure.detail);
        }
        if (addon::path_to_utf8(entry.path().filename()) != package->bootstrap.package_id) {
            return fail("invalid_staged_root", "directory name differs from validated package ID: " + source.describe());
        }
        artifacts.push_back({entry.path(), package->generation_digest_hex, std::move(*package)});
    }
    const std::string expected = evidence_bytes(std::move(artifacts));
    const fs::path absolute_evidence = fs::absolute(evidence);
    addon::Directory_package_source source(absolute_evidence.parent_path());
    addon::Source_error error;
    const auto actual = source.read(addon::path_to_utf8(absolute_evidence.filename()), expected.size(), &error);
    if (!actual || *actual != expected) {
        return fail("evidence_mismatch", actual ? "receipt differs from validated package bytes" : error.detail);
    }
    std::fwrite(expected.data(), 1, expected.size(), stdout);
    return std::ferror(stdout) ? fail("evidence_write_failed", "cannot write verified receipt to stdout") : 0;
}

int stage(
    const fs::path& output,
    const fs::path& evidence,
    std::vector<Artifact> artifacts,
    const addon::Validation_options& options)
{
    const fs::path destination = fs::canonical(fs::absolute(output).parent_path()) / output.filename();
    if (output.filename().empty() || output.filename() == "." || output.filename() == ".." ||
        fs::exists(fs::symlink_status(destination)))
    {
        return fail("output_exists", "output must name a fresh absent directory");
    }
    fs::path evidence_path;
    if (!evidence.empty()) {
        evidence_path = fs::canonical(fs::absolute(evidence).parent_path()) / evidence.filename();
        if (fs::exists(fs::symlink_status(evidence_path)) || evidence_path == destination) {
            return fail("output_exists", "evidence must name a fresh absent file outside the output");
        }
    }
    addon::Package_validator validator(options);
    std::set<std::string> package_ids;
    for (Artifact& artifact : artifacts) {
        if (!validate_generation(validator, artifact.directory, artifact.expected_digest, &artifact.package)) {
            return 1;
        }
        if (!package_ids.insert(artifact.package.bootstrap.package_id).second) {
            return fail(addon::reason::k_duplicate_package_id, artifact.package.bootstrap.package_id);
        }
    }

    Temporary_directory temporary(destination.parent_path());
    for (const Artifact& artifact : artifacts) {
        const fs::path package_root = temporary.path() / artifact.package.bootstrap.package_id;
        if (!write_file(package_root / addon::k_bootstrap_file_name, artifact.package.bootstrap_bytes, false) ||
            !write_file(package_root / addon::k_index_file_name, artifact.package.index_bytes, false))
        {
            return 1;
        }
        addon::Directory_package_source source(artifact.directory);
        for (const addon::Index_entry& entry : artifact.package.index.files) {
            addon::Source_error error;
            const auto bytes = source.read(entry.path, entry.bytes, &error);
            if (!bytes || bytes->size() != entry.bytes ||
                addon::to_hex(addon::Sha256::digest(*bytes)) != entry.sha256_hex)
            {
                return fail(addon::reason::k_source_mutated,
                    source.describe() + "/" + entry.path + (bytes ? "" : ": " + error.detail));
            }
            const bool executable = entry.kind == addon::Resource_kind::EXECUTABLE ||
                                    entry.kind == addon::Resource_kind::HELPER;
            if (!write_file(package_root / addon::path_from_utf8(entry.path), *bytes, executable)) {
                return 1;
            }
        }
    }
    set_mode(temporary.path(), true);
    for (const auto& entry : fs::recursive_directory_iterator(temporary.path())) {
        if (entry.is_directory()) {
            set_mode(entry.path(), true);
        }
    }
    for (const Artifact& artifact : artifacts) {
        addon::Validated_package verified;
        if (!validate_generation(validator, temporary.path() / artifact.package.bootstrap.package_id,
                artifact.expected_digest, &verified) ||
            !validate_generation(validator, artifact.directory, artifact.expected_digest, &verified))
        {
            return 1;
        }
    }
    if (fs::exists(fs::symlink_status(destination))) {
        return fail("output_exists", "output appeared during staging");
    }
    fs::rename(temporary.path(), destination);
    temporary.release();
    if (!evidence_path.empty() && !write_file(evidence_path, evidence_bytes(artifacts), false)) {
        return 1;
    }
    std::fprintf(stdout, "OK staged %zu packages\n", artifacts.size());
    return 0;
}

} // namespace

int run(const std::vector<std::string>& arguments)
{
    fs::path output;
    fs::path input;
    fs::path evidence;
    fs::path verify_root;
    addon::Validation_options options;
    std::set<std::string> seen;
    for (std::size_t i = 1; i < arguments.size(); ++i) {
        const std::string& argument = arguments[i];
        if (i + 1 == arguments.size() || arguments[i + 1].empty() || !seen.insert(argument).second) {
            return usage();
        }
        if (argument == "--output") {
            output = addon::path_from_utf8(arguments[++i]);
        }
        else
        if (argument == "--input") {
            input = addon::path_from_utf8(arguments[++i]);
        }
        else
        if (argument == "--evidence") {
            evidence = addon::path_from_utf8(arguments[++i]);
        }
        else
        if (argument == "--verify") {
            verify_root = addon::path_from_utf8(arguments[++i]);
        }
        else
        if (argument == "--platform") {
            options.platform_id = arguments[++i];
        }
        else {
            return usage();
        }
    }
    if (output.empty() == verify_root.empty() || !addon::is_valid_platform_id(options.platform_id) ||
        (!verify_root.empty() && (evidence.empty() || !input.empty())))
    {
        return usage();
    }
    try {
        if (!verify_root.empty()) {
            return verify(verify_root, evidence, options);
        }
        std::vector<Artifact> artifacts;
        if (!input.empty() && !read_artifacts(input, &artifacts)) {
            return 1;
        }
        return stage(output, evidence, std::move(artifacts), options);
    }
    catch (const std::exception& error) {
        return fail("staging_io_failed", error.what());
    }
}

#ifdef _WIN32
int wmain(int argc, wchar_t** argv)
{
    std::vector<std::string> arguments;
    for (int i = 0; i < argc; ++i) {
        arguments.push_back(addon::path_to_utf8(fs::path(argv[i])));
    }
    return run(arguments);
}
#else
int main(int argc, char** argv)
{
    return run(std::vector<std::string>(argv, argv + argc));
}
#endif
