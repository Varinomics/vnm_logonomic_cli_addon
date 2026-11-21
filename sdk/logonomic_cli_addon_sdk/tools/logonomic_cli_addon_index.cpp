// logonomic_cli_addon_index <package-dir>
//     [--third-party <resource> <notice> <provenance>]...
//     [--inject-defect <name>]
//
// Writes <package-dir>/package.files.json: every regular file below the
// package directory other than the bootstrap and the index itself, with its
// size, SHA-256 digest and resource classification. Files are package-owned
// unless a --third-party declaration names them; the declared resource and
// its notice and provenance records all carry the third-party origin with
// the same two references.
//
// --inject-defect exists for assembling the conformance fixture packages
// whose index must be deliberately wrong. The names and what they break are
// listed in docs/conformance.md; without the option the tool writes only a
// contract-conforming index.

#include "tool_arguments.h"

#include "logonomic_cli_addon/ids.h"
#include "logonomic_cli_addon/integrity_index.h"
#include "logonomic_cli_addon/json.h"
#include "logonomic_cli_addon/limits.h"
#include "logonomic_cli_addon/package_source.h"
#include "logonomic_cli_addon/sha256.h"
#include "logonomic_cli_addon/version.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

namespace {

namespace addon = logonomic::cli_addon;

struct Third_party_declaration
{
    std::string resource;
    std::string notice;
    std::string provenance;
};

int usage()
{
    std::fprintf(stderr,
        "usage: logonomic_cli_addon_index <package-dir> "
        "[--third-party <resource> <notice> <provenance>]... [--inject-defect <name>]\n");
    return 2;
}

int fail(const std::string& message)
{
    std::fprintf(stderr, "logonomic_cli_addon_index: %s\n", message.c_str());
    return 1;
}

addon::Json_value* find_entry(addon::Json_value& document, const std::string& path)
{
    for (addon::Json_value& entry : document.find("files")->as_array()) {
        if (entry.find("path")->as_string() == path) {
            return &entry;
        }
    }
    return nullptr;
}

// The first entry with the given origin and, when given, kind.
addon::Json_value* find_entry_by_class(addon::Json_value& document, const char* origin, const char* kind)
{
    for (addon::Json_value& entry : document.find("files")->as_array()) {
        const addon::Json_value* resource = entry.find("resource");
        if (resource->find("origin")->as_string() != origin) {
            continue;
        }
        if (kind != nullptr && resource->find("kind")->as_string() != kind) {
            continue;
        }
        return &entry;
    }
    return nullptr;
}

void flip_digest(addon::Json_value& entry)
{
    std::string digest = entry.find("sha256")->as_string();
    digest[0] = digest[0] == '0' ? '1' : '0';
    entry.set("sha256", addon::Json_value(digest));
}

// Applies one deliberate defect to the canonical index document; returns
// false for an unknown name or a package without the entries it needs.
bool inject_defect(addon::Json_value& document, const std::string& defect, std::string* out_error)
{
    if (defect == "escaped-path") {
        addon::Json_value resource = addon::Json_value::object();
        resource.set("kind",   addon::Json_value("asset"));
        resource.set("origin", addon::Json_value("package"));
        addon::Json_value entry = addon::Json_value::object();
        entry.set("path",     addon::Json_value("assets/../escaped.txt"));
        entry.set("bytes",    addon::Json_value(0));
        entry.set("sha256",   addon::Json_value(addon::to_hex(addon::Sha256::digest(""))));
        entry.set("resource", std::move(resource));
        addon::Json_value::Array& files = document.find("files")->as_array();
        const auto position = std::find_if(files.begin(), files.end(), [](const addon::Json_value& existing) {
            return existing.find("path")->as_string() > "assets/../escaped.txt";
        });
        files.insert(position, std::move(entry));
        return true;
    }
    if (defect == "digest-mismatch") {
        addon::Json_value* entry = find_entry_by_class(document, "package", "asset");
        if (entry == nullptr) {
            entry = find_entry_by_class(document, "package", "executable");
        }
        if (entry == nullptr) {
            *out_error = "digest-mismatch needs a package-owned asset or executable";
            return false;
        }
        flip_digest(*entry);
        return true;
    }

    // The remaining defects operate on a third-party resource and its notice.
    addon::Json_value* third_party = nullptr;
    for (addon::Json_value& entry : document.find("files")->as_array()) {
        const addon::Json_value* resource = entry.find("resource");
        const std::string& kind = resource->find("kind")->as_string();
        if (resource->find("origin")->as_string() == "third-party" && kind != "notice" && kind != "provenance") {
            third_party = &entry;
            break;
        }
    }
    if (third_party == nullptr) {
        *out_error = defect + " needs a third-party resource declaration";
        return false;
    }
    addon::Json_value& resource = *third_party->find("resource");
    const std::string notice_path = resource.find("notice")->as_string();

    if (defect == "third-party-missing-notice") {
        resource.erase("notice");
        return true;
    }
    if (defect == "third-party-escaped-notice") {
        resource.set("notice", addon::Json_value("../" + notice_path));
        return true;
    }
    if (defect == "third-party-unindexed-notice") {
        addon::Json_value::Array& files = document.find("files")->as_array();
        files.erase(std::remove_if(files.begin(), files.end(), [&notice_path](const addon::Json_value& entry) {
            return entry.find("path")->as_string() == notice_path;
        }), files.end());
        return true;
    }
    if (defect == "third-party-digest-mismatch") {
        addon::Json_value* notice = find_entry(document, notice_path);
        flip_digest(*notice);
        return true;
    }
    if (defect == "third-party-empty-notice") {
        // The index stays faithful; the defect is the zero-byte notice file
        // the package ships, which the tool would otherwise refuse to index.
        const addon::Json_value* notice = find_entry(document, notice_path);
        if (notice->find("bytes")->as_integer() != 0) {
            *out_error = "third-party-empty-notice needs a zero-byte notice file at " + notice_path;
            return false;
        }
        return true;
    }
    *out_error = "unknown defect " + defect;
    return false;
}

} // namespace

