#pragma once

#include "logonomic_cli_addon_native_file_path.h"
#include "logonomic_cli_addon/package_source.h"

#include <filesystem>
#include <fstream>
#include <string>

namespace logonomic::cli_addon_host {

class Supervisor_request_file
{
public:
    bool write(const std::filesystem::path& in_path, const std::string& in_contents, std::string* out_error)
    {
        std::error_code error;
        const auto io_path = native_file_path(in_path);
        std::filesystem::remove(io_path, error);
        m_path = in_path;
        std::ofstream file(io_path, std::ios::binary | std::ios::trunc);
        file.write(in_contents.data(), static_cast<std::streamsize>(in_contents.size()));
        file.close();
        if (!file) {
            *out_error = "could not write " + cli_addon::path_to_utf8(in_path);
            return false;
        }
        return true;
    }

    const std::filesystem::path& path() const { return m_path; }

    ~Supervisor_request_file()
    {
        if (!m_path.empty()) {
            std::error_code error;
            std::filesystem::remove(native_file_path(m_path), error);
        }
    }

private:
    std::filesystem::path m_path;
};

} // namespace logonomic::cli_addon_host
