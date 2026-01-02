/**
 * @file system.cpp
 * @brief System utilities implementation
 *
 * @details Provides:
 * 
 *          - Cgroup-aware CPU limit detection for Docker containers
 *          - Time formatting utilities
 */

#include "motion_trim/system.hpp"

#include <fstream>
#include <string>
#include <thread>

#include <fmt/core.h>

namespace motion_trim {

int detect_cpu_limit() {
  /// Helper to read a number from a file
  auto read_long = [](const char *path) -> long {
    std::ifstream f(path);
    if (!f)
      return -1;
    long val;
    f >> val;
    return f.good() ? val : -1;
  };

  /// Helper to count CPUs from cpuset string like "0,2,4,6,8" or "0-3"
  auto count_cpuset = [](const char *path) -> int {
    std::ifstream f(path);
    if (!f)
      return -1;
    std::string line;
    std::getline(f, line);
    if (line.empty())
      return -1;

    int count = 0;
    size_t pos = 0;
    while (pos < line.size()) {
      /// Find next number
      size_t end = line.find_first_of(",-", pos);
      if (end == std::string::npos)
        end = line.size();

      int start_cpu = std::stoi(line.substr(pos, end - pos));

      if (end < line.size() && line[end] == '-') {
        /// Range like "0-3"
        pos = end + 1;
        end = line.find(',', pos);
        if (end == std::string::npos)
          end = line.size();
        int end_cpu = std::stoi(line.substr(pos, end - pos));
        count += (end_cpu - start_cpu + 1);
      } else {
        /// Single CPU
        count++;
      }

      pos = (end < line.size()) ? end + 1 : line.size();
    }
    return count;
  };

  int limit = -1;

  /// Try cgroup v2 first (unified hierarchy)
  {
    std::ifstream f("/sys/fs/cgroup/cpu.max");
    if (f) {
      std::string quota_str, period_str;
      f >> quota_str >> period_str;
      if (quota_str != "max" && !period_str.empty()) {
        long quota = std::stol(quota_str);
        long period = std::stol(period_str);
        if (quota > 0 && period > 0) {
          /// Use ceiling to round up fractional CPUs (2.5 -> 3)
          limit = static_cast<int>((quota + period - 1) / period);
        }
      }
    }
  }

  /// Try cgroup v1 CPU quota
  if (limit <= 0) {
    long quota = read_long("/sys/fs/cgroup/cpu/cpu.cfs_quota_us");
    long period = read_long("/sys/fs/cgroup/cpu/cpu.cfs_period_us");
    if (quota > 0 && period > 0) {
      /// Use ceiling to round up fractional CPUs (2.5 -> 3)
      limit = static_cast<int>((quota + period - 1) / period);
    }
  }

  /// Try cpuset (counts actual allowed cores)
  if (limit <= 0) {
    // Try cgroup v2 path first
    limit = count_cpuset("/sys/fs/cgroup/cpuset.cpus.effective");
    if (limit <= 0) {
      // Try cgroup v1 path
      limit = count_cpuset("/sys/fs/cgroup/cpuset/cpuset.cpus");
    }
  }

  /// Fallback to hardware_concurrency
  if (limit <= 0) {
    limit = std::thread::hardware_concurrency();
  }

  /// Sanity check
  if (limit <= 0)
    limit = 4;
  if (limit > 64)
    limit = 64; //< Cap at 64 to prevent excessive threading

  /// Use the larger of the two limits
  ///\note If cpuset pins us to 3 specific cores but quota says 2.0 CPUs,
  ///      we prefer 3 threads to utilize L1/L2 caches of all pinned cores,
  ///      even if they are throttled.
  if (limit > 0) {
    int cpuset_count = count_cpuset("/sys/fs/cgroup/cpuset.cpus.effective");
    if (cpuset_count <= 0) {
      cpuset_count = count_cpuset("/sys/fs/cgroup/cpuset/cpuset.cpus");
    }
    if (cpuset_count > limit) {
      limit = cpuset_count;
    }
  }

  return limit;
}

std::string format_time(double seconds) {
  int h = static_cast<int>(seconds) / 3600;
  int m = (static_cast<int>(seconds) % 3600) / 60;
  int s = static_cast<int>(seconds) % 60;
  return fmt::format("{:02d}:{:02d}:{:02d}", h, m, s);
}

} // namespace motion_trim
