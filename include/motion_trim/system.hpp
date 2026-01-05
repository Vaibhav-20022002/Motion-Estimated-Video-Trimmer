/**
 * @file system.hpp
 * @brief System utilities, CPU detection, and thread management
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
 *       For Docker containers, CPU discovery respects cgroup limits set by
 *       docker-compose or docker run --cpus flags.
 */

#ifndef MOTION_TRIM_SYSTEM_HPP
#define MOTION_TRIM_SYSTEM_HPP

#include <string>
#include <vector>

namespace motion_trim {

// **---- CPU Detection ----**

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
 * @brief Get the list of available CPU core IDs.
 *
 * @note Reads from cgroup cpuset to find which CPUs are actually available
 *       in Docker containers. Falls back to 0..n-1 range if cgroup is not set.
 *
 * @return Vector of CPU IDs available for thread pinning
 */
std::vector<int> get_available_cpus();

/**
 * @brief Calculate the number of parallel streams to use.
 *
 * @note Uses the formula: min(PARALLEL_STREAMS, available_cpus - 1)
 *       where available_cpus is from Docker cgroup limits.
 *       Always reserves 1 CPU to avoid busy waiting on system tasks.
 *       If PARALLEL_STREAMS is 0 (auto), uses available_cpus - 1.
 *
 * @return Number of parallel video streams to use
 */
int calculate_parallel_streams();

// **---- Thread Pinning ----**

/**
 * @brief Pin the calling thread to a specific CPU core.
 *
 * @note Uses pthread_setaffinity_np on Linux. Improves cache locality
 *       and prevents thread migration between cores.
 *
 * @param cpu_id The CPU core ID to pin to (0-indexed)
 * @return true if pinning succeeded, false otherwise
 */
bool pin_thread_to_cpu(int cpu_id);

/**
 * @brief Pin the calling thread to a set of CPU cores.
 *
 * @note Allows thread to run on any of the specified CPUs.
 *       Useful for assigning a thread to a NUMA node or socket.
 *
 * @param cpu_ids Vector of CPU core IDs to pin to
 * @return true if pinning succeeded, false otherwise
 */
bool pin_thread_to_cpus(const std::vector<int> &cpu_ids);

// **---- Utilities ----**

/**
 * @brief Format seconds as HH:MM:SS string.
 * @param seconds Time in seconds
 * @return Formatted string in HH:MM:SS format
 */
std::string format_time(double seconds);

} // namespace motion_trim

#endif // MOTION_TRIM_SYSTEM_HPP
