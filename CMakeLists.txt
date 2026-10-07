cmake_minimum_required(VERSION 3.22.1)
project(hw80pro_spoof CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# ---------------------------------------------------------------------------
# 纯静态、无第三方依赖: 只用 libc / libdl / liblog / libandroid。
# 不要引入 c++_shared, 否则模块会把 libc++_shared.so 带进目标进程。
# ---------------------------------------------------------------------------
set(CMAKE_CXX_FLAGS_RELEASE "-O2 -DNDEBUG -fno-exceptions -fno-rtti")
set(CMAKE_CXX_FLAGS_DEBUG   "-O0 -g -fno-exceptions -fno-rtti")

# ---------------------------------------------------------------------------
# 把默认配置和 cpuinfo 模板嵌进二进制(不需要 C++ 标准库的文件 IO)
# ---------------------------------------------------------------------------
set(EMBED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(EMBED_SRC "${EMBED_DIR}/profile_data.cpp")
file(MAKE_DIRECTORY "${EMBED_DIR}")

find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
    add_custom_command(
        OUTPUT "${EMBED_SRC}"
        COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tools/embed_profile.py"
                "${EMBED_DIR}"
                "${CMAKE_CURRENT_SOURCE_DIR}/config/hw80pro.conf"
                "${CMAKE_CURRENT_SOURCE_DIR}/config/proc_cpuinfo.txt"
        DEPENDS
            "${CMAKE_CURRENT_SOURCE_DIR}/config/hw80pro.conf"
            "${CMAKE_CURRENT_SOURCE_DIR}/config/proc_cpuinfo.txt"
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/embed_profile.py"
        COMMENT "生成嵌入的配置数据 profile_data.cpp"
        VERBATIM)
else()
    message(FATAL_ERROR
        "需要 Python3 来生成嵌入配置(profile_data.cpp)。\n"
        "NDK 自带的构建环境一般都能找到 python3; 找不到时请显式指定:\n"
        "  cmake -DPython3_EXECUTABLE=/path/to/python3 ...")
endif()

add_library(hw80pro SHARED
    src/zygisk_module.cpp
    src/companion.cpp
    src/config.cpp
    src/spoof_db.cpp
    src/util.cpp
    src/plt_hook.cpp
    src/hook_property.cpp
    src/hook_file.cpp
    src/hook_misc.cpp
    src/hook_jni.cpp
    "${EMBED_SRC}"
)

target_include_directories(hw80pro PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

target_compile_options(hw80pro PRIVATE
    -Wall -Wextra -Wno-unused-parameter
    -fvisibility=hidden
    -fvisibility-inlines-hidden
    -ffunction-sections -fdata-sections
)

target_compile_definitions(hw80pro PRIVATE HW80PRO_VERSION="1.0.0")

target_link_options(hw80pro PRIVATE
    -Wl,--gc-sections
    -Wl,-z,now
    -Wl,-z,relro
    -Wl,-z,max-page-size=16384   # Android 15+ 16KB 页面要求
)

# 注意: 这里【不能】设 ANDROID_STL=none。
# 模块里有函数内静态对象 (SpoofDb::instance()、entry_impl 里的 static T module
# 和 module_abi), 它们需要 __cxa_guard_acquire/release, 由 libc++abi 提供。
# 保持 NDK 默认的 c++_static 即可 —— 它不会产生对 libc++_shared.so 的依赖,
# 所以不会给目标进程带来任何动态库冲突。
target_link_libraries(hw80pro PRIVATE log dl android)
