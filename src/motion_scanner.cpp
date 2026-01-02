/**
 * @file motion_scanner.cpp
 * @brief Motion vector analysis and detection implementation
 *
 * @details The MotionScanner class decodes video and analyzes motion vectors
 *          to detect significant motion in video frames.
 *
 * @attention OPTIMIZATIONS:
 * 
 *          - Config values cached in aligned struct (avoids function calls in
 *            hot loop)
 * 
 *          - Grid buffer pre-allocated (avoids malloc in hot loop)
 * 
 *          - Bounds checking for motion vectors (prevents segfaults)
 */

#include "motion_trim/motion_scanner.hpp"

#include <chrono>
#include <cstring>

extern "C" {
#include <libavutil/motion_vector.h>
}

#include "motion_trim/config.hpp"
#include "motion_trim/logging.hpp"

namespace motion_trim {

MotionScanner::MotionScanner(const std::vector<uint8_t> &data)
    : file_data(data) {
  frame = av_frame_alloc();
  pkt = av_packet_alloc();
}

MotionScanner::~MotionScanner() {
  /// Free decoder context first
  if (dec_ctx)
    avcodec_free_context(&dec_ctx);

  /// Handle format context and custom I/O
  if (fmt_ctx) {
    /// avformat_close_input closes the IO context and frees the buffer
    /// attached to it (avio_buffer) because we used av_malloc.
    avformat_close_input(&fmt_ctx);
  } else {
    /// fmt_ctx was never opened - clean up manually
    if (avio_ctx) {
      /// avio_context_free frees the internal buffer
      avio_context_free(&avio_ctx);
    } else if (avio_buffer) {
      /// Only free buffer manually if context wasn't created
      av_free(avio_buffer);
    }
  }

  av_frame_free(&frame);
  av_packet_free(&pkt);
}

bool MotionScanner::initialize() {
  /// Allocate format context
  fmt_ctx = avformat_alloc_context();
  if (!fmt_ctx) {
    LOG_ERROR("Failed to allocate AVFormatContext");
    return false;
  }

  /// Allocate I/O buffer
  avio_buffer = static_cast<uint8_t *>(av_malloc(AVIO_BUFFER_SIZE));
  if (!avio_buffer) {
    LOG_ERROR("Failed to allocate AVIO buffer");
    return false;
  }

  /// Set up memory I/O state
  mem_state = {file_data.data(), file_data.size(), 0};

  /// Create custom I/O context
  avio_ctx =
      avio_alloc_context(avio_buffer, AVIO_BUFFER_SIZE, 0, &mem_state,
                         MemoryLoader::read, nullptr, MemoryLoader::seek);
  if (!avio_ctx) {
    LOG_ERROR("Failed to allocate AVIOContext");
    return false;
  }

  /// Attach custom I/O to format context
  fmt_ctx->pb = avio_ctx;

  /// CRITICAL: Set flag to indicate we're using custom I/O
  ///\note This ensures proper cleanup behavior in avformat_close_input
  fmt_ctx->flags |= AVFMT_FLAG_CUSTOM_IO;

  /// Open input (probes format from memory)
  if (avformat_open_input(&fmt_ctx, "RAM", nullptr, nullptr) < 0) {
    LOG_ERROR("avformat_open_input failed");
    return false;
  }

  /// Find stream info (reads some packets to determine streams)
  if (avformat_find_stream_info(fmt_ctx, nullptr) < 0) {
    LOG_ERROR("avformat_find_stream_info failed");
    return false;
  }

  /// Find the best video stream
  video_stream_idx =
      av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
  if (video_stream_idx < 0) {
    LOG_ERROR("No video stream found");
    return false;
  }

  /// Discard non-video streams to save processing time
  for (unsigned int i = 0; i < fmt_ctx->nb_streams; i++) {
    if (i != static_cast<unsigned int>(video_stream_idx)) {
      fmt_ctx->streams[i]->discard = AVDISCARD_ALL;
    }
  }

  /// Find decoder for the video stream
  AVCodecParameters *param = fmt_ctx->streams[video_stream_idx]->codecpar;
  const AVCodec *codec = avcodec_find_decoder(param->codec_id);
  if (!codec) {
    /// Fallback for common codecs
    codec = (param->codec_id == AV_CODEC_ID_HEVC)
                ? avcodec_find_decoder_by_name("hevc")
                : avcodec_find_decoder_by_name("h264");
  }
  if (!codec) {
    LOG_ERROR("No decoder found for codec ID {}", (int)param->codec_id);
    return false;
  }

  /// Allocate decoder context
  dec_ctx = avcodec_alloc_context3(codec);
  if (!dec_ctx) {
    LOG_ERROR("Failed to allocate decoder context");
    return false;
  }
  avcodec_parameters_to_context(dec_ctx, param);

  // **--- DECODER OPTIMIZATIONS ---**

  /// Skip in-loop deblocking filter (we don't need visual quality)
  dec_ctx->skip_loop_filter = AVDISCARD_ALL;

  /// Skip the IDCT (we don't need actual image pixels)
  dec_ctx->skip_idct = AVDISCARD_ALL;

  /// Skip B-frames (they don't contain useful motion vectors for us)
  dec_ctx->skip_frame = AVDISCARD_BIDIR;

  /// Enable fast decoding flags
  dec_ctx->flags2 |= AV_CODEC_FLAG2_FAST;
  dec_ctx->flags |= AV_CODEC_FLAG_GRAY;

  /// Single-threaded decoding (we parallelize at chunk level instead)
  dec_ctx->thread_count = 1;
  dec_ctx->thread_type = FF_THREAD_SLICE;

  /// CRITICAL: Enable motion vector export to side data
  ///\note Without this flag,
  /// av_frame_get_side_data(AV_FRAME_DATA_MOTION_VECTORS)
  /// returns NULL and no motion is detected!
  AVDictionary *opts = nullptr;
  av_dict_set(&opts, "flags2", "+export_mvs", 0);

  /// Open decoder with export_mvs enabled
  int ret = avcodec_open2(dec_ctx, codec, &opts);
  av_dict_free(&opts);

  if (ret < 0) {
    LOG_ERROR("avcodec_open2 failed");
    return false;
  }

  // **--- MEMOIZE CONFIG VALUES ---**

  ///\note These are read once and cached in aligned struct to avoid
  /// function call overhead in the hot loop
  cfg.mv_threshold_sq = Config::mv_threshold_sq();
  cfg.block_shift = Config::block_shift();
  cfg.vectors_needed = Config::vectors_needed();
  cfg.clusters_needed = Config::clusters_needed();

  /// Calculate grid dimensions (int16_t sufficient for up to 32K video)
  grid_w = static_cast<int16_t>((dec_ctx->width + Config::block_size() - 1) >>
                                cfg.block_shift);
  grid_h = static_cast<int16_t>((dec_ctx->height + Config::block_size() - 1) >>
                                cfg.block_shift);

  /// Calculate vertical margin (rows to skip at top/bottom)
  cfg.vertical_margin = static_cast<int>(grid_h * Config::vertical_mask());

  /// Pre-allocate grid to avoid malloc in hot loop
  grid_votes.resize(grid_w * grid_h);

  return true;
}

double MotionScanner::get_duration() {
  return (fmt_ctx->duration != AV_NOPTS_VALUE)
             ? fmt_ctx->duration / static_cast<double>(AV_TIME_BASE)
             : 0.0;
}

double MotionScanner::get_fps() {
  if (video_stream_idx < 0)
    return 25.0;
  AVRational r = fmt_ctx->streams[video_stream_idx]->avg_frame_rate;
  return (r.den > 0) ? av_q2d(r) : 25.0;
}

bool MotionScanner::check_frame(const AVFrame *f) {
  /// Get motion vector side data (may be null for I-frames)
  AVFrameSideData *sd = av_frame_get_side_data(f, AV_FRAME_DATA_MOTION_VECTORS);
  if (!sd)
    return false;

  /// Cast to motion vector array
  const AVMotionVector *mvs =
      reinterpret_cast<const AVMotionVector *>(sd->data);
  const int count = sd->size / sizeof(AVMotionVector);

  /// Fast memset (compiler can optimize to SIMD)
  std::memset(grid_votes.data(), 0, grid_votes.size());
  uint8_t *__restrict grid = grid_votes.data();

  /// Cache everything in local variables for register allocation
  const double threshold_sq = cfg.mv_threshold_sq;
  const int block_shift = cfg.block_shift;
  const int gw = grid_w;
  const int gh = grid_h;
  const int y_min = cfg.vertical_margin;
  const int y_max = gh - cfg.vertical_margin;

  // **----- PHASE 1: VOTE ACCUMULATION -----**

  for (int i = 0; i < count; ++i) {
    const AVMotionVector *mv = &mvs[i];

    /// Calculate motion magnitude (squared to avoid sqrt)
    int dx = mv->dst_x - mv->src_x;
    int dy = mv->dst_y - mv->src_y;
    int mag_sq = dx * dx + dy * dy;

    /// Skip small motions
    if (mag_sq < threshold_sq)
      continue;

    /// Map to grid coordinates
    int gx = mv->dst_x >> block_shift;
    int gy = mv->dst_y >> block_shift;

    /// NOTE: Bounds checking is mandatory!
    /// Motion vectors can be negative or outside frame bounds due to
    /// padding macroblocks in H.264/HEVC. Without this check, we would
    /// write to invalid memory and crash.
    if (gx >= 0 && gx < gw && gy >= y_min && gy < y_max) {
      int idx = gy * gw + gx;
      /// Saturating increment (prevent wrap-around at 255)
      if (grid[idx] < 255)
        grid[idx]++;
    }
  }

  // **----- PHASE 2: CLUSTER DETECTION -----**

  const uint8_t vec_need = cfg.vectors_needed;
  const int clust_need = cfg.clusters_needed;
  int clusters = 0;

  // Scan grid for clusters (groups of adjacent active cells)
  for (int y = y_min; y < y_max; ++y) {
    const int row = y * gw;
    /// Start at x=1 and end at gw-1 to safely access neighbors
    for (int x = 1; x < gw - 1; ++x) {
      const int idx = row + x;
      if (grid[idx] >= vec_need) {
        /// Check 4-connected neighbors (using bitwise OR for speed)
        bool has_neighbor =
            (grid[idx - 1] >= vec_need) | (grid[idx + 1] >= vec_need) |
            (grid[idx - gw] >= vec_need) | (grid[idx + gw] >= vec_need);
        if (has_neighbor) {
          if (++clusters >= clust_need)
            return true;
        }
      }
    }
  }
  return false;
}

std::vector<double> MotionScanner::scan_range(double start, double end,
                                              long &seek_us, long &decode_us,
                                              long &analyze_us) {
  std::vector<double> ts;
  ts.reserve(4'91'520); // Pre-allocate for typical chunk

  /// OPTIMIZATION: Pre-compute time_base once (avoid per-frame calculation)
  const double time_base =
      av_q2d(fmt_ctx->streams[video_stream_idx]->time_base);

  /// OPTIMIZATION: Calculate frame skip interval based on TARGET_FPS
  /// If TARGET_FPS is 0 or >= video FPS, analyze all frames
  const double video_fps = get_fps();
  const double target = Config::target_fps();
  const int frame_skip = (target > 0 && target < video_fps)
                             ? static_cast<int>(video_fps / target)
                             : 1;
  int frame_count = 0;

  // **----- SEEK TIMING -----**

  auto seek_start = std::chrono::high_resolution_clock::now();

  /// Seek to start position if not at beginning
  if (start > 0) {
    int64_t seek_ts = static_cast<int64_t>(start / time_base);
    av_seek_frame(fmt_ctx, video_stream_idx, seek_ts, AVSEEK_FLAG_BACKWARD);
    avcodec_flush_buffers(dec_ctx);
  }

  auto seek_end = std::chrono::high_resolution_clock::now();
  seek_us += std::chrono::duration_cast<std::chrono::microseconds>(seek_end -
                                                                   seek_start)
                 .count();

  /// **----- DECODE + ANALYZE LOOP -----**

  while (av_read_frame(fmt_ctx, pkt) >= 0) {
    if (pkt->stream_index == video_stream_idx) {
      /// Decode timing
      auto dec_start = std::chrono::high_resolution_clock::now();
      int send_ret = avcodec_send_packet(dec_ctx, pkt);
      auto dec_end = std::chrono::high_resolution_clock::now();
      decode_us += std::chrono::duration_cast<std::chrono::microseconds>(
                       dec_end - dec_start)
                       .count();

      if (send_ret >= 0) {
        while (true) {
          dec_start = std::chrono::high_resolution_clock::now();
          int recv_ret = avcodec_receive_frame(dec_ctx, frame);
          dec_end = std::chrono::high_resolution_clock::now();
          decode_us += std::chrono::duration_cast<std::chrono::microseconds>(
                           dec_end - dec_start)
                           .count();

          if (recv_ret < 0)
            break;

          /// OPTIMIZATION: Skip frames based on TARGET_FPS
          if (++frame_count % frame_skip != 0)
            continue;

          /// Calculate presentation timestamp (using pre-computed time_base)
          double pts = frame->pts * time_base;

          /// Skip frames before our range
          if (pts < start)
            continue;

          /// Stop if we've passed our range
          if (pts >= end) {
            av_packet_unref(pkt);
            return ts;
          }

          // **----- ANALYZE TIMING -----**

          auto analyze_start = std::chrono::high_resolution_clock::now();
          bool has_motion = check_frame(frame);
          auto analyze_end = std::chrono::high_resolution_clock::now();
          analyze_us += std::chrono::duration_cast<std::chrono::microseconds>(
                            analyze_end - analyze_start)
                            .count();

          if (has_motion)
            ts.push_back(pts);
        }
      }
    }
    av_packet_unref(pkt);
  }

  return ts;
}

} // namespace motion_trim
