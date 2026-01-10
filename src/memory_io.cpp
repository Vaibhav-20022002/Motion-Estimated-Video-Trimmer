/**
 * @file memory_io.cpp
 * @brief Memory-based I/O for FFmpeg implementation
 *
 * @details Provides implementations for:
 *
 *          - MappedFile: RAII wrapper for mmap
 *
 *          - MemoryLoader::load_file - Map file into memory
 *
 *          - MemoryLoader::read - FFmpeg read callback
 *
 *          - MemoryLoader::seek - FFmpeg seek callback
 */

#include "motion_trim/memory_io.hpp"

#include <algorithm>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

extern "C" {
#include <libavformat/avio.h>
#include <libavutil/error.h>
}

#include "motion_trim/logging.hpp"

namespace motion_trim {

// **---- MappedFile Implementation ----**

MappedFile::~MappedFile() {
  if (data_) {
    munmap(data_, size_);
  }
  if (fd_ != -1) {
    close(fd_);
  }
}

MappedFile::MappedFile(MappedFile &&other) noexcept
    : data_(other.data_), size_(other.size_), fd_(other.fd_) {
  other.data_ = nullptr;
  other.size_ = 0;
  other.fd_ = -1;
}

MappedFile &MappedFile::operator=(MappedFile &&other) noexcept {
  if (this != &other) {
    if (data_) {
      munmap(data_, size_);
    }
    if (fd_ != -1) {
      close(fd_);
    }
    data_ = other.data_;
    size_ = other.size_;
    fd_ = other.fd_;

    other.data_ = nullptr;
    other.size_ = 0;
    other.fd_ = -1;
  }
  return *this;
}

// **---- MemoryLoader Implementation ----**

bool MemoryLoader::load_file(const std::string &path, MappedFile &file) {
  TIMER_START(load_file);

  /// Open file for reading
  int fd = open(path.c_str(), O_RDONLY);
  if (fd == -1) {
    LOG_ERROR("Failed to open file: {}", path);
    return false;
  }

  /// Get file size
  struct stat sb;
  if (fstat(fd, &sb) == -1) {
    LOG_ERROR("Failed to stat file: {}", path);
    close(fd);
    return false;
  }

  /// Validate file size
  if (sb.st_size <= 0) {
    LOG_ERROR("File is empty or invalid: {}", path);
    close(fd);
    return false;
  }

  /// Map file into memory (read-only, private copy-on-write)
  ///\note MAP_PRIVATE ensures we don't modify the original file if we write
  ///      (though we don't)
  ///\note MAP_POPULATE forces the kernel to read the file into RAM immediately.
  ///      This prevents page faults during scanning, ensuring 100% CPU usage.
  void *addr =
      mmap(nullptr, sb.st_size, PROT_READ, MAP_PRIVATE | MAP_POPULATE, fd, 0);
  if (addr == MAP_FAILED) {
    LOG_ERROR("Failed to mmap file: {}", path);
    close(fd);
    return false;
  }

  /// Hint kernel that we will read sequentially and use huge pages
  ///\note MADV_HUGEPAGE enables Transparent Huge Pages (THP) to reduce TLB
  /// misses
  ///\note MADV_SEQUENTIAL enables aggressive read-ahead
  madvise(addr, sb.st_size, MADV_SEQUENTIAL | MADV_HUGEPAGE);

  /// Clean up existing mapping if any
  if (file.data_) {
    munmap(file.data_, file.size_);
  }
  if (file.fd_ != -1) {
    close(file.fd_);
  }

  /// Transfer ownership to MappedFile
  file.data_ = static_cast<uint8_t *>(addr);
  file.size_ = static_cast<size_t>(sb.st_size);
  file.fd_ = fd;

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
