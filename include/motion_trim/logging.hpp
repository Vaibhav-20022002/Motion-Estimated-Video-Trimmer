/**
 * @file logging.hpp
 * @brief Logging macros and timing collection utilities
 *
 * @details Provides:
 *          - Compile-time controlled logging macros (LOG_INFO, LOG_WARN, etc.)
 *
 *          - Timing measurement macros (TIMER_START, TIMER_END)
 *
 *          - Thread-safe TimingCollector for aggregating performance metrics
 *
 * @note All logs use fmt::print for type-safe formatting and are flushed
 *       immediately to ensure visibility in Docker container logs.
 *
 */

#ifndef MOTION_TRIM_LOGGING_HPP
#define MOTION_TRIM_LOGGING_HPP

#include <chrono>
#include <mutex>
#include <string>
#include <vector>

#include <fmt/color.h>
#include <fmt/core.h>

namespace motion_trim {

// **----- LOGGING CONFIGURATION -----**

/**
 * @brief Logging is controlled by ENABLE_LOGGING at compile time.
 */
#ifndef ENABLE_LOGGING
#define ENABLE_LOGGING 1
#endif

#ifndef ENABLE_TIMING
#define ENABLE_TIMING 1
#endif

/// Global log mutex (defined in logging.cpp)
extern std::mutex log_mutex;

// **----- LOGGING MACROS -----**

#if ENABLE_LOGGING
#define LOG_INFO(format_str, ...)                                              \
  do {                                                                         \
    std::lock_guard<std::mutex> lock(motion_trim::log_mutex);                  \
    fmt::print("[INFO] " format_str "\n", ##__VA_ARGS__);                      \
    std::fflush(stdout);                                                       \
  } while (0)

#define LOG_WARN(format_str, ...)                                              \
  do {                                                                         \
    std::lock_guard<std::mutex> lock(motion_trim::log_mutex);                  \
    fmt::print(fg(fmt::color::yellow), "[WARN] " format_str "\n",              \
               ##__VA_ARGS__);                                                 \
    std::fflush(stdout);                                                       \
  } while (0)

#define LOG_ERROR(format_str, ...)                                             \
  do {                                                                         \
    std::lock_guard<std::mutex> lock(motion_trim::log_mutex);                  \
    fmt::print(fg(fmt::color::red), "[ERROR] " format_str "\n",                \
               ##__VA_ARGS__);                                                 \
    std::fflush(stdout);                                                       \
  } while (0)

#define LOG_PHASE(format_str, ...)                                             \
  do {                                                                         \
    std::lock_guard<std::mutex> lock(motion_trim::log_mutex);                  \
    fmt::print(fg(fmt::color::cyan), format_str "\n", ##__VA_ARGS__);          \
    std::fflush(stdout);                                                       \
  } while (0)

#define LOG_SUCCESS(format_str, ...)                                           \
  do {                                                                         \
    std::lock_guard<std::mutex> lock(motion_trim::log_mutex);                  \
    fmt::print(fg(fmt::color::green), format_str "\n", ##__VA_ARGS__);         \
    std::fflush(stdout);                                                       \
  } while (0)
#else
#define LOG_INFO(...) ((void)0)
#define LOG_WARN(...) ((void)0)
#define LOG_ERROR(...) ((void)0)
#define LOG_PHASE(...) ((void)0)
#define LOG_SUCCESS(...) ((void)0)
#endif

// **----- TIMING COLLECTION -----**

/**
 * @brief TimingEntry: A single timing measurement.
 * @note Stores the function name and duration in microseconds.
 */
struct TimingEntry {
  std::string name;  //< Function or phase name
  long microseconds; //< Duration in microseconds
};

/**
 * @class TimingCollector
 * @brief Thread-safe singleton for collecting timing measurements.
 * @note All worker threads can safely record their timings here.
 */
class TimingCollector {
  static std::mutex timing_mutex;
  static std::vector<TimingEntry> entries;

public:
  /**
   * @brief Record a timing measurement.
   * @param name Function or phase name
   * @param us Duration in microseconds
   */
  static void record(const std::string &name, long us);

  /**
   * @brief Print all collected timings as a formatted table.
   *        Called at program end for summary.
   */
  static void print_summary();

  /**
   * @brief Clear all collected timings.
   * @note Called between files in batch mode.
   */
  static void clear();
};

// **----- TIMING MACROS -----**

#if ENABLE_TIMING
#define TIMER_START(name)                                                      \
  auto timer_start_##name = std::chrono::high_resolution_clock::now()

#define TIMER_END(name)                                                        \
  do {                                                                         \
    auto timer_end_##name = std::chrono::high_resolution_clock::now();         \
    auto timer_duration_##name =                                               \
        std::chrono::duration_cast<std::chrono::microseconds>(                 \
            timer_end_##name - timer_start_##name)                             \
            .count();                                                          \
    motion_trim::TimingCollector::record(#name, timer_duration_##name);        \
  } while (0)
#else
#define TIMER_START(name) ((void)0)
#define TIMER_END(name) ((void)0)
#endif

} // namespace motion_trim

#endif // MOTION_TRIM_LOGGING_HPP
