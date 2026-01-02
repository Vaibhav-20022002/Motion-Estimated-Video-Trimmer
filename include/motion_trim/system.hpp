/**
 * @file system.hpp
 * @brief System utilities and CPU detection
 *
 * @details Provides:
 *          - Cgroup-aware CPU limit detection for Docker containers
 * 
 *          - Time formatting utilities
 *
 */

#ifndef MOTION_TRIM_SYSTEM_HPP
#define MOTION_TRIM_SYSTEM_HPP

#include <string>

namespace motion_trim {

/**
 * @brief Detect the actual number of CPUs available to this process.
 *
 * @note In Docker containers, std::thread::hardware_concurrency() returns the
 *       HOST's total cores, not the container's cgroup limit. This function
 *       reads cgroup files to detect the actual limit.
 *
 *       Supports:
 *
 *        - Cgroup v1: `/sys/fs/cgroup/cpu/cpu.cfs_quota_us` and
 *          `cpu.cfs_period_us`
 *
 *        - Cgroup v2: `/sys/fs/cgroup/cpu.max`
 *
 *        - Cpuset: `/sys/fs/cgroup/cpuset/cpuset.cpus` (counts allowed cores)
 *
 * @return Detected CPU limit, or hardware_concurrency() as fallback
 */
int detect_cpu_limit();

/**
 * @brief Format seconds as HH:MM:SS string.
 * @param seconds Time in seconds
 * @return Formatted string in HH:MM:SS format
 */
std::string format_time(double seconds);

} // namespace motion_trim

#endif // MOTION_TRIM_SYSTEM_HPP
