/**
 * @file main.cpp
 * @brief Entry point for Motion Trim application
 *
 * @details Main entry point that handles:
 *          - Command-line argument parsing
 * 
 *          - Single file mode
 * 
 *          - Batch directory mode
 */

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "motion_trim/logging.hpp"
#include "motion_trim/pipeline.hpp"

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

    // **----- BATCH MODE -----**

    if (!fs::exists(output_arg)) {
      fs::create_directories(output_arg);
    }

    LOG_INFO("Batch Mode: Scanning directory {}", input_arg);

    std::vector<fs::path> files;
    for (const auto &entry : fs::directory_iterator(input_arg)) {
      if (entry.is_regular_file()) {
        std::string ext = entry.path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        if (ext == ".mp4" || ext == ".mkv" || ext == ".ts" || ext == ".mov" ||
            ext == ".avi") {
          files.push_back(entry.path());
        }
      }
    }
    std::sort(files.begin(), files.end());

    int failures = 0;
    for (const auto &path : files) {
      std::string out_file = (fs::path(output_arg) / path.filename()).string();
      LOG_INFO("--------------------------------------------------");
      LOG_INFO("Processing: {}", path.filename().string());

      ProcessingPipeline app(path.string(), out_file);
      if (app.run() != 0) {
        LOG_ERROR("Failed to process: {}", path.string());
        failures++;
      }
      TimingCollector::clear();
    }
    LOG_INFO("Batch completed. {} failures.", failures);
    return failures > 0 ? 1 : 0;

  } else {

    // **----- SINGLE FILE MODE -----**
    
    LOG_INFO("Motion Trim :");
    LOG_INFO("Input: {}", input_arg);
    LOG_INFO("Output: {}", output_arg);

    ProcessingPipeline app(input_arg, output_arg);
    return app.run();
  }
}
