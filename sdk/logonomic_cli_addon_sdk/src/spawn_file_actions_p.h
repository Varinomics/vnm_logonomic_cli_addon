#pragma once

#if defined(__APPLE__)
#include <Availability.h>
#endif
#include <spawn.h>

namespace logonomic::cli_addon::detail {

inline int add_spawn_working_directory(
    posix_spawn_file_actions_t* actions,
    const char* directory)
{
    // Earlier deployment targets still need the _np symbol even with a newer SDK.
#if defined(__APPLE__) && __MAC_OS_X_VERSION_MIN_REQUIRED >= 260000 && \
    __MAC_OS_X_VERSION_MAX_ALLOWED >= 260000
    return ::posix_spawn_file_actions_addchdir(actions, directory);
#else
    return ::posix_spawn_file_actions_addchdir_np(actions, directory);
#endif
}

} // namespace logonomic::cli_addon::detail
