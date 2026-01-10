/**
 * @file pipeline.cpp
 * @brief Video processing pipeline implementation
 *
 * @details Orchestrates the entire video analysis and cutting workflow:
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
 * @note When stream_id >= 0, all log messages are prefixed with [Stream N].
 *       When num_threads is specified, internal parallelism is limited.
 */

#include "motion_trim/pipeline.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <thread>
#include <vector>

/// Linux syscall headers for memfd_create
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <fmt/color.h>
#include <fmt/core.h>

#include "motion_trim/config.hpp"
#include "motion_trim/ffmpeg_queue.hpp"
#include "motion_trim/logging.hpp"
#include "motion_trim/memory_io.hpp"
#include "motion_trim/motion_scanner.hpp"
#include "motion_trim/system.hpp"
#include "motion_trim/task_queue.hpp"

namespace motion_trim {

// **---- Static Members ----**

/// Static mutex for sequential FFmpeg execution (prevents memory bus
/// contention)
std::mutex ProcessingPipeline::ffmpeg_mutex_;

// **---- Constructor ----**

ProcessingPipeline::ProcessingPipeline(std::string in, std::string out,
                                       int stream_id, int num_threads,
                                       std::vector<int> cpu_set)
    : input_path(std::move(in)), output_path(std::move(out)),
      stream_id_(stream_id), num_threads_(num_threads),
      cpu_set_(std::move(cpu_set)) {}

// **---- Logging Helpers ----**

void ProcessingPipeline::log_info(const std::string &msg) {
  if (stream_id_ >= 0) {
    LOG_INFO("[Stream {}] {}", stream_id_, msg);
  } else {
    LOG_INFO("{}", msg);
  }
}

void ProcessingPipeline::log_phase(const std::string &msg) {
  if (stream_id_ >= 0) {
    LOG_PHASE("[Stream {}] {}", stream_id_, msg);
  } else {
    LOG_PHASE("{}", msg);
  }
}

// **---- Main Processing ----**

int ProcessingPipeline::run() {
  TIMER_START(total_run);

  // **----- PHASE 0: MAP FILE INTO RAM -----**

  log_phase("Mapping RAM...");
  if (!MemoryLoader::load_file(input_path, file_buffer)) {
    if (stream_id_ >= 0) {
      LOG_ERROR("[Stream {}] Failed to map file: {}", stream_id_, input_path);
    } else {
      LOG_ERROR("Failed to map file: {}", input_path);
    }
    return 1;
  }
  log_info(fmt::format("Mapped {} MB", file_buffer.size() / 1024 / 1024));

  // **----- PROBE VIDEO METADATA -----**

  double fps = 0;
  {
    TIMER_START(probe);
    MotionScanner probe(file_buffer);
    if (!probe.initialize()) {
      if (stream_id_ >= 0) {
        LOG_ERROR("[Stream {}] Failed to initialize probe", stream_id_);
      } else {
        LOG_ERROR("Failed to initialize probe");
      }
      return 1;
    }
    duration = probe.get_duration();
    fps = probe.get_fps();
    TIMER_END(probe);

    log_info(fmt::format("Duration: {} ({:.0f} frames @ {:.1f}fps)",
                         format_time(duration), duration * fps, fps));
  }

  // **----- PHASE 1: SCANNING -----**

  /// Determine number of internal threads
  int num_threads;
  if (num_threads_ > 0) {
    /// Use specified thread count (batch mode)
    num_threads = num_threads_;
  } else {
    /// Auto-detect (single file mode)
    int cpu_limit = detect_cpu_limit();
    num_threads = std::max(2, cpu_limit);
  }

  /// Don't use more threads than chunks
  int num_chunks =
      static_cast<int>(std::ceil(duration / Config::chunk_duration_sec()));
  num_threads = std::min(num_threads, num_chunks);

  if (num_threads == 1) {
    log_phase(fmt::format("Scanning ({:.0f}s chunks)...",
                          Config::chunk_duration_sec()));
  } else {
    log_phase(fmt::format("Parallel Scan ({} threads, {:.0f}s chunks)...",
                          num_threads, Config::chunk_duration_sec()));
  }

  TIMER_START(parallel_scan);

  // **----- SUB-PHASE: Setup task queue -----**

  auto setup_start = std::chrono::high_resolution_clock::now();

  TaskQueue task_queue;
  ResultCollector results;
  results.reserve(static_cast<size_t>(duration * 5));

  int chunk_id = 0;
  for (double t = 0; t < duration; t += Config::chunk_duration_sec()) {
    double end = std::min(t + Config::chunk_duration_sec(), duration);
    task_queue.push({t, end, chunk_id++});
  }
  log_info(fmt::format("Created {} chunks", chunk_id));

  auto setup_end = std::chrono::high_resolution_clock::now();

  // **----- SUB-PHASE: Worker execution -----**

  auto workers_start = std::chrono::high_resolution_clock::now();

  std::vector<std::thread> workers;

  /// NOTE: Each padded atomic is on its own cache line (64 bytes apart)
  /// This prevents false sharing when multiple threads update the counters
  PaddedAtomic<int> chunks_done{0};
  PaddedAtomic<long> total_init_us{0};
  PaddedAtomic<long> total_seek_us{0};
  PaddedAtomic<long> total_decode_us{0};
  PaddedAtomic<long> total_analyze_us{0};

  for (int i = 0; i < num_threads; ++i) {
    workers.emplace_back([this, &task_queue, &results, &chunks_done,
                          &total_init_us, &total_seek_us, &total_decode_us,
                          &total_analyze_us]() {
      /// Pin worker to stream's CPU set if specified
      if (!cpu_set_.empty()) {
        pin_thread_to_cpus(cpu_set_);
      }

      auto init_start = std::chrono::high_resolution_clock::now();

      MotionScanner scanner(file_buffer);
      if (!scanner.initialize())
        return;

      auto init_end = std::chrono::high_resolution_clock::now();
      /// Thread-local init time (accumulated once at end)
      long local_init_us =
          std::chrono::duration_cast<std::chrono::microseconds>(init_end -
                                                                init_start)
              .count();

      long local_seek_us = 0;
      long local_decode_us = 0;
      long local_analyze_us = 0;

      /// Track chunks locally to reduce atomic contention
      int local_chunks = 0;

      ScanTask task;
      while (task_queue.pop(task)) {
        auto chunk_results =
            scanner.scan_range(task.start, task.end, local_seek_us,
                               local_decode_us, local_analyze_us);

        if (!chunk_results.empty()) {
          results.add(std::move(chunk_results));
        }

        ++local_chunks;
      }

      /// Single atomic update at end (avoids per-chunk contention)
      chunks_done += local_chunks;
      total_init_us += local_init_us;
      total_seek_us += local_seek_us;
      total_decode_us += local_decode_us;
      total_analyze_us += local_analyze_us;
    });
  }

  // **----- SUB-PHASE: Join workers -----**

  auto join_start = std::chrono::high_resolution_clock::now();

  task_queue.finish();
  for (auto &w : workers) {
    w.join();
  }

  auto join_end = std::chrono::high_resolution_clock::now();
  auto workers_end = join_end;

  auto setup_us = std::chrono::duration_cast<std::chrono::microseconds>(
                      setup_end - setup_start)
                      .count();
  auto workers_us = std::chrono::duration_cast<std::chrono::microseconds>(
                        workers_end - workers_start)
                        .count();
  auto join_us = std::chrono::duration_cast<std::chrono::microseconds>(
                     join_end - join_start)
                     .count();

  double init_avg_sec = (total_init_us.load() / num_threads) / 1000000.0;
  double seek_avg_sec = (total_seek_us.load() / num_threads) / 1000000.0;
  double decode_avg_sec = (total_decode_us.load() / num_threads) / 1000000.0;
  double analyze_avg_sec = (total_analyze_us.load() / num_threads) / 1000000.0;

  long total_scan_us =
      total_seek_us.load() + total_decode_us.load() + total_analyze_us.load();
  double scan_avg_sec = (total_scan_us / num_threads) / 1000000.0;

  std::vector<double> timestamps = results.extract();

  TIMER_END(parallel_scan);

  /// Only record detailed timing in single-file mode
  if (stream_id_ < 0) {
    TimingCollector::record("  ├─setup", setup_us);
    TimingCollector::record("  ├─workers", workers_us);
    TimingCollector::record(
        fmt::format("  │ ├─init ({}T×{:.2f}s)", num_threads, init_avg_sec),
        total_init_us.load());
    TimingCollector::record(
        fmt::format("  │ └─scan ({}T×{:.2f}s)", num_threads, scan_avg_sec),
        total_scan_us);
    TimingCollector::record(
        fmt::format("  │   ├─seek ({}T×{:.3f}s)", num_threads, seek_avg_sec),
        total_seek_us.load());
    TimingCollector::record(fmt::format("  │   ├─decode ({}T×{:.2f}s)",
                                        num_threads, decode_avg_sec),
                            total_decode_us.load());
    TimingCollector::record(fmt::format("  │   └─analyze ({}T×{:.2f}s)",
                                        num_threads, analyze_avg_sec),
                            total_analyze_us.load());
    TimingCollector::record("  └─join", join_us);
  }

  log_info(fmt::format("Processed {} chunks, found {} motion frames",
                       chunks_done.load(), timestamps.size()));

  // **----- PHASE 2: MERGE AND DEDUPLICATE -----**

  log_phase("Merging...");
  TIMER_START(merge);

  std::sort(timestamps.begin(), timestamps.end());
  auto last = std::unique(timestamps.begin(), timestamps.end());
  timestamps.erase(last, timestamps.end());

  TIMER_END(merge);

  if (timestamps.empty()) {
    if (stream_id_ >= 0) {
      LOG_WARN("[Stream {}] No motion found.", stream_id_);
    } else {
      LOG_WARN("No motion found.");
    }
    TIMER_END(total_run);
    if (stream_id_ < 0) {
      TimingCollector::print_summary();
    }
    return 0;
  }

  // **----- PHASE 3: SEGMENTATION -----**

  TIMER_START(segmentation);

  std::vector<TimeSegment> segments;
  segments.reserve(100);

  double curr_start = timestamps[0];
  double last_act = timestamps[0];

  for (size_t i = 1; i < timestamps.size(); ++i) {
    double gap_val = timestamps[i] - last_act;
    if (gap_val > Config::max_gap_sec()) {
      log_info(fmt::format(
          "Gap: {}s -> {}s (Skipping {}s)", static_cast<int>(last_act),
          static_cast<int>(timestamps[i]), static_cast<int>(gap_val)));
      segments.push_back({std::max(0.0, curr_start - Config::padding_sec()),
                          last_act + Config::padding_sec()});
      curr_start = timestamps[i];
    }
    last_act = timestamps[i];
  }
  segments.push_back({std::max(0.0, curr_start - Config::padding_sec()),
                      last_act + Config::padding_sec()});

  TIMER_END(segmentation);

  /// Calculate savings
  double out_dur = 0;
  for (auto &s : segments) {
    s.end = std::min(s.end, duration);
    s.start = std::min(s.start, s.end);
    out_dur += (s.end - s.start);
  }
  time_removed = duration - out_dur;
  saved_pct = (duration > 0) ? time_removed / duration * 100.0 : 0.0;

  if (saved_pct > Config::min_savings_pct()) {
    /// Check if we should push to queue (batch mode) or execute directly
    if (ffmpeg_queue_) {
      /// Push to FFmpeg queue for deferred execution
      FFmpegJob job;
      job.stream_id = stream_id_;
      job.input_path = std::filesystem::absolute(input_path).string();
      job.output_path = output_path;
      job.segments = segments;
      job.cpu_set = cpu_set_;
      ffmpeg_queue_->push(std::move(job));
      log_info("Pushed FFmpeg job to queue");
    } else {
      /// Execute immediately (single-file mode)
      execute_cut(segments);
    }
  } else {
    if (stream_id_ >= 0) {
      LOG_WARN(
          "[Stream {}] Savings too low ({}%). Min required: {}%. Copying full stream.",
          stream_id_, static_cast<int>(saved_pct),
          static_cast<int>(Config::min_savings_pct()));
    } else {
      LOG_WARN("Savings too low ({}%). Min required: {}%. Copying full stream.",
               static_cast<int>(saved_pct),
               static_cast<int>(Config::min_savings_pct()));
    }

    /// Create a single segment covering the whole duration
    std::vector<TimeSegment> full_copy_segment;
    full_copy_segment.push_back({0.0, duration});

    if (ffmpeg_queue_) {
      /// Push to FFmpeg queue for deferred execution
      FFmpegJob job;
      job.stream_id = stream_id_;
      job.input_path = std::filesystem::absolute(input_path).string();
      job.output_path = output_path;
      job.segments = full_copy_segment;
      job.cpu_set = cpu_set_;
      ffmpeg_queue_->push(std::move(job));
      log_info("Pushed full-copy job to queue");
    } else {
      /// Execute immediately (single-file mode)
      execute_cut(full_copy_segment);
    }
  }

  TIMER_END(total_run);

  /// Only print timing summary in single-file mode
  if (stream_id_ < 0) {
    TimingCollector::print_summary();
  }
  print_cut_summary();

  return 0;
}

// **---- Cut Summary ----**

void ProcessingPipeline::print_cut_summary() {
  std::string prefix =
      (stream_id_ >= 0) ? fmt::format("[Stream {}] ", stream_id_) : "";

  fmt::print("\n");
  if (stream_id_ >= 0) {
    fmt::print(fg(fmt::color::cyan), "{}========= CUT SUMMARY =========\n",
               prefix);
  } else {
    fmt::print(fg(fmt::color::cyan),
               "=================== CUT SUMMARY ====================\n");
  }

  fmt::print("{}{:<20} {:>15}\n", prefix, "Original:", format_time(duration));
  fmt::print("{}{:<20} {:>15}\n", prefix,
             "Output:", format_time(duration - time_removed));
  fmt::print("{}{:<20} {:>15}\n", prefix,
             "Removed:", format_time(time_removed));
  fmt::print("{}{:<20} {:>14}%\n", prefix,
             "Saved:", static_cast<int>(saved_pct));

  if (stream_id_ >= 0) {
    fmt::print(fg(fmt::color::cyan), "{}===============================\n",
               prefix);
  } else {
    fmt::print(fg(fmt::color::cyan),
               "====================================================\n");
  }
  std::fflush(stdout);
}

// **---- FFmpeg Execution ----**

void ProcessingPipeline::execute_cut(const std::vector<TimeSegment> &segments) {
  TIMER_START(execute_cut);

  log_phase("Cutting...");

  auto cut_list_start = std::chrono::high_resolution_clock::now();

  std::string list_content;
  list_content.reserve(4096);

  std::string abs_path = std::filesystem::absolute(input_path).string();

  for (const auto &s : segments) {
    if (s.end <= s.start)
      continue;
    list_content += fmt::format("file '{}'\n", abs_path);
    list_content += fmt::format("inpoint {:.2f}\n", s.start);
    list_content += fmt::format("outpoint {:.2f}\n", s.end);
  }

  auto cut_list_end = std::chrono::high_resolution_clock::now();

  auto memfd_start = std::chrono::high_resolution_clock::now();

  int fd = syscall(SYS_memfd_create, "cut_list_mem", MFD_CLOEXEC);
  if (fd == -1) {
    if (stream_id_ >= 0) {
      LOG_ERROR("[Stream {}] Failed to create memory file!", stream_id_);
    } else {
      LOG_ERROR("Failed to create memory file! (kernel >= 3.17 required)");
    }
    TIMER_END(execute_cut);
    return;
  }

  if (write(fd, list_content.c_str(), list_content.size()) == -1) {
    if (stream_id_ >= 0) {
      LOG_ERROR("[Stream {}] Failed to write to memory file", stream_id_);
    } else {
      LOG_ERROR("Failed to write to memory file");
    }
    close(fd);
    TIMER_END(execute_cut);
    return;
  }

  std::string mem_file_path = fmt::format("/proc/{}/fd/{}", getpid(), fd);

  /// Build FFmpeg command, with optional CPU pinning via taskset
  std::string cmd;
  if (!cpu_set_.empty()) {
    /// Build CPU list string for taskset
    std::string cpu_list;
    for (size_t i = 0; i < cpu_set_.size(); ++i) {
      if (i > 0)
        cpu_list += ",";
      cpu_list += std::to_string(cpu_set_[i]);
    }
    cmd = fmt::format(
        "taskset -c {} /usr/local/bin/ffmpeg -y -hide_banner -loglevel error "
        "-f concat -safe 0 -protocol_whitelist file,pipe,fd -i \"{}\" "
        "-c copy -fflags +genpts -avoid_negative_ts make_zero "
        "-movflags +faststart \"{}\"",
        cpu_list, mem_file_path, output_path);
  } else {
    cmd = fmt::format(
        "/usr/local/bin/ffmpeg -y -hide_banner -loglevel error "
        "-f concat -safe 0 -protocol_whitelist file,pipe,fd -i \"{}\" "
        "-c copy -fflags +genpts -avoid_negative_ts make_zero "
        "-movflags +faststart \"{}\"",
        mem_file_path, output_path);
  }

  auto memfd_end = std::chrono::high_resolution_clock::now();

  auto ffmpeg_start = std::chrono::high_resolution_clock::now();

  log_info("Running FFmpeg...");

  /// Run FFmpeg (parallel across streams - pinned to stream's CPU set via
  /// taskset)
  int status = std::system(cmd.c_str());

  close(fd);

  auto ffmpeg_end = std::chrono::high_resolution_clock::now();

  auto build_list_us = std::chrono::duration_cast<std::chrono::microseconds>(
                           cut_list_end - cut_list_start)
                           .count();
  auto memfd_us = std::chrono::duration_cast<std::chrono::microseconds>(
                      memfd_end - memfd_start)
                      .count();
  auto ffmpeg_us = std::chrono::duration_cast<std::chrono::microseconds>(
                       ffmpeg_end - ffmpeg_start)
                       .count();

  if (status != 0) {
    int exit_code = (status >> 8) & 0xFF;
    if (stream_id_ >= 0) {
      LOG_ERROR("[Stream {}] FFmpeg exited with error code: {}", stream_id_,
                exit_code);
    } else {
      LOG_ERROR("FFmpeg exited with error code: {}", exit_code);
    }
  } else {
    if (stream_id_ >= 0) {
      LOG_SUCCESS("[Stream {}] Output saved to: {}", stream_id_, output_path);
    } else {
      LOG_SUCCESS("Output saved to: {}", output_path);
    }
  }

  TIMER_END(execute_cut);

  /// Only record detailed timing in single-file mode
  if (stream_id_ < 0) {
    TimingCollector::record("  ├─build_list", build_list_us);
    TimingCollector::record("  ├─memfd_setup", memfd_us);
    TimingCollector::record("  └─ffmpeg_exec", ffmpeg_us);
  }
}

} // namespace motion_trim
