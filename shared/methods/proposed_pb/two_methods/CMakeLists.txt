cmake_minimum_required(VERSION 3.16)
project(MetroAssignmentMultiScale LANGUAGES CXX)

find_package(Threads REQUIRED)
add_executable(run_all_scales
    src/main.cpp
    src/input.cpp
    src/loading.cpp
    src/proposed.cpp
    src/sar.cpp
    src/report.cpp
)
set_target_properties(run_all_scales PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO)
target_include_directories(run_all_scales PRIVATE include)
target_link_libraries(run_all_scales PRIVATE Threads::Threads)
if(MSVC)
    target_compile_options(run_all_scales PRIVATE /W4 /utf-8)
else()
    target_compile_options(run_all_scales PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(MINGW)
    target_link_options(run_all_scales PRIVATE -municode)
endif()
