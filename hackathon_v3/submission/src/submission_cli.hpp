#pragma once
// Shared source template, copied into each standalone submission project.
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace submission {
namespace fs = std::filesystem;
struct Error : std::runtime_error {
    int code;
    Error(int value, const std::string &message) : std::runtime_error(message), code(value) {}
};
struct Options { fs::path input, output; int cores = 4; bool version = false; };
inline Options parse(int argc, char **argv) {
    Options result;
    if (argc == 2 && std::string(argv[1]) == "--version") {
        result.version = true;
        return result;
    }
    bool seen_cores = false, seen_output = false;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i], name = arg, value;
        auto eq = arg.find('=');
        bool joined = arg.rfind("--", 0) == 0 && eq != std::string::npos;
        if (joined) { name = arg.substr(0, eq); value = arg.substr(eq + 1); }
        if (name == "-n" || name == "--cores" || name == "-o" || name == "--output") {
            if (!joined) {
                if (++i >= argc) throw Error(2, "missing value for " + name);
                value = argv[i];
            }
            if (value.empty() || value.front() == '-') throw Error(2, "invalid value for " + name);
            if (name == "-n" || name == "--cores") {
                if (seen_cores || value.size() != 1 || value[0] < '2' || value[0] > '5')
                    throw Error(2, "cores must be an integer from 2 through 5, specified once");
                seen_cores = true;
                result.cores = value[0] - '0';
            } else {
                if (seen_output) throw Error(2, "output specified more than once");
                seen_output = true;
                result.output = fs::u8path(value);
            }
        } else if (!arg.empty() && arg[0] == '-') {
            throw Error(2, "unknown option: " + arg);
        } else {
            if (!result.input.empty() || arg.empty()) throw Error(2, "expected exactly one input graph");
            result.input = fs::u8path(arg);
        }
    }
    if (result.input.empty()) throw Error(2, "usage: npu_sched graph.json -n N -o plan.json");
    if (result.output.empty()) {
        auto name = result.input.stem();
        name += "_multicore_res.json";
        result.output = result.input.parent_path() / name;
    }
    // Resolve relative names against the invocation only; never search the current directory
    // for config, data, checkpoints, executables, or other project resources.
    std::error_code ec;
    auto in = fs::absolute(result.input, ec).lexically_normal();
    if (ec) throw Error(3, "cannot resolve input path");
    auto out = fs::absolute(result.output, ec).lexically_normal();
    if (ec) throw Error(4, "cannot resolve output path");
    bool same = in == out;
    if (!same) { same = fs::equivalent(in, out, ec); }
    if (same) throw Error(2, "output must not overwrite the input graph");
    result.input = in;
    result.output = out;
    return result;
}
inline std::string read(const fs::path &path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) throw Error(3, "cannot open input graph");
    std::string value{std::istreambuf_iterator<char>(file), {}};
    if (file.bad()) throw Error(3, "cannot read input graph");
    return value;
}
inline void write(const fs::path &path, const std::string &value) {
    std::error_code ec;
    if (!path.parent_path().empty()) fs::create_directories(path.parent_path(), ec);
    if (ec) throw Error(4, "cannot create output directory: " + ec.message());
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) throw Error(4, "cannot create output file");
    file.write(value.data(), static_cast<std::streamsize>(value.size()));
    file.flush();
    if (!file) throw Error(4, "cannot write complete output file");
    file.close();
    if (file.fail()) throw Error(4, "cannot close output file");
}
#ifdef _WIN32
inline std::string utf8(const wchar_t *arg) {
    int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, arg, -1, nullptr, 0, nullptr, nullptr);
    if (size <= 0) throw Error(2, "invalid command-line encoding");
    std::string result(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, arg, -1, result.data(), size, nullptr, nullptr);
    result.pop_back();
    return result;
}
template <class Function> int windows_main(int argc, wchar_t **argv, Function run) {
    try {
        std::vector<std::string> text;
        for (int i = 0; i < argc; ++i) text.push_back(utf8(argv[i]));
        std::vector<char *> args;
        for (auto &s : text) args.push_back(s.data());
        return run(argc, args.data());
    } catch (const Error &e) { std::cerr << e.what() << '\n'; return e.code; }
      catch (const std::exception &e) { std::cerr << e.what() << '\n'; return 1; }
}
#endif
} // namespace submission

#ifdef _WIN32
#define SUBMISSION_MAIN(run) int wmain(int argc, wchar_t **argv) { return submission::windows_main(argc, argv, run); }
#else
#define SUBMISSION_MAIN(run) int main(int argc, char **argv) { return run(argc, argv); }
#endif
