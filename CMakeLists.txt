################################################################################
# CMakeLists.txt
# Motion Trim - Motion Vector DVR Optimization
################################################################################

cmake_minimum_required(VERSION 3.16)
project(motion_trim
  VERSION 1.0.0
  DESCRIPTION "Motion Vector-based DVR Video Trimmer"
  LANGUAGES CXX
)

# C++ Standard
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Optimization flags for Release build
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -march=native -mtune=native -flto")

################################################################################
# Dependencies
################################################################################

# Find required packages
find_package(PkgConfig REQUIRED)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET
    libavcodec
    libavformat
    libavutil
)

# Find fmt (required for formatted output)
find_package(fmt REQUIRED)

################################################################################
# Main Library (Core functionality)
################################################################################

set(MOTION_CUT_SOURCES
    src/logging.cpp
    src/system.cpp
    src/task_queue.cpp
    src/memory_io.cpp
    src/motion_scanner.cpp
    src/pipeline.cpp
)

# Create library for core functionality
add_library(motion_trim_lib STATIC ${MOTION_CUT_SOURCES})

target_include_directories(motion_trim_lib
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE
        /usr/local/include
)

target_link_libraries(motion_trim_lib
    PUBLIC
        PkgConfig::FFMPEG
        fmt::fmt
        pthread
)

# Link with jemalloc if available
find_library(JEMALLOC_LIB jemalloc)
if(JEMALLOC_LIB)
    target_link_libraries(motion_trim_lib PUBLIC ${JEMALLOC_LIB})
    message(STATUS "Found jemalloc: ${JEMALLOC_LIB}")
endif()

################################################################################
# Main Executable
################################################################################

add_executable(motion_trim src/main.cpp)

target_link_libraries(motion_trim PRIVATE motion_trim_lib)

################################################################################
# Utility Tools (Optional)
################################################################################

option(BUILD_TOOLS "Build utility tools" OFF)

if(BUILD_TOOLS)
    # Extract Motion Vectors tool
    add_executable(extract_mvs tools/extract_mvs.cpp)
    target_link_libraries(extract_mvs PRIVATE PkgConfig::FFMPEG)

    # Motion Scalar computation tool (requires nlohmann/json)
    # Note: json.hpp must be present in the search path
    add_executable(motion_scalar tools/motion_scalar.cpp)
    target_include_directories(motion_scalar
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
    )
endif()

################################################################################
# Installation
################################################################################

install(TARGETS motion_trim
    RUNTIME DESTINATION bin
)

install(DIRECTORY include/motion_trim
    DESTINATION include
)
