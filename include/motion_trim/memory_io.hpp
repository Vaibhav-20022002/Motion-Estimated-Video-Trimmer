/**
 * @file memory_io.hpp
 * @brief Memory-based I/O for FFmpeg
 *
 * @details Provides:
 *          - MemReaderState: State for custom FFmpeg I/O from memory buffer
 * 
 *          - MemoryLoader: File loading and FFmpeg I/O callbacks
 *
 */

#ifndef MOTION_TRIM_MEMORY_IO_HPP
#define MOTION_TRIM_MEMORY_IO_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "types.hpp"

namespace motion_trim {

/**
 * @brief MemReaderState: State for custom FFmpeg I/O from memory buffer.
 * @note Allows FFmpeg to read from RAM instead of disk.
 *       Cache-line aligned since this is accessed heavily in the I/O callback.
 */
struct alignas(CACHE_LINE_SIZE) MemReaderState {
  const uint8_t *ptr; //< Pointer to buffer start
  size_t size;        //< Total buffer size
  size_t pos;         //< Current read position
};

/**
 * @class MemoryLoader
 * @brief Handles loading video files into RAM and providing
 *       custom I/O callbacks for FFmpeg.
 *
 * @attention ROBUSTNESS:
 *
 * - Handles OOM gracefully with try-catch
 *
 * - Validates file size before allocation
 *
 * - Logs meaningful error messages
 */
class MemoryLoader {
public:
  /**
   * @brief Load an entire file into a memory buffer.
   * @param path Path to the file
   * @param buffer Output buffer (will be resized)
   * @return true on success, false on failure
   */
  static bool load_file(const std::string &path, std::vector<uint8_t> &buffer);

  /**
   * @brief FFmpeg read callback for custom I/O.
   * @note Reads from memory buffer instead of file.
   */
  static int read(void *opaque, uint8_t *buf, int buf_size);

  /**
   * @brief FFmpeg seek callback for custom I/O.
   * @note Handles all seek modes including AVSEEK_SIZE.
   */
  static int64_t seek(void *opaque, int64_t offset, int whence);
};

} // namespace motion_trim

#endif // MOTION_TRIM_MEMORY_IO_HPP
