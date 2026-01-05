/**
 * @file system.cpp
 * @brief System utilities implementation
 *
 * @details Provides:
 *
 *          - Cgroup-aware CPU limit detection for Docker containers
 *
 *          - Thread pinning for CPU affinity
 *
 *          - Available CPU core discovery
 *
 *          - Time formatting utilities
 *
 * @note Thread pinning uses pthread_setaffinity_np which is Linux-specific.
 */

#include "motion_trim/system.hpp"

#include <algorithm>
#include <fstream>
#include <string>
#include <thread>

#include <pthread.h>
#include <sched.h>

#include <fmt/core.h>

#include "motion_trim/config.hpp"

namespace motion_trim {

// **---- Internal Helpers ----**

namespace {

/// Helper to read a number from a file
long read_long_from_file(const char *path) {
  std::ifstream f(path);
  if (!f)
    return -1;
  long val;
  f >> val;
  return f.good() ? val : -1;
}

/// Helper to parse cpuset string like "0,2,4,6,8" or "0-3" into CPU list
std::vector<int> parse_cpuset_string(const std::string &line) {
  std::vector<int> cpus;
  if (line.empty())
    return cpus;

  size_t pos = 0;
  while (pos < line.size()) {
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
      for (int cpu = start_cpu; cpu <= end_cpu; ++cpu) {
        cpus.push_back(cpu);
      }
    } else {
      /// Single CPU
      cpus.push_back(start_cpu);
    }

    pos = (end < line.size()) ? end + 1 : line.size();
  }
  return cpus;
}

/// Helper to count CPUs from cpuset string
int count_cpuset(const char *path) {
  std::ifstream f(path);
  if (!f)
    return -1;
  std::string line;
  std::getline(f, line);
  auto cpus = parse_cpuset_string(line);
  return cpus.empty() ? -1 : static_cast<int>(cpus.size());
}

/// Read cpuset file and return CPU list
std::vector<int> read_cpuset_file(const char *path) {
  std::ifstream f(path);
  if (!f)
    return {};
  std::string line;
  std::getline(f, line);
  return parse_cpuset_string(line);
}

} // anonymous namespace

// **---- CPU Detection ----**

int detect_cpu_limit() {
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
          limit = static_cast<int>((quota + period - 1) / period);
        }
      }
    }
  }

  /// Try cgroup v1 CPU quota
  if (limit <= 0) {
    long quota = read_long_from_file("/sys/fs/cgroup/cpu/cpu.cfs_quota_us");
    long period = read_long_from_file("/sys/fs/cgroup/cpu/cpu.cfs_period_us");
    if (quota > 0 && period > 0) {
      limit = static_cast<int>((quota + period - 1) / period);
    }
  }

  /// Try cpuset (counts actual allowed cores)
  if (limit <= 0) {
    limit = count_cpuset("/sys/fs/cgroup/cpuset.cpus.effective");
    if (limit <= 0) {
      limit = count_cpuset("/sys/fs/cgroup/cpuset/cpuset.cpus");
    }
  }

  /// Fallback to hardware_concurrency
  if (limit <= 0) {
    limit = std::thread::hardware_concurrency();
  }

  /// Sanity checks
  if (limit <= 0)
    limit = 4;
  if (limit > 64)
    limit = 64;

  /// Use larger of quota vs cpuset
  int cpuset_count = count_cpuset("/sys/fs/cgroup/cpuset.cpus.effective");
  if (cpuset_count <= 0) {
    cpuset_count = count_cpuset("/sys/fs/cgroup/cpuset/cpuset.cpus");
  }
  if (cpuset_count > limit) {
    limit = cpuset_count;
  }

  return limit;
}

std::vector<int> get_available_cpus() {
  /// Try cgroup v2 path first
  auto cpus = read_cpuset_file("/sys/fs/cgroup/cpuset.cpus.effective");

  /// Try cgroup v1 path
  if (cpus.empty()) {
    cpus = read_cpuset_file("/sys/fs/cgroup/cpuset/cpuset.cpus");
  }

  /// Fallback: generate 0..n-1 based on detect_cpu_limit
  if (cpus.empty()) {
    int limit = detect_cpu_limit();
    for (int i = 0; i < limit; ++i) {
      cpus.push_back(i);
    }
  }

  return cpus;
}

int calculate_parallel_streams() {
  int available = detect_cpu_limit();
  int configured = Config::parallel_streams();

  /// Auto-detect: use all available CPUs
  if (configured == 0) {
    return std::max(1, available);
  }

  /// User configured: take minimum of configured and available
  return std::max(1, std::min(configured, available));
}

// **---- Thread Pinning ----**

bool pin_thread_to_cpu(int cpu_id) {
  cpu_set_t cpuset;
  CPU_ZERO(&cpuset);
  CPU_SET(cpu_id, &cpuset);

  int result =
      pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
  return (result == 0);
}

bool pin_thread_to_cpus(const std::vector<int> &cpu_ids) {
  if (cpu_ids.empty())
    return false;

  cpu_set_t cpuset;
  CPU_ZERO(&cpuset);

  for (int cpu_id : cpu_ids) {
    CPU_SET(cpu_id, &cpuset);
  }

  int result =
      pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
  return (result == 0);
}

// **---- Utilities ----**

std::string format_time(double seconds) {
  int h = static_cast<int>(seconds) / 3600;
  int m = (static_cast<int>(seconds) % 3600) / 60;
  int s = static_cast<int>(seconds) % 60;
  return fmt::format("{:02d}:{:02d}:{:02d}", h, m, s);
}

} // namespace motion_trim
