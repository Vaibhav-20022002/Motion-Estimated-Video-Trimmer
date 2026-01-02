/**
 * @file logging.cpp
 * @brief Logging and timing utilities implementation
 *
 * @details Provides static member definitions for:
 *          - Global log mutex
 * 
 *          - TimingCollector static members and methods
 */

#include "motion_trim/logging.hpp"

#include <fmt/color.h>
#include <fmt/core.h>

namespace motion_trim {

// **----- GLOBAL LOG MUTEX -----**

std::mutex log_mutex;

// **----- TIMING COLLECTOR STATIC MEMBERS -----**

std::mutex TimingCollector::timing_mutex;
std::vector<TimingEntry> TimingCollector::entries;

void TimingCollector::record(const std::string &name, long us) {
  std::lock_guard<std::mutex> lock(timing_mutex);
  entries.push_back({name, us});
}

void TimingCollector::print_summary() {
  std::lock_guard<std::mutex> lock(timing_mutex);
  if (entries.empty())
    return;

  fmt::print("\n");
  fmt::print(fg(fmt::color::cyan),
             "================== TIMING SUMMARY ==================\n");
  fmt::print("{:<30} {:>20}\n", "Function", "Time (us) [sec]");
  fmt::print("{:-<30} {:-<20}\n", "", "");

  for (const auto &e : entries) {
    double seconds = e.microseconds / 1000000.0;
    fmt::print("{:<30} {:>10} [{:.2f}s]\n", e.name, e.microseconds, seconds);
  }
  fmt::print(fg(fmt::color::cyan),
             "====================================================\n");
  std::fflush(stdout);
}

void TimingCollector::clear() {
  std::lock_guard<std::mutex> lock(timing_mutex);
  entries.clear();
}

} // namespace motion_trim
