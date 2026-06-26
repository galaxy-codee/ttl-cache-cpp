cmake_minimum_required(VERSION 3.16)
project(ttl_cache_cpp CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ── Compiler warnings ────────────────────────────────────────────────────────
if(MSVC)
    add_compile_options(/W4)
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# ── Release build defaults to -O2 ────────────────────────────────────────────
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# ── Include path ─────────────────────────────────────────────────────────────
include_directories(include)

# ── Threads (required for std::thread and std::shared_mutex) ─────────────────
find_package(Threads REQUIRED)

# ── Main benchmark executable ────────────────────────────────────────────────
add_executable(ttl_cache src/main.cpp)
target_link_libraries(ttl_cache PRIVATE Threads::Threads)