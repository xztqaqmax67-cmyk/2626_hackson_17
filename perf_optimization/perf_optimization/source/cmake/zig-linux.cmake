# 可选：在 Windows 上验证 Linux x86-64 编译，不属于正常运行依赖。
# cmake -S . -B build/linux -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/zig-linux.cmake -DZIG_EXECUTABLE=.../zig.exe
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
if(NOT ZIG_EXECUTABLE)
    message(FATAL_ERROR "请通过 -DZIG_EXECUTABLE 指定便携 zig 编译器")
endif()
set(CMAKE_CXX_COMPILER "${ZIG_EXECUTABLE}")
set(CMAKE_CXX_COMPILER_ARG1 "c++ -target x86_64-linux-musl")
set(CMAKE_TRY_COMPILE_PLATFORM_VARIABLES ZIG_EXECUTABLE)
