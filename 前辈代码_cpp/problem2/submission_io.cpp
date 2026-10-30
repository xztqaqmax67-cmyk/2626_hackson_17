// 对应 submission_io.py。
// 处理面向用户的错误信息；动态导入在 C++ 中由编译链接替代。
#include "submission_io.hpp"
#include <regex>
namespace problem2 {
std::string safe_message(const std::string &message) {
    // C++ 模块由链接器装载，无需 Python 的动态包导入或 sys.excepthook。
    // 同时兼容原来的 Unix 绝对路径和 Windows 盘符路径，保留文件名。
    std::regex pattern(R"((?:[A-Za-z]:[\\/]|/)(?:[^\s"'<>]+[\\/])*[^\s"'<>]*)");
    std::string result;
    std::size_t consumed = 0;
    for (std::sregex_iterator i(message.begin(), message.end(), pattern), end; i != end; ++i) {
        const auto &match = *i;
        result += message.substr(consumed, std::size_t(match.position()) - consumed);
        auto value = match.str();
        auto split = value.find_last_of("/\\");
        auto filename = split == std::string::npos ? value : value.substr(split + 1);
        result += filename.empty() ? "<directory>" : filename;
        consumed = std::size_t(match.position() + match.length());
    }
    result += message.substr(consumed);
    return result;
}
} // namespace problem2
