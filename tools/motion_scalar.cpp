/**
 * @file motion_scalar.cpp
 * @brief Motion Scalar Computation Utility
 *
 * @details Standalone utility to compute motion scalar values from a JSON
 *          file containing extracted motion vectors.
 *
 * @usage
 *   motion_scalar motion_vectors.json
 *
 * @note Requires nlohmann/json.hpp header in include path
 *
 */

#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>
#include <sys/resource.h>
#include <unordered_map>

#include "json.hpp"

using json = nlohmann::json;
using steady_clock = std::chrono::steady_clock;

/**
 * @brief Convert timeval to seconds.
 * @param tv timeval structure
 * @return Time in seconds as double
 */
static double tv_to_sec(const timeval &tv) {
  return tv.tv_sec + tv.tv_usec * 1e-6;
}

int main(int argc, char **argv) {
  if (argc < 2) {
    std::cerr << "Usage: " << argv[0] << " motion_vectors.json\n";
    return 1;
  }

  // ---- START TIMERS ----
  auto wall_start = steady_clock::now();
  struct rusage ru_start{};
  getrusage(RUSAGE_SELF, &ru_start);

  // ---- WORK ----
  std::ifstream in(argv[1]);
  if (!in) {
    std::cerr << "Failed to open JSON\n";
    return 1;
  }

  json root;
  in >> root;

  std::unordered_map<int, double> motion_per_sec(70'000);

  for (const auto &frame : root["frames"]) {
    if (frame["pts_seconds"].is_null())
      continue;

    double pts = frame["pts_seconds"];
    int sec = static_cast<int>(std::floor(pts));

    for (const auto &mv : frame["motion_vectors"]) {
      int mx = mv["motion_x"];
      int my = mv["motion_y"];
      int scale = mv["motion_scale"];
      int w = mv["w"];
      int h = mv["h"];

      if (scale == 0)
        continue;

      double dx = double(mx) / scale;
      double dy = double(my) / scale;

      double mag = std::sqrt(dx * dx + dy * dy);
      motion_per_sec[sec] += mag * w * h;
    }
  }

  // ---- END TIMERS ----
  auto wall_end = steady_clock::now();
  struct rusage ru_end{};
  getrusage(RUSAGE_SELF, &ru_end);

  double wall_time =
      std::chrono::duration<double>(wall_end - wall_start).count();

  double user_cpu = tv_to_sec(ru_end.ru_utime) - tv_to_sec(ru_start.ru_utime);

  double sys_cpu = tv_to_sec(ru_end.ru_stime) - tv_to_sec(ru_start.ru_stime);

  double cpu_time = user_cpu + sys_cpu;
  double cpu_pct = wall_time > 0 ? (cpu_time / wall_time) * 100.0 : 0.0;

  // Linux: ru_maxrss in KB
  // macOS / BSD: ru_maxrss in BYTES
#if defined(__APPLE__) || defined(__FreeBSD__)
  double max_rss_mb = ru_end.ru_maxrss / (1024.0 * 1024.0);
#else
  double max_rss_mb = ru_end.ru_maxrss / 1024.0;
#endif

  // ---- OUTPUT RESULTS ----
  std::cout << "second,motion_value\n";
  for (const auto &kv : motion_per_sec) {
    std::cout << kv.first << "," << kv.second << "\n";
  }

  std::cerr << "\n==== PERFORMANCE METRICS ====\n";
  std::cerr << "Wall time (s):        " << wall_time << "\n";
  std::cerr << "User CPU time (s):    " << user_cpu << "\n";
  std::cerr << "System CPU time (s):  " << sys_cpu << "\n";
  std::cerr << "Total CPU time (s):   " << cpu_time << "\n";
  std::cerr << "CPU utilization (%): " << cpu_pct << "\n";
  std::cerr << "Max RSS (MB):         " << max_rss_mb << "\n";

  return 0;
}
