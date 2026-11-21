#include "sdk_test_support.h"

#include <chrono>
#include <cstdio>
#include <fstream>
#include <random>
#include <sstream>
#include <system_error>

namespace sdk_tests {

namespace {

std::filesystem::path g_fixture_packages;
std::filesystem::path g_scripts;
std::filesystem::path g_test_executable;

} // namespace

bool check(bool condition, const char* test, const std::string& message)
{
    if (condition) {
        return true;
    }
    std::fprintf(stderr, "FAIL [%s]: %s\n", test, message.c_str());
    return false;
}

const std::filesystem::path& fixture_packages_directory() { return g_fixture_packages; }
const std::filesystem::path& scripts_directory()          { return g_scripts; }
const std::filesystem::path& test_executable()             { return g_test_executable; }

void set_fixture_packages_directory(std::filesystem::path directory) { g_fixture_packages = std::move(directory); }
void set_scripts_directory(std::filesystem::path directory)          { g_scripts = std::move(directory); }
void set_test_executable(std::filesystem::path executable)            { g_test_executable = std::move(executable); }

std::filesystem::path make_temp_directory(std::string_view stem)
{
    static std::mt19937_64 generator(
        (std::uint64_t)std::chrono::steady_clock::now().time_since_epoch().count() ^ std::random_device{}());
    std::error_code error;
    const std::filesystem::path base = std::filesystem::temp_directory_path(error);
    for (int attempt = 0; attempt < 16; ++attempt) {
        const std::filesystem::path candidate =
            base / (std::string("logonomic-cli-addon-sdk-tests-") + std::string(stem) + "-" + std::to_string(generator()));
        if (std::filesystem::create_directories(candidate, error) && !error) {
            return candidate;
        }
    }
    return {};
}

bool write_file(const std::filesystem::path& path, std::string_view bytes)
{
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(bytes.data(), (std::streamsize)bytes.size());
    output.close();
    return output.good();
}

std::optional<std::string> read_file(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return std::nullopt;
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

std::string fake_adapter_bytes()
{
    return std::string("\x7F" "ELF", 4) + "fixture adapter image for validator tests";
}

} // namespace sdk_tests
