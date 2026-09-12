// SPDX-License-Identifier: MIT
#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace aoap {

// Every std::string path in this project is UTF-8. These convert at the
// filesystem boundary so non-ASCII names work on Windows regardless of the
// active code page.
std::filesystem::path utf8_path(std::string_view text);
std::string path_utf8(const std::filesystem::path& path);

// Directory holding the running executable, or empty if it cannot be found.
std::string executable_directory();

// The one csv/ folder every tool shares: next to the executables, in both the
// build tree and a release archive. Recordings are saved here and the player
// lists scripts from here.
std::string script_directory();

// Full paths of the *.csv files in `directory`, sorted by file name.
std::vector<std::string> discover_csv_files(const std::string& directory);

// File name without its directory or extension, for display.
std::string display_name(const std::string& path);

} // namespace aoap
