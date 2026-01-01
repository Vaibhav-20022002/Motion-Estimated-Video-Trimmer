cmake_minimum_required(VERSION 3.16)
project(motion_cut LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Optimization flags
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -march=native -mtune=native -flto")

# Find required packages
find_package(PkgConfig REQUIRED)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET
    libavcodec
    libavformat
    libavutil
)

# Find fmt (required for fmtlog)
find_package(fmt REQUIRED)

# Main executable
add_executable(motion_cut motion_vector_DVR_trimmer.cc)

# Include fmtlog headers (header-only library)
target_include_directories(motion_cut PRIVATE /usr/local/include)

target_link_libraries(motion_cut PRIVATE
    PkgConfig::FFMPEG
    fmt::fmt
    pthread
)

# Link with jemalloc if available
find_library(JEMALLOC_LIB jemalloc)
if(JEMALLOC_LIB)
    target_link_libraries(motion_cut PRIVATE ${JEMALLOC_LIB})
endif()
