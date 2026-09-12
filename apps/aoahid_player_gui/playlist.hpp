// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace gui {

// Scripts played in order, each a number of loops, with an optional limit on
// the whole run. Saved as playlists/<name>.playlist next to the program.
struct Playlist {
    struct Entry {
        std::string script; // file name inside csv/, or a full path elsewhere
        int64_t loops{1};
        bool operator==(const Entry&) const = default;
    };
    std::string name;
    std::vector<Entry> entries;
    int time_limit_minutes{}; // 0 = no limit

    bool operator==(const Playlist&) const = default;
};

std::filesystem::path playlist_directory();
// Names of the saved playlists, sorted.
std::vector<std::string> list_playlists();
bool load_playlist(const std::string& name, Playlist& out, std::string& error);
// Creates the directory when needed; written through a temporary file.
bool save_playlist(const Playlist& playlist, std::string& error);
bool delete_playlist(const std::string& name, std::string& error);

// How an entry refers to `path`: the bare file name when the script is in
// csv/, the full path otherwise; and back again.
std::string script_reference(const std::string& path);
std::string resolve_script_reference(const std::string& reference);

} // namespace gui
