/**
 * @file batch_processor.cpp
 * @brief Parallel video processing implementation
 *
 * @details Implements the BatchProcessor class for parallel video processing:
 *
 *          - Thread pinning for CPU affinity
 *
 *          - Work-stealing queue for load balancing
 *
 *          - Stream-prefixed logging
 *
 *          - Sequential summary output
 */

#include "motion_trim/batch_processor.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <thread>

#include <fmt/color.h>
#include <fmt/core.h>

#include "motion_trim/config.hpp"
#include "motion_trim/ffmpeg_executor.hpp"
#include "motion_trim/ffmpeg_queue.hpp"
#include "motion_trim/logging.hpp"
#include "motion_trim/pipeline.hpp"
#include "motion_trim/system.hpp"

namespace motion_trim {

namespace fs = std::filesystem;

BatchProcessor::BatchProcessor(int num_streams) {
  if (num_streams <= 0) {
    num_streams_ = calculate_parallel_streams();
  } else {
    /// Respect user config but cap at available CPUs
    int available = detect_cpu_limit();
    num_streams_ = std::min(num_streams, available);
    num_streams_ = std::max(1, num_streams_);
  }
}

int BatchProcessor::process(const std::vector<std::string> &input_files,
                            const std::string &output_dir,
                            const std::string &input_dir_arg) {
  if (input_files.empty() && !Config::watch_mode()) {
    LOG_WARN("No input files to process");
    return 0;
  }

  total_files_ = static_cast<int>(input_files.size());
  files_done_.store(0);

  /// Populate work queue
  for (const auto &file : input_files) {
    /// Mark as seen so monitor_directory doesn't re-queue it
    processed_files_.insert(file);

    std::string output_file =
        (fs::path(output_dir) / fs::path(file).filename()).string();
    if (fs::exists(output_file)) {
      LOG_INFO("Skipping existing output: {}", output_file);
      continue;
    }

    work_queue_.push(file);
  }

  /// Update total files to reflect actual work
  total_files_ = static_cast<int>(work_queue_.size());

  /// Get available CPUs for pinning
  auto available_cpus = get_available_cpus();

  /// Cap streams to available CPUs
  int actual_streams =
      std::min(num_streams_, static_cast<int>(available_cpus.size()));
  actual_streams = std::max(1, actual_streams);

  /// Get configured threads per stream
  /// This value controls:
  /// 1. Number of internal chunk scanning threads
  /// 2. Number of CPUs allocated to each stream
  /// 3. Number of CPUs for FFmpeg taskset
  threads_per_stream_ = Config::threads_per_stream();

  /// If threads_per_stream is 0 (auto), calculate from available CPUs
  if (threads_per_stream_ <= 0) {
    threads_per_stream_ =
        std::max(1, static_cast<int>(available_cpus.size()) / actual_streams);
  }

  /// CPUs per stream = threads per stream (they are the same)
  int cpus_per_stream = threads_per_stream_;

  /// Build CPU sets for each stream based on threads_per_stream
  std::vector<std::vector<int>> stream_cpu_sets(actual_streams);
  int cpu_idx = 0;
  for (int s = 0; s < actual_streams; ++s) {
    for (int t = 0; t < cpus_per_stream &&
                    cpu_idx < static_cast<int>(available_cpus.size());
         ++t) {
      stream_cpu_sets[s].push_back(available_cpus[cpu_idx++]);
    }
  }

  LOG_PHASE("================== BATCH PROCESSING ==================");
  LOG_INFO("Files to process: {}", total_files_);
  LOG_INFO("Parallel streams: {}", actual_streams);
  LOG_INFO("Threads/CPUs per stream: {}", threads_per_stream_);
  LOG_INFO("Total CPUs needed: {}", actual_streams * threads_per_stream_);
  LOG_INFO("Available CPUs: {}", available_cpus.size());

  /// Log CPU allocation
  for (int s = 0; s < actual_streams; ++s) {
    std::string cpu_list;
    for (size_t i = 0; i < stream_cpu_sets[s].size(); ++i) {
      if (i > 0)
        cpu_list += ",";
      cpu_list += std::to_string(stream_cpu_sets[s][i]);
    }
    LOG_INFO("Stream {} -> CPUs [{}]", s, cpu_list);
  }
  LOG_PHASE("=======================================================");

  /// Start wall-clock timer
  auto batch_start = std::chrono::high_resolution_clock::now();

  /// Create FFmpeg queue for producer-consumer pattern
  FFmpegQueue ffmpeg_queue;

  /// Launch FFmpeg worker thread (consumes FFmpeg jobs sequentially)
  std::thread ffmpeg_worker([&ffmpeg_queue]() {
    LOG_INFO("[FFmpeg Worker] Started");
    FFmpegJob job;
    int jobs_processed = 0;
    while (ffmpeg_queue.pop(job)) {
      LOG_INFO("[FFmpeg Worker] Processing job {} from stream {}: {}",
               ++jobs_processed, job.stream_id,
               fs::path(job.output_path).filename().string());
      execute_ffmpeg_cut(job.input_path, job.output_path, job.segments,
                         job.cpu_set, job.stream_id);
    }
    LOG_INFO("[FFmpeg Worker] Finished ({} jobs)", jobs_processed);
  });

  /// Launch stream threads with CPU sets (producers)
  std::vector<std::thread> streams;
  for (int i = 0; i < actual_streams; ++i) {
    streams.emplace_back(&BatchProcessor::stream_worker, this, i,
                         stream_cpu_sets[i], output_dir, &ffmpeg_queue);
  }

  // **---- WATCH MODE ----**

  if (Config::watch_mode()) {
    /// In watch mode, we start a monitoring thread
    std::string input_dir = input_dir_arg;

    /// Fallback if not provided (should be provided by main.cpp)
    if (input_dir.empty() && !input_files.empty()) {
      input_dir = fs::path(input_files[0]).parent_path().string();
    }
    if (input_dir.empty())
      input_dir = ".";

    LOG_INFO("Starting Watch Mode on directory: {}", input_dir);

    /// Populate initial processed files to avoid re-processing
    /// We check if output file exists
    for (const auto &f : input_files) {
      processed_files_.insert(f);
    }

    std::thread monitor(&BatchProcessor::monitor_directory, this, input_dir,
                        output_dir);
    monitor.join(); //< Wait forever (or until stopped)
  }

  /// Wait for all streams to complete scanning
  for (auto &stream : streams) {
    stream.join();
  }

  /// Signal FFmpeg worker that no more jobs are coming
  ffmpeg_queue.finish();

  /// Wait for FFmpeg worker to complete
  ffmpeg_worker.join();

  /// Calculate wall-clock elapsed time
  auto batch_end = std::chrono::high_resolution_clock::now();
  double elapsed_sec =
      std::chrono::duration<double>(batch_end - batch_start).count();

  /// Print final summary with wall-clock time
  print_batch_summary(elapsed_sec);

  /// Count failures
  int failures = 0;
  for (const auto &result : results_) {
    if (!result.success) {
      failures++;
    }
  }

  return failures;
}

bool BatchProcessor::get_next_file(std::string &file) {
  std::unique_lock<std::mutex> lock(queue_mutex_);

  if (Config::watch_mode()) {
    /// In watch mode, wait for new files
    cv_.wait(lock, [this] { return !work_queue_.empty() || stop_watch_; });

    if (work_queue_.empty() && stop_watch_) {
      return false;
    }
  } else {
    /// In normal mode, return false if empty
    if (work_queue_.empty()) {
      return false;
    }
  }

  file = work_queue_.front();
  work_queue_.pop();
  return true;
}

void BatchProcessor::monitor_directory(const std::string &input_dir,
                                       const std::string &output_dir) {
  int poll_count = 0;
  while (!stop_watch_) {
    try {
      if (poll_count++ % 15 == 0) { //< Log every 30 seconds (15 * 2s)
        LOG_INFO("[Watch] Monitoring directory: {} (Waiting for new files...)",
                 input_dir);
      }

      for (const auto &entry : fs::directory_iterator(input_dir)) {
        if (entry.is_regular_file()) {
          std::string path = entry.path().string();
          std::string ext = entry.path().extension().string();
          std::transform(ext.begin(), ext.end(), ext.begin(),
                         [](unsigned char c) { return std::tolower(c); });

          if (ext == ".mp4" || ext == ".mkv" || ext == ".ts" || ext == ".mov" ||
              ext == ".avi") {

            /// Check if already processed
            if (processed_files_.find(path) == processed_files_.end()) {

              /// Check if output file already exists (persistence across
              /// restarts)
              std::string output_file =
                  (fs::path(output_dir) / entry.path().filename()).string();
              if (fs::exists(output_file)) {
                LOG_INFO("[Watch] Skipping file (already processed): {}",
                         entry.path().filename().string());
                processed_files_.insert(path);
                continue;
              }

              /// Check if file is stable (not being written to)
              /// Simple check: wait 500ms and check size
              auto size1 = fs::file_size(path);
              std::this_thread::sleep_for(std::chrono::milliseconds(500));
              auto size2 = fs::file_size(path);

              if (size1 != size2) {
                continue; // File is growing, skip for now
              }

              LOG_INFO("[Watch] New file detected: {}",
                       entry.path().filename().string());

              {
                std::lock_guard<std::mutex> lock(queue_mutex_);
                work_queue_.push(path);
                processed_files_.insert(path);
                total_files_++;
              }
              cv_.notify_one();
            }
          }
        }
      }
    } catch (const std::exception &e) {
      LOG_ERROR("[Watch] Error scanning directory: {}", e.what());
    }

    /// Poll interval
    std::this_thread::sleep_for(std::chrono::seconds(2));
  }

  /// Wake up workers to finish
  cv_.notify_all();
}

void BatchProcessor::stream_worker(int stream_id,
                                   const std::vector<int> &cpu_set,
                                   const std::string &output_dir,
                                   FFmpegQueue *ffmpeg_queue) {
  /// Pin this thread to the assigned CPU set
  if (pin_thread_to_cpus(cpu_set)) {
    std::string cpu_list;
    for (size_t i = 0; i < cpu_set.size(); ++i) {
      if (i > 0)
        cpu_list += ",";
      cpu_list += std::to_string(cpu_set[i]);
    }
    LOG_INFO("[Stream {}] Pinned to CPUs [{}]", stream_id, cpu_list);
  } else {
    LOG_WARN("[Stream {}] Failed to pin to CPUs", stream_id);
  }

  std::string file;
  while (get_next_file(file)) {
    fs::path input_path(file);
    std::string output_file =
        (fs::path(output_dir) / input_path.filename()).string();

    LOG_PHASE("[Stream {}] ----------------------------------------",
              stream_id);
    LOG_INFO("[Stream {}] Processing: {}", stream_id,
             input_path.filename().string());
    LOG_INFO("[Stream {}] Progress: {}/{}", stream_id, files_done_.load() + 1,
             total_files_);

    auto start_time = std::chrono::high_resolution_clock::now();

    /// Process the video
    StreamResult result;
    result.filename = input_path.filename().string();

    /// Create pipeline with stream_id, thread count, and CPU set for pinning
    ProcessingPipeline pipeline(file, output_file, stream_id,
                                threads_per_stream_, cpu_set);

    /// Set FFmpeg queue for deferred execution (producer-consumer pattern)
    pipeline.set_ffmpeg_queue(ffmpeg_queue);

    int ret = pipeline.run();

    auto end_time = std::chrono::high_resolution_clock::now();
    result.processing_time_us =
        std::chrono::duration_cast<std::chrono::microseconds>(end_time -
                                                              start_time)
            .count();

    result.success = (ret == 0);
    /// NOTE: Pipeline internally manages timing and cut summaries
    /// Just track success/failure here

    /// Add to results
    {
      std::lock_guard<std::mutex> lock(results_mutex_);
      results_.push_back(result);
    }

    ++files_done_;

    if (result.success) {
      LOG_SUCCESS("[Stream {}] Completed: {} ({:.1f}s)", stream_id,
                  result.filename, result.processing_time_us / 1000000.0);
    } else {
      LOG_ERROR("[Stream {}] Failed: {}", stream_id, result.filename);
    }

    /// Clear timing for next file
    TimingCollector::clear();
  }

  LOG_INFO("[Stream {}] Finished (no more files)", stream_id);
}

void BatchProcessor::print_batch_summary(double wall_clock_sec) {
  /// Collect statistics
  int total = static_cast<int>(results_.size());
  int success = 0;
  int failed = 0;
  long total_time_us = 0;

  for (const auto &result : results_) {
    if (result.success) {
      success++;
    } else {
      failed++;
    }
    total_time_us += result.processing_time_us;
  }

  double sum_time_sec = total_time_us / 1000000.0;
  double speedup = (wall_clock_sec > 0) ? sum_time_sec / wall_clock_sec : 1.0;

  fmt::print("\n");
  fmt::print(fg(fmt::color::cyan),
             "============== BATCH PROCESSING SUMMARY ==============\n");
  fmt::print("{:<25} {:>25}\n", "Total files:", total);
  fmt::print("{:<25} {:>25}\n", "Successful:", success);
  fmt::print("{:<25} {:>25}\n", "Failed:", failed);
  fmt::print("{:<25} {:>25}\n", "Parallel streams:", num_streams_);
  fmt::print("{:<25} {:>22.1f}s\n", "Wall-clock time:", wall_clock_sec);
  fmt::print("{:<25} {:>22.1f}s\n", "Sum of file times:", sum_time_sec);
  fmt::print("{:<25} {:>22.2f}x\n", "Speedup:", speedup);

  if (total > 0) {
    double avg_time = sum_time_sec / total;
    fmt::print("{:<25} {:>22.1f}s\n", "Average time per file:", avg_time);
  }

  fmt::print(fg(fmt::color::cyan),
             "======================================================\n");
  std::fflush(stdout);

  /// List failed files if any
  if (failed > 0) {
    fmt::print(fg(fmt::color::red), "\nFailed files:\n");
    for (const auto &result : results_) {
      if (!result.success) {
        fmt::print(fg(fmt::color::red), "  - {}\n", result.filename);
      }
    }
    std::fflush(stdout);
  }
}

} // namespace motion_trim
