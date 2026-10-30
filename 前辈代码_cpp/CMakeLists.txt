cmake_minimum_required(VERSION 3.20)
project(problem2_cpp LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
find_package(Threads REQUIRED)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/bin")
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
    problem2/pipeline.cpp
    problem2/activation.cpp
    problem2/structure.cpp
    problem2/official_code/multicore_cut_evaluate_problem_1.cpp
    problem2/official_code/multicore_cut_evaluate_problem_3.cpp
    problem2/official_code/singlecore_evaluate.cpp)
target_include_directories(problem2_lib PUBLIC problem2)
target_link_libraries(problem2_lib PUBLIC Threads::Threads)
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
    target_compile_options(problem2_lib PUBLIC /utf-8 PRIVATE /W4)
else()
    target_compile_options(problem2_lib PRIVATE -Wall -Wextra -Wpedantic -pedantic-errors)
endif()
add_executable(parity_runner tests/parity_runner.cpp)
target_link_libraries(parity_runner PRIVATE problem2_lib)
foreach(problem RANGE 1 3)
    add_executable(multicore_cut_evaluate_problem_${problem} problem2/official_code/cli_main.cpp)
    target_compile_definitions(multicore_cut_evaluate_problem_${problem} PRIVATE EVALUATION_PROBLEM=${problem})
    target_link_libraries(multicore_cut_evaluate_problem_${problem} PRIVATE problem2_lib)
endforeach()
add_executable(singlecore_evaluate problem2/official_code/cli_main.cpp)
target_compile_definitions(singlecore_evaluate PRIVATE EVALUATION_PROBLEM=0)
target_link_libraries(singlecore_evaluate PRIVATE problem2_lib)
add_executable(stub_multicore_cut_and_schedule problem2/official_code/cli_main.cpp)
target_compile_definitions(stub_multicore_cut_and_schedule PRIVATE EVALUATION_PROBLEM=-1)
target_link_libraries(stub_multicore_cut_and_schedule PRIVATE problem2_lib)
add_executable(problem2 problem2/main.cpp)
target_link_libraries(problem2 PRIVATE problem2_lib)
if(MINGW)
    foreach(target problem2 multicore_cut_evaluate_problem_1 multicore_cut_evaluate_problem_2 multicore_cut_evaluate_problem_3 singlecore_evaluate stub_multicore_cut_and_schedule)
        target_link_options(${target} PRIVATE -municode -static -static-libgcc -static-libstdc++)
    endforeach()
    target_link_options(parity_runner PRIVATE -static -static-libgcc -static-libstdc++)
endif()

include(CTest)
if(BUILD_TESTING)
    add_executable(generator_test tests/generator_test.cpp)
    target_link_libraries(generator_test PRIVATE problem2_lib)
    if(MINGW)
        target_link_options(generator_test PRIVATE -static -static-libgcc -static-libstdc++)
    endif()
    add_test(NAME generator_protocol COMMAND generator_test)
    set_tests_properties(generator_protocol PROPERTIES TIMEOUT 15)
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(Python3_FOUND AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../前辈代码/problem2/main.py")
        foreach(test compare_multicore compare_search compare_workflow check_hackathon)
            add_test(NAME ${test} COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/${test}.py")
            set_tests_properties(${test} PROPERTIES ENVIRONMENT
                "PROBLEM2_RUNNER=$<TARGET_FILE:parity_runner>;PROBLEM2_BIN=$<TARGET_FILE_DIR:problem2>;PROBLEM2_BUILD=${CMAKE_CURRENT_BINARY_DIR}")
        endforeach()
    endif()
endif()
