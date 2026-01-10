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
 * @class MappedFile
 * @brief RAII wrapper for memory-mapped files.
 * @note Handles automatic cleanup (munmap/close) on destruction.
 *       Supports move semantics but not copy.
 */
class MappedFile {
public:
  MappedFile() = default;
  ~MappedFile();

  /// Disable copy
  MappedFile(const MappedFile &) = delete;
  MappedFile &operator=(const MappedFile &) = delete;

  /// Enable move
  MappedFile(MappedFile &&other) noexcept;
  MappedFile &operator=(MappedFile &&other) noexcept;

  const uint8_t *data() const { return data_; }
  size_t size() const { return size_; }
  bool is_valid() const { return data_ != nullptr; }

private:
  friend class MemoryLoader;
  uint8_t *data_ = nullptr;
  size_t size_ = 0;
  int fd_ = -1;
};

/**
 * @class MemoryLoader
 * @brief Handles loading video files into RAM and providing
 *       custom I/O callbacks for FFmpeg.
 *
 * @attention ROBUSTNESS:
 *
 * - Uses mmap for efficient file access (zero-copy)
 *
 * - Handles OOM gracefully (mmap failure)
 *
 * - Validates file size before mapping
 *
 * - Logs meaningful error messages
 */
class MemoryLoader {
public:
  /**
   * @brief Map an entire file into memory using mmap.
   * @param path Path to the file
   * @param file Output MappedFile object (will take ownership of the mapping)
   * @return true on success, false on failure
   */
  static bool load_file(const std::string &path, MappedFile &file);

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
