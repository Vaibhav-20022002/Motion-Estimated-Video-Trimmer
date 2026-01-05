/**
 * @file batch_processor.hpp
 * @brief Parallel video processing for batch mode
 *
 * @details The BatchProcessor class orchestrates parallel video processing:
 *
 *          - Spawns PARALLEL_STREAMS worker threads (one per video stream)
 *
 *          - Each stream is allocated THREADS_PER_STREAM CPUs
 *
 *          - All internal operations (loading, scanning, FFmpeg) are pinned
 *
 *            to the stream's allocated CPU set
 *
 *          - Work-stealing queue distributes video files across streams
 *
 *          - Logging is stream-prefixed for clarity
 *
 * @note Configuration via environment variables:
 *
 *       - PARALLEL_STREAMS: Number of concurrent video streams
 *
 *       - THREADS_PER_STREAM: CPUs and internal threads per stream
 *
 * @attention CPU Allocation Example (8 CPUs, 4 streams, 2 threads each):
 *       Stream 0 -> CPUs [0,1]
 *
 *       Stream 1 -> CPUs [2,3]
 *
 *       Stream 2 -> CPUs [4,5]
 *
 *       Stream 3 -> CPUs [6,7]
 */

#ifndef MOTION_TRIM_BATCH_PROCESSOR_HPP
#define MOTION_TRIM_BATCH_PROCESSOR_HPP

#include <atomic>
#include <mutex>
#include <queue>
#include <string>
#include <vector>

namespace motion_trim {

/**
 * @struct StreamResult
 * @brief Result from processing a single video in a stream.
 */
struct StreamResult {
  std::string filename;    //< Input filename
  bool success;            //< Whether processing succeeded
  double duration;         //< Original video duration
  double time_removed;     //< Time removed by trimming
  double saved_pct;        //< Percentage saved
  long processing_time_us; //< Processing time in microseconds
};

/**
 * @class BatchProcessor
 * @brief Orchestrates parallel video processing with CPU isolation.
 *
 * @attention RESOURCE ALLOCATION:
 *
 *   - Each stream gets a dedicated CPU set based on THREADS_PER_STREAM
 *
 *   - Stream worker thread is pinned to its CPU set
 *
 *   - Internal chunk scanning threads are pinned to the CPU set
 *
 *   - FFmpeg process is pinned using taskset command
 *
 *   - Total CPUs used = PARALLEL_STREAMS × THREADS_PER_STREAM
 */
class BatchProcessor {
public:
  /**
   * @brief Construct a batch processor.
   * @param num_streams Number of parallel video streams (0 = auto-detect)
   */
  explicit BatchProcessor(int num_streams = 0);

  /**
   * @brief Process all video files in the input list.
   *
   * @param input_files List of input video file paths
   * @param output_dir Output directory for processed videos
   * @return Number of failures (0 = all succeeded)
   */
  int process(const std::vector<std::string> &input_files,
              const std::string &output_dir);

private:
  int num_streams_;           //< Number of parallel streams
  int threads_per_stream_{0}; //< Threads per stream for internal parallelism
  std::mutex queue_mutex_;    //< Protects work queue
  std::queue<std::string> work_queue_; //< Files to process
  std::atomic<int> files_done_{0};     //< Counter for progress
  int total_files_{0};                 //< Total files to process

  std::mutex results_mutex_;          //< Protects results vector
  std::vector<StreamResult> results_; //< Collected results

  /**
   * @brief Get next file from work queue.
   * @param file Output: next file path
   * @return true if file was retrieved, false if queue empty
   */
  bool get_next_file(std::string &file);

  /**
   * @brief Worker function for each stream thread.
   *
   * @param stream_id The stream's ID (0-indexed)
   * @param cpu_set The set of CPU cores allocated to this stream
   * @param output_dir Output directory for processed videos
   * @param ffmpeg_queue Queue for pushing FFmpeg jobs
   */
  void stream_worker(int stream_id, const std::vector<int> &cpu_set,
                     const std::string &output_dir,
                     class FFmpegQueue *ffmpeg_queue);

  /**
   * @brief Print final batch summary.
   * @param wall_clock_sec Actual elapsed wall-clock time in seconds
   */
  void print_batch_summary(double wall_clock_sec);
};

} // namespace motion_trim

#endif // MOTION_TRIM_BATCH_PROCESSOR_HPP
