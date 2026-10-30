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
// 同时支持空格与等号两种参数写法；无配置、评分导出或检查点参数。
SubmissionArguments parse_submission_arguments(const std::vector<std::string> &args) {
    SubmissionArguments result;
    bool have_input = false, have_cores = false, have_output = false;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const auto &arg = args[i];
        if (arg == "--version" || arg == "--help" || arg == "-h") {
            if (args.size() != 1) {
                throw SubmissionUsageError("help/version must be used alone");
            }
            result.version = arg == "--version";
            result.help = !result.version;
            return result;
        }
        if (!arg.empty() && arg.front() == '-') {
            const auto equal = arg.find('=');
            const auto option = arg.substr(0, equal);
            if (option != "-n" && option != "--cores" && option != "-o" && option != "--output") {
                throw SubmissionUsageError("unknown option: " + option);
            }
            std::string value;
            if (equal != std::string::npos) {
                value = arg.substr(equal + 1);
            } else {
                if (++i >= args.size()) {
                    throw SubmissionUsageError("missing value for " + option);
                }
                value = args[i];
                if (!value.empty() && value.front() == '-') {
                    throw SubmissionUsageError("missing value for " + option);
                }
            }
            if (value.empty()) {
                throw SubmissionUsageError("empty value for " + option);
            }
            if (option == "-n" || option == "--cores") {
                if (have_cores) {
                    throw SubmissionUsageError("cores specified more than once");
                }
                have_cores = true;
                std::size_t used = 0;
                try {
                    result.cores = std::stoll(value, &used);
                } catch (const std::exception &) {
                    throw SubmissionUsageError("cores must be an integer from 2 to 5");
                }
                if (used != value.size() || result.cores < 2 || result.cores > 5) {
                    throw SubmissionUsageError("cores must be an integer from 2 to 5");
                }
            } else {
                if (have_output) {
                    throw SubmissionUsageError("output specified more than once");
                }
                have_output = true;
                result.output = path_from_utf8(value);
            }
        } else {
            if (have_input || arg.empty()) {
                throw SubmissionUsageError("exactly one graph path is required");
            }
            have_input = true;
            result.input = path_from_utf8(arg);
        }
    }
    if (!have_input) {
        throw SubmissionUsageError("graph path is required");
    }
    if (!have_output) {
        result.output = result.input.parent_path() /
                        path_from_utf8(path_utf8(result.input.stem()) + "_multicore_res.json");
    }
    // 仅解释用户给出的路径，不查找工作目录中的配置或其它资源。
    std::error_code input_error, output_error, equivalent_error;
    const auto input = fs::absolute(result.input, input_error).lexically_normal();
    const auto output = fs::absolute(result.output, output_error).lexically_normal();
    if ((!input_error && !output_error && input == output) ||
        fs::equivalent(result.input, result.output, equivalent_error)) {
        throw SubmissionUsageError("output must not overwrite the input graph");
    }
    return result;
}

} // namespace problem2
