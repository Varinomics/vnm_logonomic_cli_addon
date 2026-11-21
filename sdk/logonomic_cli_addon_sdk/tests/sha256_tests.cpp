#include "sdk_test_support.h"

#include "logonomic_cli_addon/generation_digest.h"
#include "logonomic_cli_addon/integrity_index.h"
#include "logonomic_cli_addon/sha256.h"

#include <string>

namespace sdk_tests {

namespace {

namespace addon = logonomic::cli_addon;

std::string hex_of(std::string_view bytes)
{
    return addon::to_hex(addon::Sha256::digest(bytes));
}

} // namespace

bool test_sha256_fips_vectors()
{
    bool ok = true;
    const char* id = __func__;

    // FIPS 180-4 / NIST example vectors.
    ok &= check(hex_of("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", id, "empty message");
    ok &= check(hex_of("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", id, "one-block message");
    ok &= check(hex_of("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
        "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1", id, "two-block message (448 bits)");
    ok &= check(hex_of("abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu") ==
        "cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1", id, "896-bit message");
    ok &= check(hex_of(std::string(1000000, 'a')) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0", id,
        "one million 'a'");

    // Incremental updates in odd chunk sizes equal the one-shot digest.
    {
        const std::string message(1000, 'a');
        addon::Sha256 hash;
        std::size_t offset = 0;
        std::size_t chunk  = 1;
        while (offset < message.size()) {
            const std::size_t take = std::min(chunk, message.size() - offset);
            hash.update(message.data() + offset, take);
            offset += take;
            chunk = chunk * 3 + 1;
        }
        ok &= check(addon::to_hex(hash.finish()) == hex_of(message), id, "incremental digest equals one-shot digest");
    }
    // The million-'a' vector streamed in chunks that straddle block boundaries.
    {
        addon::Sha256 hash;
        const std::string chunk(1000, 'a');
        for (int i = 0; i < 1000; ++i) {
            hash.update(chunk.data() + (i % 7), chunk.size() - (i % 7));   // 993-1000 bytes per update
            hash.update(chunk.data(), (std::size_t)(i % 7));
        }
        ok &= check(addon::to_hex(hash.finish()) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0", id,
            "one million 'a' streamed in uneven chunks");
    }
    {
        addon::Sha256 hash;
        hash.update(std::string(55, 'x'));   // exactly the padding boundary
        const std::string boundary = addon::to_hex(hash.finish());
        ok &= check(boundary == hex_of(std::string(55, 'x')), id, "55-byte boundary");
        ok &= check(hex_of(std::string(56, 'x')) == addon::to_hex(addon::Sha256::digest(std::string(56, 'x'))), id, "56-byte boundary");
        ok &= check(hex_of(std::string(64, 'x')).size() == 64, id, "64-byte block digest renders as 64 hex characters");
    }

    // Hex round trip.
    const addon::Sha256_digest digest = addon::Sha256::digest("abc");
    const std::optional<addon::Sha256_digest> parsed = addon::sha256_from_hex(addon::to_hex(digest));
    ok &= check(parsed.has_value() && *parsed == digest, id, "hex round trip");
    ok &= check(!addon::sha256_from_hex(std::string(64, 'G')).has_value(), id, "non-hex rejected");
    ok &= check(!addon::sha256_from_hex(std::string(63, 'a')).has_value(), id, "short hex rejected");
    ok &= check(!addon::sha256_from_hex(std::string(64, 'A')).has_value(), id, "uppercase hex rejected");
    return ok;
}

bool test_generation_digest_definition()
{
    bool ok = true;
    const char* id = __func__;

    // The definition SHA-256(LP(bootstrap) || LP(index) || D(f_1) || ...)
    // computed independently with Python's hashlib for these inputs.
    const std::string bootstrap = R"({"bootstrap_version":1})";
    const std::string index     = R"({"index_version":1,"files":[]})";
    ok &= check(addon::compute_generation_digest_hex(bootstrap, index, std::vector<addon::Sha256_digest>{}) ==
        "1048de50834b8759944acdbb42bbddd99c594fcb22a0034b8b081bea8262741d", id, "digest without files");

    const std::vector<addon::Sha256_digest> digests = {
        addon::Sha256::digest("alpha"),
        addon::Sha256::digest("beta"),
    };
    ok &= check(addon::compute_generation_digest_hex(bootstrap, index, digests) ==
        "363388f173b3cf73dadec57a7253e3cd5729c7560b4ef2c75a8658179eb476be", id, "digest with two files in order");

    // Order matters: the index order is the definition.
    const std::vector<addon::Sha256_digest> reversed = {digests[1], digests[0]};
    ok &= check(addon::compute_generation_digest_hex(bootstrap, index, reversed) !=
        addon::compute_generation_digest_hex(bootstrap, index, digests), id, "file order changes the digest");

    // The length prefix separates the two length-prefixed inputs.
    const std::vector<addon::Sha256_digest> no_files;
    ok &= check(addon::compute_generation_digest_hex("ab", "c", no_files) != addon::compute_generation_digest_hex("a", "bc", no_files), id,
        "length prefixes keep bootstrap and index bytes distinct");

    // The index overload uses the verified entry digests.
    addon::Integrity_index verified;
    verified.index_version = 1;
    for (const char* content : {"alpha", "beta"}) {
        addon::Index_entry entry;
        entry.path       = std::string("assets/") + content;
        entry.bytes      = std::string(content).size();
        entry.sha256_hex = addon::to_hex(addon::Sha256::digest(content));
        entry.kind       = addon::Resource_kind::ASSET;
        verified.files.push_back(entry);
    }
    ok &= check(addon::compute_generation_digest_hex(bootstrap, index, verified) ==
        "363388f173b3cf73dadec57a7253e3cd5729c7560b4ef2c75a8658179eb476be", id, "index overload matches the raw overload");
    return ok;
}

} // namespace sdk_tests
