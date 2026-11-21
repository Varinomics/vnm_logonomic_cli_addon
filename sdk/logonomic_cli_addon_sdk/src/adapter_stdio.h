#pragma once

// Internal: the three standard streams as raw byte channels, implemented per
// platform (adapter_stdio_posix.cpp, adapter_stdio_win.cpp). Not part of the
// public SDK surface.

#include <cstddef>
#include <string_view>

namespace logonomic::cli_addon::stdio {

/// Puts stdin and stdout into binary mode where the platform distinguishes
/// one; frames must cross the pipe byte for byte.
void prepare_binary_streams();

/// Blocking read; 0 at end of file, negative on error.
long read_stdin(char* buffer, std::size_t size);

/// Writes every byte or returns false.
bool write_stdout(std::string_view bytes);

/// Best-effort diagnostic output.
void write_stderr(std::string_view bytes);

} // namespace logonomic::cli_addon::stdio
