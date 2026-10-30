#include "contest_io.hpp"
#include "../utf8_entry.hpp"
// 同一薄入口编译成各个独立程序，算法仍在对应的原名文件中。
#ifndef EVALUATION_PROBLEM
#define EVALUATION_PROBLEM 2
#endif
int cli_entry(int argc, char **argv) {
#if EVALUATION_PROBLEM == -1
    return problem2::run_multicore_stub_cli(argc, argv);
#else
    return problem2::run_problem_cli(EVALUATION_PROBLEM, argc, argv);
#endif
}
#ifdef _WIN32
int wmain(int argc, wchar_t **argv) {
    return problem2::utf8_entry(argc, argv, cli_entry);
}
#else
int main(int argc, char **argv) {
    return problem2::utf8_entry(argc, argv, cli_entry);
}
#endif
