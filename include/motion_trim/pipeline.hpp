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
 */

#ifndef MOTION_TRIM_PIPELINE_HPP
#define MOTION_TRIM_PIPELINE_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "types.hpp"

namespace motion_trim {

/**
 * @class ProcessingPipeline
 * @brief Orchestrates the entire video analysis and cutting.
 *
 * @attention WORKFLOW:
 *
 * 1. Load video into RAM
 *
 * 2. Probe video metadata (duration, fps)
 *
 * 3. Create task queue with small chunks
 *
 * 4. Launch worker threads that steal tasks from queue
 *
 * 5. Collect and merge results
 *
 * 6. Generate cut segments
 *
 * 7. Execute FFmpeg to produce output
 */
class ProcessingPipeline {
  std::vector<uint8_t> file_buffer;
  std::string input_path;
  std::string output_path;
  double duration = 0;
  double time_removed = 0;
  double saved_pct = 0;

  /**
   * @brief Print summary of what was cut.
   */
  void print_cut_summary();

  /**
   * @brief Execute FFmpeg to produce the cut video.
   *
   * @attention Uses memfd_create to create an in-memory file for the concat
   *            list, avoiding disk I/O entirely.
   */
  void execute_cut(const std::vector<TimeSegment> &segments);

public:
  /**
   * @brief Construct a processing pipeline.
   * @param in Input file path
   * @param out Output file path
   */
  ProcessingPipeline(std::string in, std::string out);

  /**
   * @brief Run the complete processing pipeline.
   * @return 0 on success, non-zero on error
   */
  int run();
};

} // namespace motion_trim

#endif // MOTION_TRIM_PIPELINE_HPP
