#include "adapter_stdio.h"

#include <unistd.h>

#include <cerrno>

namespace logonomic::cli_addon::stdio {

void prepare_binary_streams()
{}

long read_stdin(char* buffer, std::size_t size)
{
    while (true) {
        const ssize_t count = ::read(0, buffer, size);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        return (long)count;
    }
}

bool write_stdout(std::string_view bytes)
{
    std::size_t written = 0;
    while (written < bytes.size()) {
        const ssize_t count = ::write(1, bytes.data() + written, bytes.size() - written);
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        written += (std::size_t)count;
    }
    return true;
}

void write_stderr(std::string_view bytes)
{
    std::size_t written = 0;
    while (written < bytes.size()) {
        const ssize_t count = ::write(2, bytes.data() + written, bytes.size() - written);
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            return;
        }
        written += (std::size_t)count;
    }
}

} // namespace logonomic::cli_addon::stdio
