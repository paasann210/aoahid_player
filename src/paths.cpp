// SPDX-License-Identifier: MIT
#include "aoahid_player/paths.hpp"

#include <algorithm>
#include <system_error>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace aoap {

std::filesystem::path utf8_path(const std::string_view text) {
#if defined(_WIN32)
    if (text.empty())
        return {};
    const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                           nullptr, 0);
    std::wstring wide(static_cast<size_t>(length > 0 ? length : 0), L'\0');
    if (length > 0) {
        MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(),
                            length);
    }
    return std::filesystem::path(wide);
#else
    return std::filesystem::path(std::string(text));
#endif
}

std::string path_utf8(const std::filesystem::path& path) {
#if defined(_WIN32)
    const std::wstring& wide = path.native();
    if (wide.empty())
        return {};
    const int length = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
                                           nullptr, 0, nullptr, nullptr);
    std::string text(static_cast<size_t>(length > 0 ? length : 0), '\0');
    if (length > 0) {
        WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), text.data(),
                            length, nullptr, nullptr);
    }
    return text;
#else
    return path.string();
#endif
}

std::string executable_directory() {
#if defined(_WIN32)
    std::wstring buffer(MAX_PATH, L'\0');
    while (true) {
        const DWORD length =
            GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0)
            return {};
        if (length < buffer.size()) {
            buffer.resize(length);
            break;
        }
        buffer.resize(buffer.size() * 2);
    }
    return path_utf8(std::filesystem::path(buffer).parent_path());
#else
    std::error_code code;
    const std::filesystem::path self = std::filesystem::read_symlink("/proc/self/exe", code);
    if (code)
        return {};
    return path_utf8(self.parent_path());
#endif
}

std::string script_directory() {
    const std::string base = executable_directory();
    if (base.empty())
        return "csv";
    return path_utf8(utf8_path(base) / "csv");
}

std::vector<std::string> discover_csv_files(const std::string& directory) {
    std::vector<std::string> files;
    std::error_code code;
    const std::filesystem::path root = utf8_path(directory);
    if (directory.empty() || !std::filesystem::is_directory(root, code))
        return files;
    for (std::filesystem::directory_iterator it(root, code), end; !code && it != end;
         it.increment(code)) {
        if (!it->is_regular_file(code))
            continue;
        std::string extension = path_utf8(it->path().extension());
        std::transform(extension.begin(), extension.end(), extension.begin(),
                       [](const unsigned char c) {
                           return static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c);
                       });
        if (extension == ".csv")
            files.push_back(path_utf8(it->path()));
    }
    std::sort(files.begin(), files.end());
    return files;
}

std::string display_name(const std::string& path) {
    return path_utf8(utf8_path(path).stem());
}

} // namespace aoap
