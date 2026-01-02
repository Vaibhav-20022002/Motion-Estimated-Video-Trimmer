/**
 * @file types.hpp
 * @brief Core data types and constants for Motion Trim
 *
 * @details Contains fundamental data structures used throughout the
 * application:
 *          - Cache alignment constants
 * 
 *          - I/O buffer sizing
 * 
 *          - TimeSegment for time ranges
 * 
 *          - ScanTask for work queue items
 */

#ifndef MOTION_TRIM_TYPES_HPP
#define MOTION_TRIM_TYPES_HPP

#include <cstddef>
#include <cstdint>

namespace motion_trim {

// **----- CONSTANTS -----**

/**
 * @brief Size of the I/O buffer used by FFmpeg for reading.
 * @note Larger buffer = fewer context switches between memory reader and
 *       decoder. 256KB provides 4x fewer switches than 64KB and going beyond
 *       it proved to be counter-efficient.
 */
constexpr size_t AVIO_BUFFER_SIZE = 256 * 1024; //< 256KB

/**
 * @brief CPU cache line size for alignment.
 * @note Most modern CPUs use 64-byte cache lines.
 *       Aligning hot data to cache lines prevents false sharing in
 *       multi-threaded code.
 */
constexpr size_t CACHE_LINE_SIZE = 64;

// **----- DATA STRUCTURES -----**

/**
 * @struct TimeSegment
 * @brief Represents a time range [start, end) in seconds.
 * @note Used for both motion segments and cut points.
 *       Aligned to 16 bytes for potential SIMD operations.
 */
struct alignas(16) TimeSegment {
  double start; //< Start time in seconds
  double end;   //< End time in seconds
};

/**
 * @struct ScanTask
 * @brief A work unit for the dynamic task queue.
 * @note Cache-line aligned to prevent false sharing between threads.
 */
struct alignas(CACHE_LINE_SIZE) ScanTask {
  double start; //< Start time in seconds
  double end;   //< End time in seconds
  int id;       //< Chunk ID for debugging
};

} // namespace motion_trim

#endif // MOTION_TRIM_TYPES_HPP
