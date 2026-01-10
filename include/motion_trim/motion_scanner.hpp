/**
 * @file motion_scanner.hpp
 * @brief Motion vector analysis and detection
 *
 * @details The MotionScanner class decodes video and analyzes motion vectors
 *          to detect significant motion in video frames.
 *
 * @attention THREAD MODEL:
 *            - Each worker thread creates its own MotionScanner instance.
 *
 *            - This is necessary because FFmpeg decoder state is not
 *              thread-safe.
 *
 */

#ifndef MOTION_TRIM_MOTION_SCANNER_HPP
#define MOTION_TRIM_MOTION_SCANNER_HPP

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

#include <cstdint>
#include <vector>

#include "memory_io.hpp"
#include "types.hpp"

namespace motion_trim {

/**
 * @class MotionScanner
 * @brief Decodes video and analyzes motion vectors.
 *
 * @attention
 * `THREAD MODEL`:
 *
 *            - Each worker thread creates its own MotionScanner instance.
 *
 *            - This is necessary because FFmpeg decoder state is not
 *              thread-safe.
 *
 * `MANAGEMENT`:
 *
 *            - Uses AVFMT_FLAG_CUSTOM_IO for proper cleanup of custom I/O
 *
 *            - Destructor handles partial initialization failures
 *
 *            - All FFmpeg resources are freed in reverse allocation order
 *
 * `PERFORMANCE`:
 *
 *            - Config values cached in aligned struct (avoids function calls in
 *              hot loop)
 *
 *            - Grid buffer pre-allocated (avoids malloc in hot loop)
 *
 *            - Bounds checking for motion vectors (prevents segfaults)
 */
class MotionScanner {
  /// FFmpeg contexts (owned by this instance)
  AVFormatContext *fmt_ctx = nullptr;
  AVCodecContext *dec_ctx = nullptr;
  AVFrame *frame = nullptr;
  AVPacket *pkt = nullptr;
  AVIOContext *avio_ctx = nullptr;
  uint8_t *avio_buffer = nullptr;

  /// Memory I/O state
  MemReaderState mem_state;
  int video_stream_idx = -1;

  /// Pre-allocated grid for motion voting (avoid malloc in hot loop)
  std::vector<uint8_t> grid_votes;
  int16_t grid_w = 0; //< Grid width in blocks (int16 sufficient for 32K video)
  int16_t grid_h = 0; //< Grid height in blocks

  /**
   * @brief Memoized configuration values.
   * @note Cache-line aligned to prevent false sharing in multi-threaded access.
   *       These are copied from Config:: once during initialization to avoid
   *       function call overhead in the hot loop.
   *       Members ordered by size (largest first) for optimal packing.
   */
  alignas(CACHE_LINE_SIZE) struct {
    double mv_threshold_sq; //< Squared magnitude threshold (8 bytes)
    int block_shift;        //< Bit shift for block size division (4 bytes)
    int clusters_needed;    //< Min clusters to trigger (4 bytes)
    int vertical_margin;    //< Rows to skip at top/bottom (4 bytes)
    uint8_t vectors_needed; //< Min vectors per block (1 byte)
    uint8_t _pad[3];        //< Explicit padding for alignment
  } cfg;

  /// Reference to shared file buffer (not owned)
  const MappedFile &file_data;

  /**
   * @brief Check if a frame contains significant motion.
   *
   * @attention This is the HOT PATH - called for every decoded frame.
   *
   * @param f Decoded frame with motion vector side data
   * @return true if significant motion detected
   */
  bool check_frame(const AVFrame *f);

public:
  /**
   * @brief Constructor: Allocate frame and packet structures.
   * @attention The actual decoder setup happens in initialize().
   */
  explicit MotionScanner(const MappedFile &data);

  /**
   * @brief Destructor: Clean up all FFmpeg resources.
   * @attention Handles the case where initialization failed partway.
   */
  ~MotionScanner();

  /// Disable copy (FFmpeg contexts are not copyable)
  MotionScanner(const MotionScanner &) = delete;
  MotionScanner &operator=(const MotionScanner &) = delete;

  /**
   * @brief Initialize decoder and prepare for scanning.
   * @return true on success, false on failure
   */
  bool initialize();

  /**
   * @brief Get video duration in seconds.
   */
  double get_duration();

  /**
   * @brief Get video frame rate.
   */
  double get_fps();

  /**
   * @brief Scan a time range and return timestamps with motion.
   *
   * @param start Start time in seconds
   * @param end End time in seconds
   * @param seek_us Output: accumulated seek time in microseconds
   * @param decode_us Output: accumulated decode time in microseconds
   * @param analyze_us Output: accumulated check_frame time in microseconds
   * @return Vector of timestamps (in seconds) where motion was detected
   */
  std::vector<double> scan_range(double start, double end, long &seek_us,
                                 long &decode_us, long &analyze_us);
};

} // namespace motion_trim

#endif // MOTION_TRIM_MOTION_SCANNER_HPP
