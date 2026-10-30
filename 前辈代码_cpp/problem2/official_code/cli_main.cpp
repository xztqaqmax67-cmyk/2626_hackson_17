#include "contest_io.hpp"
#include "../utf8_entry.hpp"
// 同一薄入口用于场景 B 独立评估器和随机示例方案生成器。
int cli_entry(int argc, char **argv) {
#ifdef MULTICORE_STUB_ENTRY
    return problem2::run_multicore_stub_cli(argc, argv);
#else
    return problem2::run_problem_cli(argc, argv);
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
