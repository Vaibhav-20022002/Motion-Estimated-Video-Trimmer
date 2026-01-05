/**
 * @file ffmpeg_executor.hpp
 * @brief Standalone FFmpeg execution for cut operations
 *
 * @details Separate module for executing FFmpeg cut commands.
 *          Enables producer-consumer pattern where:
 * 
 *          - Stream workers scan and push jobs to queue
 * 
 *          - FFmpeg worker executes jobs sequentially
 */

#ifndef MOTION_TRIM_FFMPEG_EXECUTOR_HPP
#define MOTION_TRIM_FFMPEG_EXECUTOR_HPP

#include <string>
#include <vector>

#include "types.hpp"

namespace motion_trim {

/**
 * @brief Execute FFmpeg to cut a video based on segments.
 *
 * @param input_path Absolute path to input video
 * @param output_path Absolute path to output video
 * @param segments Time segments to keep
 * @param cpu_set CPU set for taskset pinning (empty = no pinning)
 * @param stream_id Stream ID for logging (-1 = no prefix)
 * @return 0 on success, non-zero on error
 */
int execute_ffmpeg_cut(const std::string &input_path,
                       const std::string &output_path,
                       const std::vector<TimeSegment> &segments,
                       const std::vector<int> &cpu_set = {},
                       int stream_id = -1);

} // namespace motion_trim

#endif // MOTION_TRIM_FFMPEG_EXECUTOR_HPP
