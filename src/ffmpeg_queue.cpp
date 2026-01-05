/**
 * @file ffmpeg_queue.cpp
 * @brief FFmpeg job queue implementation
 */

#include "motion_trim/ffmpeg_queue.hpp"

namespace motion_trim {

void FFmpegQueue::push(FFmpegJob job) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    jobs_.push(std::move(job));
  }
  cv_.notify_one();
}

bool FFmpegQueue::pop(FFmpegJob &job) {
  std::unique_lock<std::mutex> lock(mutex_);
  cv_.wait(lock, [this] { return !jobs_.empty() || done_.load(); });

  if (jobs_.empty()) {
    return false;
  }

  job = std::move(jobs_.front());
  jobs_.pop();
  return true;
}

void FFmpegQueue::finish() {
  done_.store(true);
  cv_.notify_all();
}

} // namespace motion_trim
