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

# * STEP:1 - Detect Cache Line Size of the system :
message(STATUS "Attempting to detect cache line size by compiling & running a utility...")

try_run(
  # These variables will hold the results of the operation
  RUN_RESULT_CODE    ## Exit code of the utility binary
  COMPILE_RESULT     ## SUCCESS (0) or FAILURE(non-zero)

  # The directory where the test binary will be built. Must be an absolute path.
  "${CMAKE_BINARY_DIR}/cacheLineTest"

  # The source file to compile and run. Must be an absolute path.
  "${CMAKE_SOURCE_DIR}/tools/cacheLineSize.cpp"

  # This option tells try_run to store the standard output from the program
  # in the specified variable.
  RUN_OUTPUT_VARIABLE DETECTED_CACHE_LINE_SIZE
)

# * Step:2 - Process the result :
if(COMPILE_RESULT AND "${RUN_RESULT_CODE}" STREQUAL "0")
  # The output might have leading/trailing whitespace, so we clean it up.
  string(STRIP "${DETECTED_CACHE_LINE_SIZE}" DETECTED_CACHE_LINE_SIZE)
  message(STATUS "Successfully detected cache line size: ${DETECTED_CACHE_LINE_SIZE} bytes.")
else()
  # If the process failed, fall back to safe default and warn
  set(DETECTED_CACHE_LINE_SIZE 64) # Common and safe default
  message(WARNING "Couldn't determine cache line size automatically. Falling back to a default of ${DETECTED_CACHE_LINE_SIZE} bytes. Performance may not be optimal.")
  if(NOT COMPILE_RESULT)
    message(WARNING "Reason: The utility failed to compile.")
  else()
    message(WARNING "Reason: The utility program ran but exited with the code: ${RUN_RESULT_CODE}.")
  endif()
endif()

# * Step:3 - Pass the Captured Value as a Compile time constant :
add_compile_definitions(SYSTEM_CACHE_LINE_SIZE=${DETECTED_CACHE_LINE_SIZE})

################################################################################
# Build Type Specific Compiler Flags
################################################################################

# Release build flags: maximum optimization
string(CONCAT CMAKE_CXX_FLAGS_RELEASE
    "-O3 "
    "-march=native "
    "-mtune=native "
    "-flto "
    "-pipe "
    "-DNDEBUG"
)

# Debug build flags: debugging symbols, no optimization
string(CONCAT CMAKE_CXX_FLAGS_DEBUG
    "-g "
    "-O0 "
    "-Wall "
    "-Wextra "
    "-pipe "
    "-DDEBUG"
)

# RelWithDebInfo: optimized but with debug symbols
string(CONCAT CMAKE_CXX_FLAGS_RELWITHDEBINFO
    "-O2 "
    "-g "
    "-march=native "
    "-pipe "
    "-DNDEBUG"
)

# MinSizeRel: optimize for size
string(CONCAT CMAKE_CXX_FLAGS_MINSIZEREL
    "-Os "
    "-pipe "
    "-DNDEBUG"
)

# Print the active build type
if(CMAKE_BUILD_TYPE)
    message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")
endif()
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
    src/batch_processor.cpp
    src/ffmpeg_queue.cpp
    src/ffmpeg_executor.cpp
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
