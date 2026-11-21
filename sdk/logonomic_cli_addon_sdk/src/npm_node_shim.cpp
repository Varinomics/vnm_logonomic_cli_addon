/*
npm generated Windows Node launchers
Source: https://github.com/npm/cmd-shim

The ISC License

Copyright (c) npm, Inc. and Contributors

Permission to use, copy, modify, and/or distribute this software for any
purpose with or without fee is hereby granted, provided that the above
copyright notice and this permission notice appear in all copies.

THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR
IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
*/

#include "logonomic_cli_addon/launcher_utilities.h"
#include "logonomic_cli_addon/package_source.h"

#include <algorithm>
#include <array>
#include <fstream>

namespace logonomic::cli_addon {
namespace {

constexpr std::string_view k_npm_cmd_shim = R"npm(@ECHO off
GOTO start
:find_dp0
SET dp0=%~dp0
EXIT /b
:start
SETLOCAL
CALL :find_dp0

IF EXIST "%dp0%\node.exe" (
  SET "_prog=%dp0%\node.exe"
) ELSE (
  SET "_prog=node"
  SET PATHEXT=%PATHEXT:;.JS;=;%
)

endLocal & goto #_undefined_# 2>NUL || title %COMSPEC% & "%_prog%"  "%dp0%\@ENTRY@" %*
)npm";

constexpr std::string_view k_npm_powershell_shim = R"npm(#!/usr/bin/env pwsh
$basedir=Split-Path $MyInvocation.MyCommand.Definition -Parent

$exe=""
if ($PSVersionTable.PSVersion -lt "6.0" -or $IsWindows) {
  # Fix case when both the Windows and Linux builds of Node
  # are installed in the same directory
  $exe=".exe"
}
$ret=0
if (Test-Path "$basedir/node$exe") {
  # Support pipeline input
  if ($MyInvocation.ExpectingInput) {
    $input | & "$basedir/node$exe"  "$basedir/@ENTRY@" $args
  } else {
    & "$basedir/node$exe"  "$basedir/@ENTRY@" $args
  }
  $ret=$LASTEXITCODE
} else {
  # Support pipeline input
  if ($MyInvocation.ExpectingInput) {
    $input | & "node$exe"  "$basedir/@ENTRY@" $args
  } else {
    & "node$exe"  "$basedir/@ENTRY@" $args
  }
  $ret=$LASTEXITCODE
}
exit $ret
)npm";

} // namespace

std::optional<std::filesystem::path> npm_node_shim_entry(const std::filesystem::path& shim)
{
    const auto extension = lower(path_to_utf8(shim.extension()));
    if (!shim.is_absolute() || (extension != ".cmd" && extension != ".ps1")) {
        return std::nullopt;
    }
    // npm's launchers are small. Reject an oversized or partly read script
    // rather than authenticating a matching prefix with uninspected actions.
    std::ifstream stream(shim, std::ios::binary);
    std::array<char, 16 * 1024> buffer;
    stream.read(buffer.data(), buffer.size());
    if (!stream.eof() || stream.bad()) {
        return std::nullopt;
    }
    std::string bytes(buffer.data(), static_cast<std::size_t>(stream.gcount()));
    for (auto pos = bytes.find("\r\n"); pos != std::string::npos; pos = bytes.find("\r\n", pos + 1)) {
        bytes.erase(pos, 1);
    }
    auto pattern = extension == ".cmd" ? k_npm_cmd_shim : k_npm_powershell_shim;
    auto script = std::string_view(bytes);
    constexpr std::string_view marker = "@ENTRY@";
    std::string_view target;
    for (auto pos = pattern.find(marker); pos != pattern.npos; pos = pattern.find(marker)) {
        if (!script.starts_with(pattern.substr(0, pos))) {
            return std::nullopt;
        }
        script.remove_prefix(pos);
        pattern.remove_prefix(pos + marker.size());
        const auto end = script.find('"');
        if (end == script.npos || end == 0 || (!target.empty() && script.substr(0, end) != target)) {
            return std::nullopt;
        }
        target = script.substr(0, end);
        script.remove_prefix(end);
    }
    if (script != pattern || !is_valid_utf8(target) || target.find('\0') != target.npos ||
        target.find_first_of("%!`$\r\n:") != target.npos)
    {
        return std::nullopt;
    }
    std::string relative(target);
    std::replace(relative.begin(), relative.end(), '\\', '/');
    const auto entry = path_from_utf8(relative);
    if (entry.has_root_path()) {
        return std::nullopt;
    }
    return regular_file(shim.parent_path() / entry);
}

} // namespace logonomic::cli_addon
