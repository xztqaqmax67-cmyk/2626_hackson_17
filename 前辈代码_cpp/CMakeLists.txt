cmake_minimum_required(VERSION 3.16)
project(problem2_cpp LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
# 场景 B 的正式目标与开发工具共享同一套算法库。
option(BUILD_TESTING "显式构建本地对照工具；默认不构建测试" OFF)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/bin")
foreach(config IN LISTS CMAKE_CONFIGURATION_TYPES)
    string(TOUPPER "${config}" config_upper)
    set("CMAKE_RUNTIME_OUTPUT_DIRECTORY_${config_upper}" "${CMAKE_CURRENT_BINARY_DIR}/bin")
endforeach()
add_library(problem2_lib
    problem2/official_code/evaluation_validation.cpp
    problem2/official_code/schedule_step1.cpp
    problem2/official_code/schedule_step2.cpp
    problem2/official_code/schedule_step3.cpp
    problem2/official_code/stub_multicore_cut_and_schedule.cpp
    problem2/official_code/multicore_cut_evaluate_problem_2.cpp)
target_sources(problem2_lib PRIVATE
    problem2/batch.cpp
    problem2/time_budget.cpp
    problem2/search.cpp
    problem2/official.cpp
    problem2/portfolio.cpp
    problem2/artifacts.cpp
    problem2/submission_io.cpp
    problem2/inheritance.cpp
    problem2/official_code/contest_io.cpp
    problem2/utils.cpp
    problem2/core.cpp
    problem2/bootstrap.cpp
    problem2/chain.cpp
    problem2/pipeline.cpp
    problem2/activation.cpp
    problem2/structure.cpp)
target_include_directories(problem2_lib PUBLIC problem2)
# 标准线程只保存生成器调用栈；GCC/Clang 使用系统 pthread，无第三方包查找。
# MSVC 的 std::thread 由其运行库提供，不需要额外链接选项。
if(NOT MSVC AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(problem2_lib PUBLIC -pthread)
    target_link_options(problem2_lib PUBLIC -pthread)
endif()
# 把本次构建的官方模块指纹编入程序；拷贝可执行文件后不依赖源码所在位置。
file(GLOB official_sources CONFIGURE_DEPENDS problem2/official_code/*.cpp problem2/official_code/*.hpp)
# 公共硬件模型也影响评分，修改 PIPE 表后必须使旧检查点失效。
list(APPEND official_sources "${CMAKE_CURRENT_SOURCE_DIR}/problem2/common.hpp")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${official_sources})
set(official_manifest "{")
set(separator "")
foreach(source IN LISTS official_sources)
    get_filename_component(name "${source}" NAME)
    file(SHA256 "${source}" digest)
    string(APPEND official_manifest "${separator}\"${name}\":\"${digest}\"")
    set(separator ",")
endforeach()
string(APPEND official_manifest "}")
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/original_official_hashes.json" original_manifest)
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/generated/official_manifest.hpp"
    "#pragma once\nnamespace problem2 {\ninline constexpr char official_manifest[] = R\"manifest(${official_manifest})manifest\";\ninline constexpr char original_official_manifest[] = R\"manifest(${original_manifest})manifest\";\n}\n")
target_include_directories(problem2_lib PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
if(MSVC)
    target_compile_options(problem2_lib PUBLIC /utf-8 /permissive- /Zc:__cplusplus PRIVATE /W4)
else()
    target_compile_options(problem2_lib PRIVATE -Wall -Wextra -Wpedantic -pedantic-errors)
endif()
add_executable(parity_runner EXCLUDE_FROM_ALL tests/parity_runner.cpp)
target_link_libraries(parity_runner PRIVATE problem2_lib)
add_executable(multicore_cut_evaluate_problem_2 EXCLUDE_FROM_ALL problem2/official_code/cli_main.cpp)
target_link_libraries(multicore_cut_evaluate_problem_2 PRIVATE problem2_lib)
add_executable(stub_multicore_cut_and_schedule EXCLUDE_FROM_ALL problem2/official_code/cli_main.cpp)
target_compile_definitions(stub_multicore_cut_and_schedule PRIVATE MULTICORE_STUB_ENTRY=1)
target_link_libraries(stub_multicore_cut_and_schedule PRIVATE problem2_lib)
add_executable(problem2 EXCLUDE_FROM_ALL problem2/main.cpp)
target_link_libraries(problem2 PRIVATE problem2_lib)
# 在原 main.cpp 中选择正式接口；切图、排序、邻域和完整评分仍来自原库。
add_executable(npu_sched problem2/main.cpp)
target_compile_definitions(npu_sched PRIVATE NPU_SCHED_ENTRY=1)
target_link_libraries(npu_sched PRIVATE problem2_lib)
if(MINGW)
    foreach(target npu_sched problem2 multicore_cut_evaluate_problem_2 stub_multicore_cut_and_schedule)
        target_link_options(${target} PRIVATE -municode -static -static-libgcc -static-libstdc++)
    endforeach()
    target_link_options(parity_runner PRIVATE -static -static-libgcc -static-libstdc++)
endif()

if(BUILD_TESTING)
    enable_testing()
    set_target_properties(parity_runner problem2
        multicore_cut_evaluate_problem_2 stub_multicore_cut_and_schedule
        PROPERTIES EXCLUDE_FROM_ALL FALSE)
    add_executable(generator_test tests/generator_test.cpp)
    target_link_libraries(generator_test PRIVATE problem2_lib)
    if(MINGW)
        target_link_options(generator_test PRIVATE -static -static-libgcc -static-libstdc++)
    endif()
    add_test(NAME generator_protocol COMMAND generator_test)
    add_executable(chain_tests tests/chain_tests.cpp)
    target_link_libraries(chain_tests PRIVATE problem2_lib)
    if(MINGW)
        target_link_options(chain_tests PRIVATE -static -static-libgcc -static-libstdc++)
    endif()
    add_test(NAME chain_seed_invariants COMMAND chain_tests)
    set_tests_properties(generator_protocol PROPERTIES TIMEOUT 15)
    # 仅由开发者显式提供测试解释器；正常构建不搜索 Python 或包外资源。
    set(PROBLEM2_TEST_PYTHON "" CACHE FILEPATH "可选：运行原版对照脚本的 Python 路径")
    if(PROBLEM2_TEST_PYTHON)
        add_test(NAME check_official COMMAND "${PROBLEM2_TEST_PYTHON}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/check_official.py")
        set_tests_properties(check_official PROPERTIES ENVIRONMENT
            "PROBLEM2_RUNNER=$<TARGET_FILE:parity_runner>;PROBLEM2_BUILD=${CMAKE_CURRENT_BINARY_DIR}")
    endif()
    if(PROBLEM2_TEST_PYTHON AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../前辈代码/problem2/main.py")
        foreach(test compare_multicore compare_search compare_workflow check_hackathon check_chain_integration)
            add_test(NAME ${test} COMMAND "${PROBLEM2_TEST_PYTHON}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/${test}.py")
            set_tests_properties(${test} PROPERTIES ENVIRONMENT
                "PROBLEM2_RUNNER=$<TARGET_FILE:parity_runner>;PROBLEM2_BIN=$<TARGET_FILE_DIR:problem2>;PROBLEM2_BUILD=${CMAKE_CURRENT_BINARY_DIR}")
        endforeach()
    endif()
endif()
