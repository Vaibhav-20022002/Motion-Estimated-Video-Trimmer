/**
 * @file extract_mvs.cpp
 * @brief Motion Vector Extraction Utility
 *
 * @details Standalone utility to extract motion vectors from a video file
 *          and output them as JSON format for analysis.
 *
 * @usage
 *   extract_mvs input.mp4 output.json
 *
 * @note Build:
 *   g++ -std=c++14 -O2 extract_mvs.cpp \
 *       `pkg-config --cflags --libs libavformat libavcodec libavutil`
 *
 */

#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <string>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/frame.h>
#include <libavutil/motion_vector.h>
}

/**
 * @brief Convert AVPictureType to string representation.
 * @param t Picture type enum value
 * @return String representation ("I", "P", "B", or "?")
 */
static const char *frame_type_str(int t) {
  switch (t) {
  case AV_PICTURE_TYPE_I:
    return "I";
  case AV_PICTURE_TYPE_P:
    return "P";
  case AV_PICTURE_TYPE_B:
    return "B";
  default:
    return "?";
  }
}

int main(int argc, char **argv) {
  if (argc < 3) {
    fprintf(stderr, "Usage: %s input.mp4 output.json\n", argv[0]);
    return 1;
  }

  const char *input = argv[1];
  const char *output = argv[2];

  FILE *out = fopen(output, "w");
  if (!out) {
    perror("fopen");
    return 1;
  }

  av_log_set_level(AV_LOG_ERROR);

  AVFormatContext *fmt = nullptr;
  if (avformat_open_input(&fmt, input, nullptr, nullptr) < 0 ||
      avformat_find_stream_info(fmt, nullptr) < 0) {
    fprintf(stderr, "Failed to open input\n");
    return 1;
  }

  int vs = -1;
  for (unsigned i = 0; i < fmt->nb_streams; ++i) {
    if (fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
      vs = i;
      break;
    }
  }
  if (vs < 0) {
    fprintf(stderr, "No video stream\n");
    return 1;
  }

  AVStream *st = fmt->streams[vs];
  const AVCodec *dec = avcodec_find_decoder(st->codecpar->codec_id);
  AVCodecContext *ctx = avcodec_alloc_context3(dec);
  avcodec_parameters_to_context(ctx, st->codecpar);

  AVDictionary *opts = nullptr;
  av_dict_set(&opts, "flags2", "+export_mvs", 0);
  avcodec_open2(ctx, dec, &opts);
  av_dict_free(&opts);

  AVPacket *pkt = av_packet_alloc();
  AVFrame *frm = av_frame_alloc();

  // JSON header
  fprintf(out, "{\n");
  fprintf(out, "  \"input\": \"%s\",\n", input);
  fprintf(out, "  \"time_base\": \"%d/%d\",\n", st->time_base.num,
          st->time_base.den);
  fprintf(out, "  \"frames\": [\n");

  bool first_frame = true;
  int64_t frame_idx = 0;

  while (av_read_frame(fmt, pkt) >= 0) {
    if (pkt->stream_index != vs) {
      av_packet_unref(pkt);
      continue;
    }

    if (avcodec_send_packet(ctx, pkt) < 0) {
      av_packet_unref(pkt);
      continue;
    }

    while (avcodec_receive_frame(ctx, frm) == 0) {
      frame_idx++;

      int64_t pts = frm->best_effort_timestamp;
      double pts_sec =
          (pts == AV_NOPTS_VALUE) ? -1.0 : pts * av_q2d(st->time_base);

      AVFrameSideData *sd =
          av_frame_get_side_data(frm, AV_FRAME_DATA_MOTION_VECTORS);
      int mv_count = sd ? sd->size / sizeof(AVMotionVector) : 0;
      const AVMotionVector *mvs =
          sd ? (const AVMotionVector *)sd->data : nullptr;

      if (!first_frame)
        fprintf(out, ",\n");
      first_frame = false;

      fprintf(out, "    {\n");
      fprintf(out, "      \"frame_index\": %" PRId64 ",\n", frame_idx);
      if (pts_sec >= 0)
        fprintf(out, "      \"pts_seconds\": %.6f,\n", pts_sec);
      else
        fprintf(out, "      \"pts_seconds\": null,\n");

      fprintf(out, "      \"frame_type\": \"%s\",\n",
              frame_type_str(frm->pict_type));
      fprintf(out, "      \"num_mvs\": %d,\n", mv_count);
      fprintf(out, "      \"motion_vectors\": [");

      for (int i = 0; i < mv_count; ++i) {
        const AVMotionVector &mv = mvs[i];
        double src_x = mv.dst_x + (double)mv.motion_x /
                                      (mv.motion_scale ? mv.motion_scale : 1);
        double src_y = mv.dst_y + (double)mv.motion_y /
                                      (mv.motion_scale ? mv.motion_scale : 1);

        if (i)
          fprintf(out, ",");
        fprintf(out,
                "\n        {"
                "\"dst_x\":%d,\"dst_y\":%d,"
                "\"src_x\":%.3f,\"src_y\":%.3f,"
                "\"w\":%d,\"h\":%d,"
                "\"motion_x\":%d,\"motion_y\":%d,"
                "\"motion_scale\":%d,"
                "\"source\":%d}",
                mv.dst_x, mv.dst_y, src_x, src_y, mv.w, mv.h, mv.motion_x,
                mv.motion_y, mv.motion_scale, mv.source);
      }

      if (mv_count)
        fprintf(out, "\n      ");
      fprintf(out, "]\n    }");

      av_frame_unref(frm);
    }
    av_packet_unref(pkt);
  }

  fprintf(out, "\n  ]\n}\n");

  fclose(out);
  av_frame_free(&frm);
  av_packet_free(&pkt);
  avcodec_free_context(&ctx);
  avformat_close_input(&fmt);

  return 0;
}
