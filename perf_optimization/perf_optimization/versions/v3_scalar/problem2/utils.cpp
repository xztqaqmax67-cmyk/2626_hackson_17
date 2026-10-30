// 对应 utils.py。
// UTF-8 文本和 JSON 读写、原子替换检查点及 SHA-256 指纹。
#include "utils.hpp"
#include "third_party/picosha2.h"
#include <atomic>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif
namespace problem2 {
std::string read_text(const fs::path &path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("cannot open " + path.string());
    }
    return {std::istreambuf_iterator<char>(input), {}};
}
void write_text(const fs::path &path, const std::string &text) {
    if (!path.parent_path().empty()) {
        fs::create_directories(path.parent_path());
    }
    std::ofstream out(path, std::ios::binary);
    out << text;
    if (!out) {
        throw std::runtime_error("cannot write " + path.string());
    }
}
Json read_json(const fs::path &path) {
    return fs::is_regular_file(path) ? Json::parse(read_text(path)) : object();
}
Json read_json_strict(const fs::path &path) {
    std::map<int, std::set<std::string>> keys;
    auto callback = [&](int depth, Json::parse_event_t event, Json &value) {
        if (event == Json::parse_event_t::object_start) {
            keys[depth + 1].clear();
        }
        if (event == Json::parse_event_t::key &&
            !keys[depth].insert(value.get<std::string>()).second) {
            throw std::invalid_argument("duplicate JSON key " + value.dump());
        }
        return true;
    };
    return Json::parse(read_text(path), callback);
}
void atomic_write(const fs::path &path, const std::string &text) {
    static std::atomic<unsigned long long> counter{0};
    if (!path.parent_path().empty()) {
        fs::create_directories(path.parent_path());
    }
    auto temporary = path;
    temporary += std::string(".tmp.") +
                 std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "." +
                 std::to_string(counter++);
    try {
        write_text(temporary, text);
#ifdef _WIN32
        HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) {
            throw std::runtime_error("cannot flush checkpoint");
        }
        bool flushed = FlushFileBuffers(file);
        CloseHandle(file);
        if (!flushed) {
            throw std::runtime_error("checkpoint flush failed");
        }
        if (!MoveFileExW(temporary.c_str(), path.c_str(),
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            throw std::runtime_error("checkpoint replacement failed");
        }
#else
        int fd = ::open(temporary.c_str(), O_WRONLY);
        if (fd < 0) {
            throw std::runtime_error("cannot flush checkpoint");
        }
        int result = ::fsync(fd);
        ::close(fd);
        if (result < 0) {
            throw std::runtime_error("checkpoint flush failed");
        }
        fs::rename(temporary, path);
#endif
    } catch (...) {
        std::error_code ignored;
        fs::remove(temporary, ignored);
        throw;
    }
}
void atomic_write(const fs::path &path, const Json &value) {
    atomic_write(path, value.dump(2) + "\n");
}
std::string sha256_text(const std::string &text) {
    return picosha2::hash256_hex_string(text);
}
std::string sha256(const fs::path &path) {
    return sha256_text(read_text(path));
}
} // namespace problem2
