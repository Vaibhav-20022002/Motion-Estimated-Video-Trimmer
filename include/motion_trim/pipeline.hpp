/**
 * @file pipeline.hpp
 * @brief Video processing pipeline orchestration
 *
 * @details The ProcessingPipeline class orchestrates the entire video analysis
 *          and cutting workflow:
 *
 *          1. Load video into RAM
 *
 *          2. Probe video metadata
 *
 *          3. Create task queue with chunks
 *
 *          4. Launch worker threads for parallel scanning
 *
 *          5. Collect and merge results
 *
 *          6. Generate cut segments
 *
 *          7. Execute FFmpeg to produce output
 *
 * @note When used in batch mode:
 *
 *       - num_threads controls internal parallelism (scanning threads)
 *
 *       - cpu_set specifies which CPUs to pin threads to
 *
 *       - FFmpeg is also pinned to cpu_set using taskset
 *
 *       - stream_id prefixes all log messages for clarity
 */

#ifndef MOTION_TRIM_PIPELINE_HPP
#define MOTION_TRIM_PIPELINE_HPP

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "memory_io.hpp"
#include "types.hpp"

namespace motion_trim {

/**
 * @class ProcessingPipeline
 * @brief Orchestrates the entire video analysis and cutting.
 *
 * @attention WORKFLOW:
 *
 * 1. Map video into RAM (zero-copy)
 *
 * 2. Probe video metadata (duration, fps)
 *
 * 3. Create task queue with small chunks
 *
 * 4. Launch num_threads workers, each pinned to cpu_set
 *
 * 5. Collect and merge results
 *
 * 6. Generate cut segments
 *
 * 7. Execute FFmpeg (pinned to cpu_set via taskset)
 */
class ProcessingPipeline {
  MappedFile file_buffer;
  std::string input_path;
  std::string output_path;
  double duration = 0;
  double time_removed = 0;
  double saved_pct = 0;

  int stream_id_;   //< Stream ID for log prefixing (-1 = no prefix)
  int num_threads_; //< Number of internal worker threads (0 = auto)
  std::vector<int>
      cpu_set_; //< CPU cores for thread pinning (empty = no pinning)

  /// Static mutex for sequential FFmpeg execution (one at a time)
  static std::mutex ffmpeg_mutex_;

  /**
   * @brief Print summary of what was cut.
   */
  void print_cut_summary();

  /**
   * @brief Execute FFmpeg to produce the cut video.
   * @attention Uses memfd_create to create an in-memory file for the concat
   *            list, avoiding disk I/O entirely.
   * @note When cpu_set_ is set, FFmpeg is pinned using taskset.
   * @note Uses ffmpeg_mutex_ to ensure only one FFmpeg runs at a time,
   *       preventing memory bus contention during disk writes.
   */
  void execute_cut(const std::vector<TimeSegment> &segments);

  /**
   * @brief Log a message with optional stream prefix.
   */
  void log_info(const std::string &msg);
  void log_phase(const std::string &msg);

public:
  /**
   * @brief Construct a processing pipeline.
   * @param in Input file path
   * @param out Output file path
   * @param stream_id Stream ID for log prefixing (-1 = no prefix, default)
   * @param num_threads Number of internal worker threads (0 = auto, default)
   * @param cpu_set CPU cores for thread pinning (empty = no pinning, default)
   */
  ProcessingPipeline(std::string in, std::string out, int stream_id = -1,
                     int num_threads = 0, std::vector<int> cpu_set = {});

  /**
   * @brief Run the complete processing pipeline.
   * @return 0 on success, non-zero on error
   * @note If ffmpeg_queue_ is set, FFmpeg job is pushed to queue instead of
   *       executed directly. This enables producer-consumer pattern for batch.
   */
  int run();

  /**
   * @brief Set FFmpeg queue for deferred execution.
   * @param queue Pointer to shared FFmpeg queue (nullptr = execute immediately)
   */
  void set_ffmpeg_queue(class FFmpegQueue *queue) { ffmpeg_queue_ = queue; }

  /**
   * @brief Get the duration of the video after processing.
   */
  double get_duration() const { return duration; }

  /**
   * @brief Get the time removed by trimming.
   */
  double get_time_removed() const { return time_removed; }

  /**
   * @brief Get the percentage saved.
   */
  double get_saved_pct() const { return saved_pct; }

private:
  class FFmpegQueue *ffmpeg_queue_{
      nullptr}; //< Optional queue for deferred FFmpeg
};

} // namespace motion_trim

#endif // MOTION_TRIM_PIPELINE_HPP
