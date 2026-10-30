// 对应 time_budget.py。
// 管理独立搜索进程、目标截止时间及首次有效解的有限延时。
#include "time_budget.hpp"
#include <thread>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <csignal>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>
#endif
namespace problem2 {
struct ChildProcess::Impl {
    std::optional<int> code;
#ifdef _WIN32
    HANDLE process = nullptr, job = nullptr;
#else
    pid_t pid = -1;
#endif
};
ChildProcess::ChildProcess(const fs::path &executable, const std::vector<std::string> &args,
                           const fs::path &log)
    : impl_(std::make_unique<Impl>()) {
#ifdef _WIN32
    auto quote = [](const std::wstring &value) {
        std::wstring out = L"\"";
        std::size_t slashes = 0;
        for (wchar_t c : value) {
            if (c == L'\\') {
                ++slashes;
                continue;
            }
            if (c == L'"') {
                out.append(slashes * 2 + 1, L'\\');
                out += c;
            } else {
                out.append(slashes, L'\\');
                out += c;
            }
            slashes = 0;
        }
        out.append(slashes * 2, L'\\');
        out += L'"';
        return out;
    };
    std::wstring command = quote(executable.wstring());
    for (const auto &arg : args) {
        int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, arg.data(), int(arg.size()),
                                        nullptr, 0);
        if (!count && !arg.empty()) {
            throw std::runtime_error("invalid UTF-8 argument");
        }
        std::wstring wide(count, L'\0');
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, arg.data(), int(arg.size()), wide.data(),
                            count);
        command += L" " + quote(wide);
    }
    SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE output = CreateFileW(log.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &security,
                                CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (output == INVALID_HANDLE_VALUE) {
        throw std::runtime_error("cannot create worker log");
    }
    HANDLE input = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &security,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = input;
    startup.hStdOutput = output;
    startup.hStdError = output;
    PROCESS_INFORMATION process{};
    // Job 对象只包含本次求解进程，关闭时不会影响用户的其他程序。
    impl_->job = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!impl_->job || !SetInformationJobObject(impl_->job, JobObjectExtendedLimitInformation,
                                                &limits, sizeof(limits))) {
        CloseHandle(output);
        CloseHandle(input);
        if (impl_->job) {
            CloseHandle(impl_->job);
        }
        throw std::runtime_error("cannot create worker job");
    }
    BOOL ok =
        CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, TRUE,
                       CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, nullptr, &startup, &process);
    CloseHandle(output);
    CloseHandle(input);
    if (!ok) {
        CloseHandle(impl_->job);
        throw std::runtime_error("cannot start search worker");
    }
    impl_->process = process.hProcess;
    if (!AssignProcessToJobObject(impl_->job, impl_->process)) {
        TerminateProcess(impl_->process, 1);
        CloseHandle(process.hThread);
        CloseHandle(impl_->process);
        CloseHandle(impl_->job);
        throw std::runtime_error("cannot assign search worker to job");
    }
    ResumeThread(process.hThread);
    CloseHandle(process.hThread);
#else
    impl_->pid = fork();
    if (impl_->pid < 0) {
        throw std::runtime_error("cannot fork search worker");
    }
    if (impl_->pid == 0) {
        setsid();
        int out = ::open(log.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600),
            in = ::open("/dev/null", O_RDONLY);
        if (out < 0 || in < 0) {
            _exit(127);
        }
        dup2(out, STDOUT_FILENO);
        dup2(out, STDERR_FILENO);
        dup2(in, STDIN_FILENO);
        ::close(out);
        ::close(in);
        std::vector<std::string> strings = {executable.string()};
        strings.insert(strings.end(), args.begin(), args.end());
        std::vector<char *> raw;
        for (auto &s : strings) {
            raw.push_back(s.data());
        }
        raw.push_back(nullptr);
        execv(executable.c_str(), raw.data());
        _exit(127);
    }
#endif
}
std::optional<int> ChildProcess::poll() {
    if (impl_->code) {
        return impl_->code;
    }
#ifdef _WIN32
    if (WaitForSingleObject(impl_->process, 0) == WAIT_OBJECT_0) {
        DWORD code;
        GetExitCodeProcess(impl_->process, &code);
        impl_->code = int(code);
    }
#else
    int status = 0;
    auto waited = waitpid(impl_->pid, &status, WNOHANG);
    if (waited == impl_->pid) {
        impl_->code = WIFEXITED(status) ? WEXITSTATUS(status) : -WTERMSIG(status);
    }
#endif
    return impl_->code;
}
std::optional<int> ChildProcess::wait(double seconds) {
    double until = monotonic() + std::max(0., seconds);
    do {
        if (auto code = poll()) {
            return code;
        }
        if (monotonic() >= until) {
            break;
        }
        std::this_thread::sleep_for(
            std::chrono::duration<double>(std::min(.02, std::max(0., until - monotonic()))));
    } while (true);
    return {};
}
void ChildProcess::stop() {
    if (poll()) {
        return;
    }
#ifdef _WIN32
    TerminateJobObject(impl_->job, 137);
    WaitForSingleObject(impl_->process, INFINITE);
#else
    kill(-impl_->pid, SIGKILL);
    int status;
    waitpid(impl_->pid, &status, 0);
    impl_->code = -SIGKILL;
#endif
    poll();
}
ChildProcess::~ChildProcess() {
    if (!impl_) {
        return;
    }
    stop();
#ifdef _WIN32
    CloseHandle(impl_->process);
    CloseHandle(impl_->job);
#endif
}
WaitResult wait_for_search(ChildProcess &child, double target, double hard,
                           const fs::path &checkpoint, const std::function<bool()> &interrupted) {
    auto until = [&](double deadline) -> std::optional<int> {
        while (true) {
            if (interrupted()) {
                return {};
            }
            if (auto code = child.wait(std::min(.1, std::max(0., deadline - monotonic())))) {
                return code;
            }
            if (monotonic() >= deadline) {
                return {};
            }
        }
    };
    if (auto code = until(target)) {
        return {code, false, false, false};
    }
    if (interrupted()) {
        return {{}, true, false, true};
    }
    if (fs::exists(checkpoint) || hard <= monotonic()) {
        return {{}, true, false, false};
    }
    while (monotonic() < hard) {
        if (auto code = until(std::min(monotonic() + .2, hard))) {
            return {code, false, true, false};
        }
        if (interrupted()) {
            return {{}, true, true, true};
        }
        if (fs::exists(checkpoint)) {
            if (auto code = until(std::min(monotonic() + .5, hard))) {
                return {code, false, true, false};
            }
            return {{}, true, true, interrupted()};
        }
    }
    return {{}, true, true, false};
}
std::map<Id, double> core_targets(const Ids &cores, double budget,
                                  std::optional<double> time_limit) {
    Id total = 0;
    for (Id n : cores) {
        total += std::max<Id>(1, n - 1);
    }
    std::map<Id, double> result;
    for (Id n : cores) {
        result[n] = time_limit ? *time_limit : budget * std::max<Id>(1, n - 1) / total;
    }
    return result;
}
std::tuple<double, double, double> case_slot_limits(double slot_start, double now, double target,
                                                    double case_deadline, double future_budget,
                                                    double grace_limit) {
    double reserve = std::min(3., std::max(.1, target * .05)),
           allowance = std::min(slot_start + target, case_deadline - future_budget) - now - reserve,
           grace = std::max(0., std::min(grace_limit, case_deadline - future_budget - now -
                                                          allowance - reserve));
    return {allowance, grace, reserve};
}
} // namespace problem2
