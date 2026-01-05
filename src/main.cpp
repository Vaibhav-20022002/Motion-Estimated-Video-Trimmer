/**
 * @file main.cpp
 * @brief Entry point for Motion Trim application
 *
 * @details Main entry point that handles:
 *
 *          - Command-line argument parsing
 *
 *          - Single file mode: process one video
 *
 *          - Batch directory mode: parallel processing with BatchProcessor
 *
 * @note In batch mode, the application uses parallel streams for concurrent
 *       video processing. Each stream is pinned to specific CPU cores.
 *       Set PARALLEL_STREAMS environment variable to control parallelism.
 */

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "motion_trim/batch_processor.hpp"
#include "motion_trim/config.hpp"
#include "motion_trim/logging.hpp"
#include "motion_trim/pipeline.hpp"
#include "motion_trim/system.hpp"

using namespace motion_trim;

// **---- MAIN ----**

int main(int argc, char *argv[]) {
  /// Disable stdout buffering for real-time log visibility
  std::setvbuf(stdout, nullptr, _IONBF, 0);

  if (argc < 3) {
    LOG_WARN("Usage: ./motion_trim <input> <output>");
    return 1;
  }

  namespace fs = std::filesystem;
  std::string input_arg = argv[1];
  std::string output_arg = argv[2];

  if (fs::is_directory(input_arg)) {
    // **---- BATCH MODE - Parallel video processing ----**

    if (!fs::exists(output_arg)) {
      fs::create_directories(output_arg);
    }

    LOG_INFO("Motion Trim - Batch Mode");
    LOG_INFO("Input directory: {}", input_arg);
    LOG_INFO("Output directory: {}", output_arg);

    /// Collect video files
    std::vector<std::string> files;
    for (const auto &entry : fs::directory_iterator(input_arg)) {
      if (entry.is_regular_file()) {
        std::string ext = entry.path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        if (ext == ".mp4" || ext == ".mkv" || ext == ".ts" || ext == ".mov" ||
            ext == ".avi") {
          files.push_back(entry.path().string());
        }
      }
    }
    std::sort(files.begin(), files.end());

    if (files.empty()) {
      LOG_WARN("No video files found in directory");
      return 0;
    }

    LOG_INFO("Found {} video files", files.size());

    /// Process using BatchProcessor
    int num_streams = Config::parallel_streams(); // 0 = auto-detect
    BatchProcessor processor(num_streams);
    return processor.process(files, output_arg);

  } else {

    // **---- SINGLE FILE MODE ----**

    LOG_INFO("Motion Trim - Single File Mode");
    LOG_INFO("Input: {}", input_arg);
    LOG_INFO("Output: {}", output_arg);

    /// Use THREADS_PER_STREAM config (0 = auto-detect)
    int num_threads = Config::threads_per_stream();
    ProcessingPipeline app(input_arg, output_arg, -1, num_threads);
    return app.run();
  }
}
