/**
 * @file ffmpeg_queue.hpp
 * @brief Thread-safe FFmpeg job queue for producer-consumer pattern
 *
 * @details Enables parallel scanning with sequential FFmpeg execution:
 * 
 *          - Stream workers (producers) scan videos and push cut jobs
 * 
 *          - FFmpeg worker (consumer) executes jobs one at a time
 * 
 *          - Eliminates I/O contention while maintaining scan parallelism
 */

#ifndef MOTION_TRIM_FFMPEG_QUEUE_HPP
#define MOTION_TRIM_FFMPEG_QUEUE_HPP

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <queue>
#include <string>
#include <vector>

#include "types.hpp"

namespace motion_trim {

/**
 * @struct FFmpegJob
 * @brief A single FFmpeg cut job to be executed.
 */
struct FFmpegJob {
  int stream_id;                     //< Source stream ID for logging
  std::string input_path;            //< Absolute path to input video
  std::string output_path;           //< Absolute path to output video
  std::vector<TimeSegment> segments; //< Time segments to keep
  std::vector<int> cpu_set;          //< CPU set for taskset (may be empty)
};

/**
 * @class FFmpegQueue
 * @brief Thread-safe queue for FFmpeg jobs (producer-consumer pattern).
 *
 * @attention USAGE:
 * 
 *   - Stream workers call push() after scanning
 * 
 *   - FFmpeg worker calls pop() in a loop
 * 
 *   - Call finish() when all streams are done
 */
class FFmpegQueue {
public:
  /**
   * @brief Push a job to the queue.
   * @param job The FFmpeg job to execute
   */
  void push(FFmpegJob job);

  /**
   * @brief Pop a job from the queue (blocking).
   * @param job Output: the job to execute
   * @return true if job was retrieved, false if queue is finished
   */
  bool pop(FFmpegJob &job);

  /**
   * @brief Signal that no more jobs will be pushed.
   */
  void finish();

  /**
   * @brief Check if queue is finished and empty.
   */
  bool is_done() const { return done_.load() && empty(); }

  /**
   * @brief Check if queue is empty.
   */
  bool empty() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return jobs_.empty();
  }

private:
  mutable std::mutex mutex_;
  std::condition_variable cv_;
  std::queue<FFmpegJob> jobs_;
  std::atomic<bool> done_{false};
};

} // namespace motion_trim

#endif // MOTION_TRIM_FFMPEG_QUEUE_HPP
