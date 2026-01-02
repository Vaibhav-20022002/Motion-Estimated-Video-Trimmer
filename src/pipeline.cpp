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

#include <fmt/core.h>

#include "motion_trim/config.hpp"
#include "motion_trim/logging.hpp"
#include "motion_trim/memory_io.hpp"
#include "motion_trim/motion_scanner.hpp"
#include "motion_trim/system.hpp"
#include "motion_trim/task_queue.hpp"

namespace motion_trim {

ProcessingPipeline::ProcessingPipeline(std::string in, std::string out)
    : input_path(std::move(in)), output_path(std::move(out)) {}

int ProcessingPipeline::run() {
  TIMER_START(total_run);

  // **----- PHASE 0: LOAD FILE INTO RAM -----**

  LOG_PHASE("[PHASE 0] Loading RAM...");
  if (!MemoryLoader::load_file(input_path, file_buffer)) {
    LOG_ERROR("Failed to load file: {}", input_path);
    return 1;
  }
  LOG_INFO("Loaded {} MB", file_buffer.size() / 1024 / 1024);

  // **----- PROBE VIDEO METADATA -----**

  {
    TIMER_START(probe);
    MotionScanner probe(file_buffer);
    if (!probe.initialize()) {
      LOG_ERROR("Failed to initialize probe");
      return 1;
    }
    duration = probe.get_duration();
    TIMER_END(probe);

    LOG_INFO("Duration: {} ({:.0f} frames @ {:.1f}fps)", format_time(duration),
             duration * probe.get_fps(), probe.get_fps());
  }

  // **----- PHASE 1: DYNAMIC PARALLEL SCAN -----**

  /// Use cgroup-aware CPU detection for Docker compatibility
  int cpu_limit = detect_cpu_limit();

  /// Use detected limit, but ensure at least 2 threads
  int num_threads = std::max(2, cpu_limit);

  /// Don't use more threads than chunks
  int num_chunks =
      static_cast<int>(std::ceil(duration / Config::chunk_duration_sec()));
  num_threads = std::min(num_threads, num_chunks);

  LOG_PHASE("[PHASE 1] Parallel Scan ({} threads, {:.0f}s chunks)...",
            num_threads, Config::chunk_duration_sec());

  TIMER_START(parallel_scan);

  // **----- SUB-PHASE: Setup task queue -----**

  auto setup_start = std::chrono::high_resolution_clock::now();

  /// Create task queue and result collector
  TaskQueue task_queue;
  ResultCollector results;
  results.reserve(
      static_cast<size_t>(duration * 5)); //< Estimate 5 motion-frames/sec

  /// Generate work chunks
  int chunk_id = 0;
  for (double t = 0; t < duration; t += Config::chunk_duration_sec()) {
    double end = std::min(t + Config::chunk_duration_sec(), duration);
    task_queue.push({t, end, chunk_id++});
  }
  LOG_INFO("Created {} chunks", chunk_id);

  auto setup_end = std::chrono::high_resolution_clock::now();

  // **----- SUB-PHASE: Worker execution -----**

  auto workers_start = std::chrono::high_resolution_clock::now();

  /// Launch worker threads
  std::vector<std::thread> workers;
  std::atomic<int> chunks_done{0};

  /// Per-worker timing aggregation
  std::atomic<long> total_init_us{0};
  std::atomic<long> total_seek_us{0};
  std::atomic<long> total_decode_us{0};
  std::atomic<long> total_analyze_us{0};

  for (int i = 0; i < num_threads; ++i) {
    workers.emplace_back([this, &task_queue, &results, &chunks_done,
                          &total_init_us, &total_seek_us, &total_decode_us,
                          &total_analyze_us]() {
      // **----- Worker init timing -----**

      auto init_start = std::chrono::high_resolution_clock::now();

      /// Each worker creates its own scanner (decoder state is not
      /// thread-safe)
      MotionScanner scanner(file_buffer);
      if (!scanner.initialize())
        return;

      auto init_end = std::chrono::high_resolution_clock::now();
      total_init_us += std::chrono::duration_cast<std::chrono::microseconds>(
                           init_end - init_start)
                           .count();

      /// Per-worker local timing accumulators
      long local_seek_us = 0;
      long local_decode_us = 0;
      long local_analyze_us = 0;

      ScanTask task;
      while (task_queue.pop(task)) {
        /// Process chunk with detailed timing
        auto chunk_results =
            scanner.scan_range(task.start, task.end, local_seek_us,
                               local_decode_us, local_analyze_us);

        /// Move results to collector (avoid copy)
        if (!chunk_results.empty()) {
          results.add(std::move(chunk_results));
        }

        ++chunks_done;
      }

      /// Aggregate local timings to global atomics
      total_seek_us += local_seek_us;
      total_decode_us += local_decode_us;
      total_analyze_us += local_analyze_us;
    });
  }

  // **----- SUB-PHASE: Join workers -----**

  auto join_start = std::chrono::high_resolution_clock::now();

  /// Signal completion and wait for all workers
  task_queue.finish();
  for (auto &w : workers) {
    w.join();
  }

  auto join_end = std::chrono::high_resolution_clock::now();
  auto workers_end = join_end;

  /// Capture all timing values
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

  ///\note Total scan time = seek + decode + analyze (cumulative across
  /// threads)
  long total_scan_us =
      total_seek_us.load() + total_decode_us.load() + total_analyze_us.load();
  double scan_avg_sec = (total_scan_us / num_threads) / 1000000.0;

  /// Extract results using move semantics
  std::vector<double> timestamps = results.extract();

  /// Record parent timer FIRST, then children
  TIMER_END(parallel_scan);

  /// Now record sub-timers in hierarchical order
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
  TimingCollector::record(
      fmt::format("  │   ├─decode ({}T×{:.2f}s)", num_threads, decode_avg_sec),
      total_decode_us.load());
  TimingCollector::record(fmt::format("  │   └─analyze ({}T×{:.2f}s)",
                                      num_threads, analyze_avg_sec),
                          total_analyze_us.load());
  TimingCollector::record("  └─join", join_us);

  LOG_INFO("Processed {} chunks, found {} motion frames", chunks_done.load(),
           timestamps.size());

  // **----- PHASE 2: MERGE AND DEDUPLICATE -----**

  LOG_PHASE("[PHASE 2] Merging...");
  TIMER_START(merge);

  std::sort(timestamps.begin(), timestamps.end());
  auto last = std::unique(timestamps.begin(), timestamps.end());
  timestamps.erase(last, timestamps.end());

  TIMER_END(merge);

  if (timestamps.empty()) {
    LOG_WARN("No motion found.");
    TIMER_END(total_run);
    TimingCollector::print_summary();
    return 0;
  }

  // **----- PHASE 3: SEGMENTATION -----**

  TIMER_START(segmentation);

  std::vector<TimeSegment> segments;
  segments.reserve(100);

  double curr_start = timestamps[0];
  double last_act = timestamps[0];

  for (size_t i = 1; i < timestamps.size(); ++i) {
    double gap = timestamps[i] - last_act;
    if (gap > Config::max_gap_sec()) {
      /// Gap found - split here
      LOG_INFO("Gap: {}s -> {}s (Skipping {}s)", static_cast<int>(last_act),
               static_cast<int>(timestamps[i]), static_cast<int>(gap));
      segments.push_back({std::max(0.0, curr_start - Config::padding_sec()),
                          last_act + Config::padding_sec()});
      curr_start = timestamps[i];
    }
    last_act = timestamps[i];
  }
  /// Add final segment
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

  /// Only cut if savings are worthwhile
  if (saved_pct > Config::min_savings_pct()) {
    execute_cut(segments);
  } else {
    LOG_WARN("Savings too low ({}%). Min required: {}%. Skipping cut.",
             static_cast<int>(saved_pct),
             static_cast<int>(Config::min_savings_pct()));
  }

  TIMER_END(total_run);

  TimingCollector::print_summary();
  print_cut_summary();

  return 0;
}

void ProcessingPipeline::print_cut_summary() {
  fmt::print("\n");
  fmt::print(fg(fmt::color::cyan),
             "=================== CUT SUMMARY ====================\n");
  fmt::print("{:<25} {:>25}\n", "Original Duration:", format_time(duration));
  fmt::print("{:<25} {:>25}\n",
             "Output Duration:", format_time(duration - time_removed));
  fmt::print("{:<25} {:>25}\n", "Time Removed:", format_time(time_removed));
  fmt::print("{:<25} {:>24}%\n", "Saved:", static_cast<int>(saved_pct));
  fmt::print(fg(fmt::color::cyan),
             "====================================================\n");
  std::fflush(stdout);
}

void ProcessingPipeline::execute_cut(const std::vector<TimeSegment> &segments) {
  TIMER_START(execute_cut);

  LOG_PHASE("[PHASE 4] Cutting (In-Memory)...");

  // **----- SUB-PHASE: Build concat list -----**

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

  // **----- SUB-PHASE: Create memfd and write -----**
  
  auto memfd_start = std::chrono::high_resolution_clock::now();

  int fd = syscall(SYS_memfd_create, "cut_list_mem", MFD_CLOEXEC);
  if (fd == -1) {
    LOG_ERROR("Failed to create memory file! (kernel >= 3.17 required)");
    TIMER_END(execute_cut);
    return;
  }

  if (write(fd, list_content.c_str(), list_content.size()) == -1) {
    LOG_ERROR("Failed to write to memory file");
    close(fd);
    TIMER_END(execute_cut);
    return;
  }

  std::string mem_file_path = fmt::format("/proc/{}/fd/{}", getpid(), fd);

  std::string cmd = fmt::format(
      "/usr/local/bin/ffmpeg -y -hide_banner -loglevel error "
      "-f concat -safe 0 -protocol_whitelist file,pipe,fd -i \"{}\" "
      "-c copy -fflags +genpts -avoid_negative_ts make_zero "
      "-movflags +faststart \"{}\"",
      mem_file_path, output_path);

  auto memfd_end = std::chrono::high_resolution_clock::now();

  // **----- SUB-PHASE: Execute FFmpeg -----**

  auto ffmpeg_start = std::chrono::high_resolution_clock::now();

  LOG_INFO("Running FFmpeg...");

  int status = std::system(cmd.c_str());

  close(fd);

  auto ffmpeg_end = std::chrono::high_resolution_clock::now();

  /// Capture all timing values
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
    LOG_ERROR("FFmpeg exited with error code: {}", exit_code);
  } else {
    LOG_SUCCESS("Output saved to: {}", output_path);
  }

  /// Record parent timer FIRST, then children
  TIMER_END(execute_cut);
  TimingCollector::record("  ├─build_list", build_list_us);
  TimingCollector::record("  ├─memfd_setup", memfd_us);
  TimingCollector::record("  └─ffmpeg_exec", ffmpeg_us);
}

} // namespace motion_trim
