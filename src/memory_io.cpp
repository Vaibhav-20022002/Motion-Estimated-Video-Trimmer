/**
 * @file memory_io.cpp
 * @brief Memory-based I/O for FFmpeg implementation
 *
 * @details Provides implementations for:
 * 
 *          - MemoryLoader::load_file - Load entire file into memory
 * 
 *          - MemoryLoader::read - FFmpeg read callback
 * 
 *          - MemoryLoader::seek - FFmpeg seek callback
 */

#include "motion_trim/memory_io.hpp"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <new>

extern "C" {
#include <libavformat/avio.h>
#include <libavutil/error.h>
}

#include "motion_trim/logging.hpp"

namespace motion_trim {

bool MemoryLoader::load_file(const std::string &path,
                             std::vector<uint8_t> &buffer) {
  TIMER_START(load_file);

  /// Open file and get size
  std::ifstream file(path, std::ios::binary | std::ios::ate);
  if (!file) {
    LOG_ERROR("Failed to open file: {}", path);
    return false;
  }

  std::streamsize size = file.tellg();

  /// Validate file size
  if (size <= 0) {
    LOG_ERROR("File is empty or invalid: {}", path);
    return false;
  }

  file.seekg(0, std::ios::beg);

  /// CRITICAL: Catch allocation failures
  ///\note This prevents crashes when file is larger than available RAM
  try {
    buffer.resize(static_cast<size_t>(size));
  } catch (const std::bad_alloc &e) {
    LOG_ERROR("Not enough RAM to load {} MB file! ({})", size / 1024 / 1024,
              e.what());
    return false;
  }

  /// Read file into buffer
  bool result = static_cast<bool>(
      file.read(reinterpret_cast<char *>(buffer.data()), size));

  if (!result) {
    LOG_ERROR("Failed to read file contents");
    return false;
  }

  TIMER_END(load_file);
  return true;
}

int MemoryLoader::read(void *opaque, uint8_t *buf, int buf_size) {
  MemReaderState *bd = static_cast<MemReaderState *>(opaque);
  size_t bytes_left = bd->size - bd->pos;
  if (bytes_left == 0)
    return AVERROR_EOF;
  size_t copy = std::min(bytes_left, static_cast<size_t>(buf_size));
  memcpy(buf, bd->ptr + bd->pos, copy);
  bd->pos += copy;
  return static_cast<int>(copy);
}

int64_t MemoryLoader::seek(void *opaque, int64_t offset, int whence) {
  MemReaderState *bd = static_cast<MemReaderState *>(opaque);

  if (whence == AVSEEK_SIZE)
    return static_cast<int64_t>(bd->size);

  int64_t new_pos = static_cast<int64_t>(bd->pos);
  if (whence == SEEK_SET)
    new_pos = offset;
  else if (whence == SEEK_CUR)
    new_pos += offset;
  else if (whence == SEEK_END)
    new_pos = static_cast<int64_t>(bd->size) + offset;

  if (new_pos < 0)
    new_pos = 0;
  if (new_pos > static_cast<int64_t>(bd->size))
    new_pos = static_cast<int64_t>(bd->size);

  bd->pos = static_cast<size_t>(new_pos);
  return new_pos;
}

} // namespace motion_trim
