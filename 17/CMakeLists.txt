cmake_minimum_required(VERSION 3.16)
project(team17_npu_scheduler VERSION 0.8.4 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 单配置、多配置生成器统一将可执行文件放到 build/bin。
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
foreach(config DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${config} "${CMAKE_BINARY_DIR}/bin")
endforeach()

add_executable(npu_sched
    src/anytime.cpp
    src/chain.cpp
    src/main.cpp
    src/refine.cpp
    src/regions.cpp
    src/slack.cpp
    src/spill.cpp
    src/tensor_neighborhood.cpp
)
target_include_directories(npu_sched PRIVATE src)

if(MSVC)
    target_compile_options(npu_sched PRIVATE /utf-8 /W4)
else()
    target_compile_options(npu_sched PRIVATE -Wall -Wextra -Wpedantic)
endif()
if(UNIX)
    target_compile_options(npu_sched PRIVATE -pthread)
    target_link_options(npu_sched PRIVATE -pthread)
endif()
if(MINGW)
    target_link_options(npu_sched PRIVATE -municode -static -static-libgcc -static-libstdc++)
endif()
