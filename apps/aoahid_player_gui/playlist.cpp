// SPDX-License-Identifier: MIT
#include "playlist.hpp"

#include "aoahid_player/paths.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string_view>
#include <system_error>

namespace gui {
namespace {

constexpr const char* extension = ".playlist";

std::string_view trim(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t'))
        text.remove_prefix(1);
    while (!text.empty() &&
           (text.back() == ' ' || text.back() == '\t' || text.back() == '\r'))
        text.remove_suffix(1);
    return text;
}

std::filesystem::path file_for(const std::string& name) {
    return playlist_directory() / aoap::utf8_path(name + extension);
}

bool read_count(const std::string_view text, const long minimum, const long maximum,
                long& out) {
    const std::string copy(trim(text));
    if (copy.empty())
        return false;
    errno = 0;
    char* end = nullptr;
    const long value = std::strtol(copy.c_str(), &end, 10);
    if (errno != 0 || *end != '\0' || value < minimum || value > maximum)
        return false;
    out = value;
    return true;
}

} // namespace

std::filesystem::path playlist_directory() {
    return aoap::utf8_path(aoap::executable_directory()) / "playlists";
}

std::vector<std::string> list_playlists() {
    std::vector<std::string> names;
    std::error_code code;
    for (const auto& item : std::filesystem::directory_iterator(playlist_directory(), code)) {
        if (!item.is_regular_file(code))
            continue;
        const std::filesystem::path& path = item.path();
        if (path.extension() == extension)
            names.push_back(aoap::path_utf8(path.stem()));
    }
    std::sort(names.begin(), names.end());
    return names;
}

bool load_playlist(const std::string& name, Playlist& out, std::string& error) {
    std::ifstream file(file_for(name), std::ios::binary);
    if (!file) {
        error = "cannot open the playlist \"" + name + "\"";
        return false;
    }
    Playlist playlist;
    playlist.name = name;
    std::string line;
    while (std::getline(file, line)) {
        const std::string_view view = trim(line);
        if (view.empty() || view.front() == '#')
            continue;
        const size_t equals = view.find('=');
        if (equals == std::string_view::npos)
            continue;
        const std::string_view key = trim(view.substr(0, equals));
        const std::string_view value = trim(view.substr(equals + 1));
        if (key == "time_limit_minutes") {
            long minutes = 0;
            if (read_count(value, 0, 100000, minutes))
                playlist.time_limit_minutes = static_cast<int>(minutes);
        } else if (key == "play") {
            // "<script>, <loops>"; the script name itself may contain commas.
            const size_t comma = value.rfind(',');
            Playlist::Entry entry;
            long loops = 1;
            if (comma != std::string_view::npos &&
                read_count(value.substr(comma + 1), 1, 1000000, loops))
                entry.script = std::string(trim(value.substr(0, comma)));
            else
                entry.script = std::string(value);
            entry.loops = loops;
            if (!entry.script.empty())
                playlist.entries.push_back(std::move(entry));
        }
    }
    out = std::move(playlist);
    return true;
}

bool save_playlist(const Playlist& playlist, std::string& error) {
    std::error_code code;
    std::filesystem::create_directories(playlist_directory(), code);
    std::ostringstream text;
    text << "# AOA HID Player playlist, saved automatically.\n";
    text << "time_limit_minutes = " << playlist.time_limit_minutes << '\n';
    for (const Playlist::Entry& entry : playlist.entries)
        text << "play = " << entry.script << ", " << entry.loops << '\n';

    const std::filesystem::path path = file_for(playlist.name);
    std::filesystem::path temporary = path;
    temporary += ".tmp";
    {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        const std::string data = text.str();
        file.write(data.data(), static_cast<std::streamsize>(data.size()));
        file.flush();
        if (!file) {
            error = "cannot write " + aoap::path_utf8(temporary);
            return false;
        }
    }
    std::filesystem::rename(temporary, path, code);
    if (code) {
        std::filesystem::remove(temporary, code);
        error = "cannot replace " + aoap::path_utf8(path);
        return false;
    }
    return true;
}

bool delete_playlist(const std::string& name, std::string& error) {
    std::error_code code;
    if (!std::filesystem::remove(file_for(name), code) || code) {
        error = "cannot delete the playlist \"" + name + "\"";
        return false;
    }
    return true;
}

std::string script_reference(const std::string& path) {
    const std::filesystem::path file = aoap::utf8_path(path);
    std::error_code code;
    const std::filesystem::path folder =
        std::filesystem::weakly_canonical(aoap::utf8_path(aoap::script_directory()), code);
    const std::filesystem::path parent =
        std::filesystem::weakly_canonical(file.parent_path(), code);
    if (!code && parent == folder)
        return aoap::path_utf8(file.filename());
    return path;
}

std::string resolve_script_reference(const std::string& reference) {
    const std::filesystem::path path = aoap::utf8_path(reference);
    if (path.has_parent_path())
        return reference;
    return aoap::path_utf8(aoap::utf8_path(aoap::script_directory()) / path);
}

} // namespace gui
