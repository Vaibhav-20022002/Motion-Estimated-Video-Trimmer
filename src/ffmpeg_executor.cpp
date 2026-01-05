/**
 * @file ffmpeg_executor.cpp
 * @brief FFmpeg execution implementation
 */

#include "motion_trim/ffmpeg_executor.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <sys/syscall.h>
#include <unistd.h>

#include <fmt/core.h>

#include "motion_trim/logging.hpp"

#ifndef MFD_CLOEXEC
#define MFD_CLOEXEC 0x0001U
#endif

namespace motion_trim {

int execute_ffmpeg_cut(const std::string &input_path,
                       const std::string &output_path,
                       const std::vector<TimeSegment> &segments,
                       const std::vector<int> &cpu_set, int stream_id) {

  if (segments.empty()) {
    if (stream_id >= 0) {
      LOG_WARN("[Stream {}] No segments to cut", stream_id);
    } else {
      LOG_WARN("No segments to cut");
    }
    return 0;
  }

  /// Build concat list
  std::string list_content;
  list_content.reserve(4096);

  std::string abs_path = std::filesystem::absolute(input_path).string();

  for (const auto &s : segments) {
    if (s.end <= s.start)
      continue;
    list_content += fmt::format("file '{}'\n", abs_path);
    list_content += fmt::format("inpoint {:.2f}\n", s.start);
    list_content += fmt::format("outpoint {:.2f}\n", s.end);
  }

  /// Create memory file for concat list
  int fd = syscall(SYS_memfd_create, "cut_list_mem", MFD_CLOEXEC);
  if (fd == -1) {
    if (stream_id >= 0) {
      LOG_ERROR("[Stream {}] Failed to create memory file!", stream_id);
    } else {
      LOG_ERROR("Failed to create memory file!");
    }
    return -1;
  }

  if (write(fd, list_content.c_str(), list_content.size()) == -1) {
    if (stream_id >= 0) {
      LOG_ERROR("[Stream {}] Failed to write to memory file", stream_id);
    } else {
      LOG_ERROR("Failed to write to memory file");
    }
    close(fd);
    return -1;
  }

  std::string mem_file_path = fmt::format("/proc/{}/fd/{}", getpid(), fd);

  /// Build FFmpeg command with optional CPU pinning
  std::string cmd;
  if (!cpu_set.empty()) {
    std::string cpu_list;
    for (size_t i = 0; i < cpu_set.size(); ++i) {
      if (i > 0)
        cpu_list += ",";
      cpu_list += std::to_string(cpu_set[i]);
    }
    cmd = fmt::format(
        "taskset -c {} /usr/local/bin/ffmpeg -y -hide_banner -loglevel error "
        "-f concat -safe 0 -protocol_whitelist file,pipe,fd -i \"{}\" "
        "-c copy -fflags +genpts -avoid_negative_ts make_zero "
        "-movflags +faststart \"{}\"",
        cpu_list, mem_file_path, output_path);
  } else {
    cmd = fmt::format(
        "/usr/local/bin/ffmpeg -y -hide_banner -loglevel error "
        "-f concat -safe 0 -protocol_whitelist file,pipe,fd -i \"{}\" "
        "-c copy -fflags +genpts -avoid_negative_ts make_zero "
        "-movflags +faststart \"{}\"",
        mem_file_path, output_path);
  }

  if (stream_id >= 0) {
    LOG_INFO("[FFmpeg Worker] Executing cut for stream {}: {}", stream_id,
             std::filesystem::path(output_path).filename().string());
  }

  int status = std::system(cmd.c_str());

  close(fd);

  if (status != 0) {
    if (stream_id >= 0) {
      LOG_ERROR("[Stream {}] FFmpeg failed with status {}", stream_id, status);
    } else {
      LOG_ERROR("FFmpeg failed with status {}", status);
    }
    return status;
  }

  return 0;
}

} // namespace motion_trim