int run(int argc, char** argv)
{
    std::string directory;
    std::vector<Third_party_declaration> third_party;
    std::string defect;

    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument == "--third-party" && i + 3 < argc) {
            third_party.push_back(Third_party_declaration{argv[i + 1], argv[i + 2], argv[i + 3]});
            i += 3;
        }
        else
        if (argument == "--inject-defect" && i + 1 < argc) {
            defect = argv[++i];
        }
        else
        if (!argument.empty() && argument[0] == '-') {
            return usage();
        }
        else
        if (directory.empty()) {
            directory = argument;
        }
        else {
            return usage();
        }
    }
    if (directory.empty()) {
        return usage();
    }

    const std::filesystem::path root = addon::path_from_utf8(directory);
    addon::Directory_package_source source(root);
    addon::Source_error list_error;
    const std::optional<std::vector<addon::Source_entry>> entries = source.list(&list_error);
    if (!entries.has_value()) {
        return fail(list_error.detail);
    }

    addon::Integrity_index index;
    index.index_version = addon::k_index_version;
    for (const addon::Source_entry& entry : *entries) {
        if (entry.kind == addon::Source_entry_kind::DIRECTORY) {
            continue;
        }
        if (entry.relative_path == addon::k_bootstrap_file_name || entry.relative_path == addon::k_index_file_name) {
            continue;
        }
        if (entry.kind != addon::Source_entry_kind::REGULAR_FILE) {
            return fail("not a regular file: " + entry.relative_path);
        }
        if (!addon::is_valid_package_relative_path(entry.relative_path)) {
            return fail("not a canonical package-relative path: " + entry.relative_path);
        }
        addon::Source_error read_error;
        const std::optional<std::string> bytes = source.read(entry.relative_path, addon::limits::k_max_file_bytes, &read_error);
        if (!bytes.has_value()) {
            return fail(read_error.detail);
        }

        addon::Index_entry indexed;
        indexed.path       = entry.relative_path;
        indexed.bytes      = bytes->size();
        indexed.sha256_hex = addon::to_hex(addon::Sha256::digest(*bytes));
        indexed.kind       = *addon::resource_kind_for_path(entry.relative_path);
        indexed.origin     = addon::Resource_origin::PACKAGE;
        for (const Third_party_declaration& declaration : third_party) {
            if (indexed.path == declaration.resource || indexed.path == declaration.notice || indexed.path == declaration.provenance) {
                indexed.origin     = addon::Resource_origin::THIRD_PARTY;
                indexed.notice     = declaration.notice;
                indexed.provenance = declaration.provenance;
            }
        }
        index.files.push_back(std::move(indexed));
    }

    for (const Third_party_declaration& declaration : third_party) {
        for (const std::string* path : {&declaration.resource, &declaration.notice, &declaration.provenance}) {
            if (index.find(*path) == nullptr) {
                return fail("third-party declaration names a file that is not in the package: " + *path);
            }
        }
    }

    std::string text = addon::serialize_integrity_index(index);
    if (!defect.empty()) {
        addon::Json_parse_limits parse_limits;
        parse_limits.max_bytes = addon::limits::k_max_index_bytes;
        std::optional<addon::Json_value> document = addon::parse_json(text, parse_limits, nullptr);
        std::string error;
        if (!document.has_value() || !inject_defect(*document, defect, &error)) {
            return fail(error.empty() ? std::string("cannot re-parse the index for defect injection") : error);
        }
        text = addon::serialize_json(*document);
    }
    else {
        // The tool never writes an index the validator would refuse.
        addon::Contract_failure failure;
        if (!addon::parse_integrity_index(text, &failure).has_value()) {
            return fail("the assembled index is not valid: " + failure.reason_code + ": " + failure.detail);
        }
    }

    std::ofstream output(root / addon::k_index_file_name, std::ios::binary | std::ios::trunc);
    output.write(text.data(), (std::streamsize)text.size());
    output.close();
    if (!output) {
        return fail("cannot write " + addon::path_to_utf8(root / addon::k_index_file_name));
    }
    std::fprintf(stdout, "wrote %s (%zu files)\n", addon::path_to_utf8(root / addon::k_index_file_name).c_str(), index.files.size());
    return 0;
}

#ifdef _WIN32
int wmain(int argc, wchar_t** argv)
{
    return logonomic::cli_addon::tools::run_utf8_arguments(argc, argv, run);
}
#else
int main(int argc, char** argv)
{
    return run(argc, argv);
}
#endif
