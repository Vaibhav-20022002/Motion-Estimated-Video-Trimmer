/**
 * @file config.hpp
 * @brief Configuration management via environment variables
 *
 * @details Provides a Config namespace with lazy-initialized, memoized
 *          configuration parameters loaded from environment variables.
 *          See config/motion_trim.env for detailed documentation of each
 *          parameter.
 *
 */

#ifndef MOTION_TRIM_CONFIG_HPP
#define MOTION_TRIM_CONFIG_HPP

#include <cstdint>
#include <cstdlib>
#include <string>

namespace motion_trim {
namespace Config {

/**
 * @brief Get a double value from environment variable.
 * @param name Environment variable name
 * @param default_val Default value if not set
 * @return Parsed double value or default
 */
inline double get_env_double(const char *name, double default_val) {
  const char *val = std::getenv(name);
  return val ? std::stod(val) : default_val;
}

/**
 * @brief Get an integer value from environment variable.
 * @param name Environment variable name
 * @param default_val Default value if not set
 * @return Parsed integer value or default
 */
inline int get_env_int(const char *name, int default_val) {
  const char *val = std::getenv(name);
  return val ? std::stoi(val) : default_val;
}

/**
 * @brief Get a float value from environment variable.
 * @param name Environment variable name
 * @param default_val Default value if not set
 * @return Parsed float value or default
 */
inline float get_env_float(const char *name, float default_val) {
  const char *val = std::getenv(name);
  return val ? std::stof(val) : default_val;
}

/// Motion vector magnitude threshold (squared to avoid sqrt in hot loop)
inline double mv_threshold_sq() {
  static double val = get_env_double("MV_THRESHOLD_SQ", 16.0);
  return val;
}

/// Macroblock size (16 for H.264/HEVC)
inline int block_size() {
  static int val = get_env_int("BLOCK_SIZE", 16);
  return val;
}

/// Bit shift for fast division by block_size (16 = 2^4)
inline int block_shift() {
  static int val = get_env_int("BLOCK_SHIFT", 4);
  return val;
}

/// Minimum motion vectors per block to mark as "active"
inline uint8_t vectors_needed() {
  static uint8_t val = static_cast<uint8_t>(get_env_int("VECTORS_NEEDED", 2));
  return val;
}

/// Minimum clusters of active blocks to trigger motion detection
inline int clusters_needed() {
  static int val = get_env_int("CLUSTERS_NEEDED", 2);
  return val;
}

/// Fraction of frame height to ignore at top/bottom (for timestamps)
inline float vertical_mask() {
  static float val = get_env_float("VERTICAL_MASK", 0.05f);
  return val;
}

/// Maximum gap between motion events before cutting
inline double max_gap_sec() {
  static double val = get_env_double("MAX_GAP_SEC", 5.0);
  return val;
}

/// Padding to add before/after motion segments
inline double padding_sec() {
  static double val = get_env_double("PADDING_SEC", 0.5);
  return val;
}

/// Chunk duration for dynamic load balancing (seconds per work unit)
inline double chunk_duration_sec() {
  static double val = get_env_double("CHUNK_DURATION_SEC", 30.0);
  return val;
}

/**
 * @brief Target analysis FPS for frame skipping (0 = analyze all frames)
 * @note Lower values = faster processing but may miss brief motion
 */
inline double target_fps() {
  static double val = get_env_double("TARGET_FPS", 0.0);
  return val;
}

/**
 * @brief Minimum savings percentage to trigger cutting
 * @note If savings are below this threshold, the original file is copied to output
 */
inline double min_savings_pct() {
  static double val = get_env_double("MIN_SAVINGS_PCT", 5.0);
  return val;
}

// **---- PARALLEL PROCESSING ----**

/**
 * @brief Number of parallel video streams for batch processing
 * @note Controls how many videos are processed simultaneously.
 *       0 = auto-detect based on available CPUs
 *       All available CPUs are used; no reservation.
 * @see THREADS_PER_STREAM for internal parallelism per stream
 */
inline int parallel_streams() {
  static int val = get_env_int("PARALLEL_STREAMS", 0);
  return val;
}

/**
 * @brief Threads and CPUs per stream for batch processing
 * @note This value controls THREE things:
 *
 *       1. Number of internal chunk scanning threads per stream
 *
 *       2. Number of CPUs allocated to each stream for pinning
 *
 *       3. Number of CPUs for FFmpeg process via taskset
 *
 *       - 0 = auto-calculate as (available_cpus / parallel_streams)
 *
 *       - N = use exactly N threads and N CPUs per stream
 *
 * @attention TUNING:
 *
 *   - Total CPUs used = PARALLEL_STREAMS × THREADS_PER_STREAM
 *
 *   - Ensure this does not exceed available CPUs
 *
 *   - Example: 4 streams × 2 threads = 8 CPUs
 */
inline int threads_per_stream() {
  static int val = get_env_int("THREADS_PER_STREAM", 0);
  return val;
}

/**
 * @brief Enable continuous watch mode for batch processing
 * @note If true, monitors input directory for new files
 */
inline bool watch_mode() {
  static bool val = (get_env_int("WATCH_MODE", 0) != 0);
  return val;
}

} // namespace Config
} // namespace motion_trim

#endif // MOTION_TRIM_CONFIG_HPP
