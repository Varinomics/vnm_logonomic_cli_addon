#include "adapter_stdio.h"

#include <windows.h>

#include <algorithm>

namespace logonomic::cli_addon::stdio {

namespace {

bool write_all(HANDLE handle, std::string_view bytes)
{
    std::size_t written = 0;
    while (written < bytes.size()) {
        DWORD count = 0;
        const DWORD chunk = (DWORD)std::min<std::size_t>(bytes.size() - written, 1u << 20);
        if (!::WriteFile(handle, bytes.data() + written, chunk, &count, nullptr)) {
            return false;
        }
        written += count;
    }
    return true;
}

} // namespace

void prepare_binary_streams()
{
    // The runtime bypasses the C runtime streams and uses the Win32 handles
    // directly, so no text-mode translation applies.
}

long read_stdin(char* buffer, std::size_t size)
{
    DWORD count = 0;
    const DWORD chunk = (DWORD)std::min<std::size_t>(size, 1u << 20);
    if (!::ReadFile(::GetStdHandle(STD_INPUT_HANDLE), buffer, chunk, &count, nullptr)) {
        const DWORD error = ::GetLastError();
        // A broken or closed pipe is end of file for a protocol stream.
        if (error == ERROR_BROKEN_PIPE || error == ERROR_HANDLE_EOF || error == ERROR_NO_DATA) {
            return 0;
        }
        return -1;
    }
    return (long)count;
}

bool write_stdout(std::string_view bytes)
{
    return write_all(::GetStdHandle(STD_OUTPUT_HANDLE), bytes);
}

void write_stderr(std::string_view bytes)
{
    write_all(::GetStdHandle(STD_ERROR_HANDLE), bytes);
}

} // namespace logonomic::cli_addon::stdio
