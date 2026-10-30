#pragma once
#include "common.hpp"
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
namespace problem2 {
inline int utf8_entry(int argc, wchar_t **argv, const std::function<int(int, char **)> &run) {
    SetConsoleOutputCP(CP_UTF8);
    std::vector<std::string> storage;
    storage.reserve(argc);
    for (int i = 0; i < argc; ++i) {
        int size = WideCharToMultiByte(CP_UTF8, 0, argv[i], -1, nullptr, 0, nullptr, nullptr);
        std::string value(size, '\0');
        WideCharToMultiByte(CP_UTF8, 0, argv[i], -1, value.data(), size, nullptr, nullptr);
        value.pop_back();
        storage.push_back(std::move(value));
    }
    std::vector<char *> pointers;
    for (auto &value : storage) {
        pointers.push_back(value.data());
    }
    pointers.push_back(nullptr);
    // argv[0] 在通过 PATH 启动时可能只是程序名；子进程需要完整可执行文件路径。
    std::vector<wchar_t> executable(32768);
    DWORD length = GetModuleFileNameW(nullptr, executable.data(), DWORD(executable.size()));
    if (length > 0 && length < executable.size()) {
        storage[0] = path_utf8(fs::path(std::wstring(executable.data(), length)));
        pointers[0] = storage[0].data();
    }
    return run(argc, pointers.data());
}
} // namespace problem2
#else
namespace problem2 {
inline int utf8_entry(int argc, char **argv, const std::function<int(int, char **)> &run) {
    // Linux 通过 PATH 启动时，argv[0] 可能只是程序名。使用实际可执行文件路径，
    // 使批量入口和受限时子进程在任意工作目录下都能再次启动本程序。
    std::error_code error;
    const auto executable = fs::read_symlink("/proc/self/exe", error);
    if (!error && argc > 0) {
        auto name = path_utf8(executable);
        argv[0] = name.data();
        return run(argc, argv);
    }
    return run(argc, argv);
}
} // namespace problem2
#endif
