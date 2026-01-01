/**
 * @file motion_vector_DVR_trimmer.cc
 * @brief Motion Vector DVR Optimization
 *
 * @attention
 * `ARCHITECTURE`:
 *
 *  This application analyzes motion vectors in H.264/HEVC video streams to
 *  identify segments with significant motion. It then uses FFmpeg to cut
 *  out the "dead" segments, saving storage and playback time.
 *
 * `OPTIMIZATIONS`:
 *
 * 1. [DYNAMIC]   Work-stealing queue for dynamic load balancing
 *
 * 2. [CACHE]     Cache-aligned structures for hot data (64-byte alignment)
 *
 * 3. [ALLOC]     Pre-allocated buffers, minimized malloc/realloc calls
 *
 * 4. [MOVE]      Move semantics for result aggregation
 *
 * 5. [IO]        256KB AVIO buffer for reduced syscalls
 *
 * 6. [THREADS]   Uses all hardware threads (no conservative cap)
 *
 * `ROBUSTNESS`:
 *
 * - OOM handling with try-catch around large allocations
 *
 * - Bounds checking for motion vectors (can be negative or OOB)
 *
 * - Proper FFmpeg resource cleanup in all failure paths
 *
 * - AVFMT_FLAG_CUSTOM_IO for correct memory management
 */

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/motion_vector.h>
}

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

/// Linux syscall headers for memfd_create
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

/// fmt library for high-performance formatted output
#include <fmt/color.h>
#include <fmt/core.h>

// **----- CONSTANTS -----**

/**
 * @brief Size of the I/O buffer used by FFmpeg for reading.
 * @note Larger buffer = fewer context switches between memory reader and
 *       decoder. 256KB provides 4x fewer switches than 64KB and going beyond
 *       it proved to be counter-efficient.
 */
constexpr size_t AVIO_BUFFER_SIZE = 256 * 1024; //< 256KB

/**
 * @brief CPU cache line size for alignment.
 * @note Most modern CPUs use 64-byte cache lines.
 *       Aligning hot data to cache lines prevents false sharing in
 *       multi-threaded code.
 */
constexpr size_t CACHE_LINE_SIZE = 64;

// **----- LOGGING MACROS -----**

/**
 * @brief Logging is controlled by ENABLE_LOGGING at compile time.
 * @note All logs use fmt::print for type-safe formatting and are flushed
 *       immediately to ensure visibility in Docker container logs.
 */
#define ENABLE_LOGGING 1
#define ENABLE_TIMING 1

static std::mutex log_mutex; //< Protects concurrent access to stdout

#if ENABLE_LOGGING
#define LOG_INFO(format_str, ...)                                              \
  do {                                                                         \
    std::lock_guard<std::mutex> lock(log_mutex);                               \
    fmt::print("[INFO] " format_str "\n", ##__VA_ARGS__);                      \
    std::fflush(stdout);                                                       \
  } while (0)

#define LOG_WARN(format_str, ...)                                              \
  do {                                                                         \
    std::lock_guard<std::mutex> lock(log_mutex);                               \
    fmt::print(fg(fmt::color::yellow), "[WARN] " format_str "\n",              \
               ##__VA_ARGS__);                                                 \
    std::fflush(stdout);                                                       \
  } while (0)

#define LOG_ERROR(format_str, ...)                                             \
  do {                                                                         \
    std::lock_guard<std::mutex> lock(log_mutex);                               \
    fmt::print(fg(fmt::color::red), "[ERROR] " format_str "\n",                \
               ##__VA_ARGS__);                                                 \
    std::fflush(stdout);                                                       \
  } while (0)

#define LOG_PHASE(format_str, ...)                                             \
  do {                                                                         \
    std::lock_guard<std::mutex> lock(log_mutex);                               \
    fmt::print(fg(fmt::color::cyan), format_str "\n", ##__VA_ARGS__);          \
    std::fflush(stdout);                                                       \
  } while (0)

#define LOG_SUCCESS(format_str, ...)                                           \
  do {                                                                         \
    std::lock_guard<std::mutex> lock(log_mutex);                               \
    fmt::print(fg(fmt::color::green), format_str "\n", ##__VA_ARGS__);         \
    std::fflush(stdout);                                                       \
  } while (0)
#else
#define LOG_INFO(...) ((void)0)
#define LOG_WARN(...) ((void)0)
#define LOG_ERROR(...) ((void)0)
#define LOG_PHASE(...) ((void)0)
#define LOG_SUCCESS(...) ((void)0)
#endif

// **----- TIMING COLLECTION -----**

/**
 * @brief TimingEntry: A single timing measurement.
 * @note Stores the function name and duration in microseconds.
 */
struct TimingEntry {
  std::string name;  //< Function or phase name
  long microseconds; //< Duration in microseconds
};

/**
 * @class TimingCollector
 * @brief Thread-safe singleton for collecting timing measurements.
 * @note All worker threads can safely record their timings here.
 */
class TimingCollector {
  static std::mutex timing_mutex;
  static std::vector<TimingEntry> entries;

public:
  /**
   * @brief Record a timing measurement.
   * @param name Function or phase name
   * @param us Duration in microseconds
   */
  static void record(const std::string &name, long us) {
    std::lock_guard<std::mutex> lock(timing_mutex);
    entries.push_back({name, us});
  }

  /**
   * @brief Print all collected timings as a formatted table.
   *        Called at program end for summary.
   */
  static void print_summary() {
    std::lock_guard<std::mutex> lock(timing_mutex);
    if (entries.empty())
      return;

    fmt::print("\n");
    fmt::print(fg(fmt::color::cyan),
               "================== TIMING SUMMARY ==================\n");
    fmt::print("{:<30} {:>20}\n", "Function", "Time (us) [sec]");
    fmt::print("{:-<30} {:-<20}\n", "", "");

    for (const auto &e : entries) {
      double seconds = e.microseconds / 1000000.0;
      fmt::print("{:<30} {:>10} [{:.2f}s]\n", e.name, e.microseconds, seconds);
    }
    fmt::print(fg(fmt::color::cyan),
               "====================================================\n");
    std::fflush(stdout);
  }

  /**
   * @brief Clear all collected timings.
   * @note Called between files in batch mode.
   */
  static void clear() {
    std::lock_guard<std::mutex> lock(timing_mutex);
    entries.clear();
  }
};

std::mutex TimingCollector::timing_mutex;
std::vector<TimingEntry> TimingCollector::entries;

#if ENABLE_TIMING
#define TIMER_START(name)                                                      \
  auto timer_start_##name = std::chrono::high_resolution_clock::now()

#define TIMER_END(name)                                                        \
  do {                                                                         \
    auto timer_end_##name = std::chrono::high_resolution_clock::now();         \
    auto timer_duration_##name =                                               \
        std::chrono::duration_cast<std::chrono::microseconds>(                 \
            timer_end_##name - timer_start_##name)                             \
            .count();                                                          \
    TimingCollector::record(#name, timer_duration_##name);                     \
  } while (0)
#else
#define TIMER_START(name) ((void)0)
#define TIMER_END(name) ((void)0)
#endif

// **----- CONFIGURATION (Environment Variables) -----**

/**
 * @brief Config namespace: All configurable parameters loaded from environment.
 * @note Uses static variables with lazy initialization for thread-safe caching.
 *       See config/motion_cut.env for detailed documentation of each parameter.
 */
namespace Config {

inline double get_env_double(const char *name, double default_val) {
  const char *val = std::getenv(name);
  return val ? std::stod(val) : default_val;
}

inline int get_env_int(const char *name, int default_val) {
  const char *val = std::getenv(name);
  return val ? std::stoi(val) : default_val;
}

inline float get_env_float(const char *name, float default_val) {
  const char *val = std::getenv(name);
  return val ? std::stof(val) : default_val;
}

/// Motion vector magnitude threshold (squared to avoid sqrt in hot loop)
inline double mv_threshold_sq() {
  static double val = get_env_double("MV_THRESHOLD_SQ", 16.0);
  return val;
}

/// Macroblock size (16 for H.264/HEVC)
inline int block_size() {
  static int val = get_env_int("BLOCK_SIZE", 16);
  return val;
}

/// Bit shift for fast division by block_size (16 = 2^4)
inline int block_shift() {
  static int val = get_env_int("BLOCK_SHIFT", 4);
  return val;
}

/// Minimum motion vectors per block to mark as "active"
inline uint8_t vectors_needed() {
  static uint8_t val = static_cast<uint8_t>(get_env_int("VECTORS_NEEDED", 2));
  return val;
}

/// Minimum clusters of active blocks to trigger motion detection
inline int clusters_needed() {
  static int val = get_env_int("CLUSTERS_NEEDED", 2);
  return val;
}

/// Fraction of frame height to ignore at top/bottom (for timestamps)
inline float vertical_mask() {
  static float val = get_env_float("VERTICAL_MASK", 0.05f);
  return val;
}

/// Maximum gap between motion events before cutting
inline double max_gap_sec() {
  static double val = get_env_double("MAX_GAP_SEC", 5.0);
  return val;
}

/// Padding to add before/after motion segments
inline double padding_sec() {
  static double val = get_env_double("PADDING_SEC", 0.5);
  return val;
}

/// Chunk duration for dynamic load balancing (seconds per work unit)
inline double chunk_duration_sec() {
  static double val = get_env_double("CHUNK_DURATION_SEC", 30.0);
  return val;
}

/// @brief Target analysis FPS for frame skipping (0 = analyze all frames)
/// @note Lower values = faster processing but may miss brief motion
inline double target_fps() {
  static double val = get_env_double("TARGET_FPS", 0.0);
  return val;
}

/// @brief Minimum savings percentage to trigger cutting
/// @note If savings are below this threshold, the cut is skipped
inline double min_savings_pct() {
  static double val = get_env_double("MIN_SAVINGS_PCT", 5.0);
  return val;
}

} // namespace Config

// **---- CGROUP-AWARE CPU DETECTION ----**

/**
 * @brief Detect the actual number of CPUs available to this process.
 *
 * @note In Docker containers, std::thread::hardware_concurrency() returns the
 *       HOST's total cores, not the container's cgroup limit. This function
 *       reads cgroup files to detect the actual limit.
 *
 *       Supports:
 *
 *        - Cgroup v1: `/sys/fs/cgroup/cpu/cpu.cfs_quota_us` and
 *          `cpu.cfs_period_us`
 *
 *        - Cgroup v2: `/sys/fs/cgroup/cpu.max`
 *
 *        - Cpuset: `/sys/fs/cgroup/cpuset/cpuset.cpus` (counts allowed cores)
 *
 * @return Detected CPU limit, or hardware_concurrency() as fallback
 */
int detect_cpu_limit() {
  /// Helper to read a number from a file
  auto read_long = [](const char *path) -> long {
    std::ifstream f(path);
    if (!f)
      return -1;
    long val;
    f >> val;
    return f.good() ? val : -1;
  };

  /// Helper to count CPUs from cpuset string like "0,2,4,6,8" or "0-3"
  auto count_cpuset = [](const char *path) -> int {
    std::ifstream f(path);
    if (!f)
      return -1;
    std::string line;
    std::getline(f, line);
    if (line.empty())
      return -1;

    int count = 0;
    size_t pos = 0;
    while (pos < line.size()) {
      /// Find next number
      size_t end = line.find_first_of(",-", pos);
      if (end == std::string::npos)
        end = line.size();

      int start_cpu = std::stoi(line.substr(pos, end - pos));

      if (end < line.size() && line[end] == '-') {
        /// Range like "0-3"
        pos = end + 1;
        end = line.find(',', pos);
        if (end == std::string::npos)
          end = line.size();
        int end_cpu = std::stoi(line.substr(pos, end - pos));
        count += (end_cpu - start_cpu + 1);
      } else {
        /// Single CPU
        count++;
      }

      pos = (end < line.size()) ? end + 1 : line.size();
    }
    return count;
  };

  int limit = -1;

  /// Try cgroup v2 first (unified hierarchy)
  {
    std::ifstream f("/sys/fs/cgroup/cpu.max");
    if (f) {
      std::string quota_str, period_str;
      f >> quota_str >> period_str;
      if (quota_str != "max" && !period_str.empty()) {
        long quota = std::stol(quota_str);
        long period = std::stol(period_str);
        if (quota > 0 && period > 0) {
          /// Use ceiling to round up fractional CPUs (2.5 -> 3)
          limit = static_cast<int>((quota + period - 1) / period);
        }
      }
    }
  }

  /// Try cgroup v1 CPU quota
  if (limit <= 0) {
    long quota = read_long("/sys/fs/cgroup/cpu/cpu.cfs_quota_us");
    long period = read_long("/sys/fs/cgroup/cpu/cpu.cfs_period_us");
    if (quota > 0 && period > 0) {
      /// Use ceiling to round up fractional CPUs (2.5 -> 3)
      limit = static_cast<int>((quota + period - 1) / period);
    }
  }

  /// Try cpuset (counts actual allowed cores)
  if (limit <= 0) {
    // Try cgroup v2 path first
    limit = count_cpuset("/sys/fs/cgroup/cpuset.cpus.effective");
    if (limit <= 0) {
      // Try cgroup v1 path
      limit = count_cpuset("/sys/fs/cgroup/cpuset/cpuset.cpus");
    }
  }

  /// Fallback to hardware_concurrency
  if (limit <= 0) {
    limit = std::thread::hardware_concurrency();
  }

  /// Sanity check
  if (limit <= 0)
    limit = 4;
  if (limit > 64)
    limit = 64; //< Cap at 64 to prevent excessive threading

  /// Use the larger of the two limits
  ///\note If cpuset pins us to 3 specific cores but quota says 2.0 CPUs,
  ///      we prefer 3 threads to utilize L1/L2 caches of all pinned cores,
  ///      even if they are throttled.
  if (limit > 0) {
    int cpuset_count = count_cpuset("/sys/fs/cgroup/cpuset.cpus.effective");
    if (cpuset_count <= 0) {
      cpuset_count = count_cpuset("/sys/fs/cgroup/cpuset/cpuset.cpus");
    }
    if (cpuset_count > limit) {
      limit = cpuset_count;
    }
  }

  return limit;
}

// **---- DATA STRUCTURES ----**

/**
 * @struct TimeSegment
 * @brief Represents a time range [start, end) in seconds.
 * @note Used for both motion segments and cut points.
 *       Aligned to 16 bytes for potential SIMD operations.
 */
struct alignas(16) TimeSegment {
  double start; //< Start time in seconds
  double end;   //< End time in seconds
};

/**
 * @struct ScanTask
 * @brief A work unit for the dynamic task queue.
 * @note Cache-line aligned to prevent false sharing between threads.
 */
struct alignas(CACHE_LINE_SIZE) ScanTask {
  double start; //< Start time in seconds
  double end;   //< End time in seconds
  int id;       //< Chunk ID for debugging
};

/**
 * @class TaskQueue
 * @brief Thread-safe work-stealing queue for dynamic load balancing.
 *
 * @attention DESIGN:
 *
 * - Workers pop tasks from a shared queue
 *
 * - If one worker gets a "hard" chunk (complex scene), others continue
 *
 * - All CPU cores stay busy until all work is done
 *
 * @note This solves the "thread starvation" problem where static partitioning
 *       leaves some threads idle while others process complex segments.
 */
class TaskQueue {
  std::queue<ScanTask> tasks;
  std::mutex mutex;
  std::condition_variable cv;
  std::atomic<bool> done{false};

public:
  /**
   * @brief Add a task to the queue.
   * @note Thread-safe; notifies one waiting worker.
   */
  void push(ScanTask task) {
    std::lock_guard<std::mutex> lock(mutex);
    tasks.push(task);
    cv.notify_one();
  }

  /**
   * @brief Pop a task from the queue.
   * @note Blocks until a task is available or queue is finished.
   * @param task Output parameter for the task
   * @return true if a task was retrieved, false if queue is empty and done
   */
  bool pop(ScanTask &task) {
    std::unique_lock<std::mutex> lock(mutex);
    cv.wait(lock, [this] { return !tasks.empty() || done.load(); });
    if (tasks.empty())
      return false;
    task = tasks.front();
    tasks.pop();
    return true;
  }

  /**
   * @brief Signal that no more tasks will be added.
   * @note Wakes all waiting workers so they can exit.
   */
  void finish() {
    done.store(true);
    cv.notify_all();
  }
};

/**
 * @class ResultCollector
 * @brief Thread-safe aggregator for scan results.
 * @note Uses move semantics to minimize copying of large vectors.
 */
class ResultCollector {
  std::vector<double> timestamps;
  std::mutex mutex;

public:
  /**
   * @brief Pre-allocate space for expected results.
   * @attention Reduces reallocation during collection.
   */
  void reserve(size_t n) {
    std::lock_guard<std::mutex> lock(mutex);
    timestamps.reserve(n);
  }

  /**
   * @brief Add results from a worker thread.
   * @attention Uses move semantics to avoid copying the vector.
   */
  void add(std::vector<double> &&results) {
    std::lock_guard<std::mutex> lock(mutex);
    timestamps.insert(timestamps.end(),
                      std::make_move_iterator(results.begin()),
                      std::make_move_iterator(results.end()));
  }

  /**
   * @brief Extract all collected results.
   * @attention Moves the internal vector out, leaving collector empty.
   */
  std::vector<double> extract() {
    std::lock_guard<std::mutex> lock(mutex);
    return std::move(timestamps);
  }
};

// **---- UTILITIES ----**

/**
 * @brief Format seconds as HH:MM:SS string.
 */
std::string format_time(double seconds) {
  int h = static_cast<int>(seconds) / 3600;
  int m = (static_cast<int>(seconds) % 3600) / 60;
  int s = static_cast<int>(seconds) % 60;
  return fmt::format("{:02d}:{:02d}:{:02d}", h, m, s);
}

// **---- MEMORY I/O ----**

/**
 * @brief MemReaderState: State for custom FFmpeg I/O from memory buffer.
 * @note Allows FFmpeg to read from RAM instead of disk.
 *       Cache-line aligned since this is accessed heavily in the I/O callback.
 */
struct alignas(CACHE_LINE_SIZE) MemReaderState {
  const uint8_t *ptr; //< Pointer to buffer start
  size_t size;        //< Total buffer size
  size_t pos;         //< Current read position
};

/**
 * @class MemoryLoader
 * @brief Handles loading video files into RAM and providing
 *       custom I/O callbacks for FFmpeg.
 *
 * @attention ROBUSTNESS:
 *
 * - Handles OOM gracefully with try-catch
 *
 * - Validates file size before allocation
 *
 * - Logs meaningful error messages
 */
class MemoryLoader {
public:
  /**
   * @brief Load an entire file into a memory buffer.
   * @param path Path to the file
   * @param buffer Output buffer (will be resized)
   * @return true on success, false on failure
   */
  static bool load_file(const std::string &path, std::vector<uint8_t> &buffer) {
    TIMER_START(load_file);

    // Open file and get size
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
      LOG_ERROR("Failed to open file: {}", path);
      return false;
    }

    std::streamsize size = file.tellg();

    /// Validate file size
    if (size <= 0) {
      LOG_ERROR("File is empty or invalid: {}", path);
      return false;
    }

    file.seekg(0, std::ios::beg);

    /// CRITICAL: Catch allocation failures
    ///\note This prevents crashes when file is larger than available RAM
    try {
      buffer.resize(static_cast<size_t>(size));
    } catch (const std::bad_alloc &e) {
      LOG_ERROR("Not enough RAM to load {} MB file! ({})", size / 1024 / 1024,
                e.what());
      return false;
    }

    /// Read file into buffer
    bool result = static_cast<bool>(
        file.read(reinterpret_cast<char *>(buffer.data()), size));

    if (!result) {
      LOG_ERROR("Failed to read file contents");
      return false;
    }

    TIMER_END(load_file);
    return true;
  }

  /**
   * @brief FFmpeg read callback for custom I/O.
   * @note Reads from memory buffer instead of file.
   */
  static int read(void *opaque, uint8_t *buf, int buf_size) {
    MemReaderState *bd = static_cast<MemReaderState *>(opaque);
    size_t bytes_left = bd->size - bd->pos;
    if (bytes_left == 0)
      return AVERROR_EOF;
    size_t copy = std::min(bytes_left, static_cast<size_t>(buf_size));
    memcpy(buf, bd->ptr + bd->pos, copy);
    bd->pos += copy;
    return static_cast<int>(copy);
  }

  /**
   * @brief FFmpeg seek callback for custom I/O.
   * @note Handles all seek modes including AVSEEK_SIZE.
   */
  static int64_t seek(void *opaque, int64_t offset, int whence) {
    MemReaderState *bd = static_cast<MemReaderState *>(opaque);

    if (whence == AVSEEK_SIZE)
      return static_cast<int64_t>(bd->size);

    int64_t new_pos = static_cast<int64_t>(bd->pos);
    if (whence == SEEK_SET)
      new_pos = offset;
    else if (whence == SEEK_CUR)
      new_pos += offset;
    else if (whence == SEEK_END)
      new_pos = static_cast<int64_t>(bd->size) + offset;

    if (new_pos < 0)
      new_pos = 0;
    if (new_pos > static_cast<int64_t>(bd->size))
      new_pos = static_cast<int64_t>(bd->size);

    bd->pos = static_cast<size_t>(new_pos);
    return new_pos;
  }
};

// **---- MOTION SCANNER ----**

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
  const std::vector<uint8_t> &file_data;

public:
  /**
   * @brief Constructor: Allocate frame and packet structures.
   * @attention The actual decoder setup happens in initialize().
   */
  explicit MotionScanner(const std::vector<uint8_t> &data) : file_data(data) {
    frame = av_frame_alloc();
    pkt = av_packet_alloc();
  }

  /**
   * @brief Destructor: Clean up all FFmpeg resources.
   *
   * @attention Handles the case where initialization failed partway.
   *
   * @note Memory management:
   *
   *      - If fmt_ctx was opened successfully:
   *
   *      - avformat_close_input closes both the format context AND the
   *        attached I/O context, including the buffer allocated with
   *        av_malloc.
   *
   *      - If initialization failed before avformat_open_input:
   *
   *      - We must clean up avio_ctx (which frees its buffer) or avio_buffer
   *        manually.
   */
  ~MotionScanner() {
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

  /**
   * @brief Initialize decoder and prepare for scanning.
   *
   * @return true on success, false on failure
   */
  bool initialize() {
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

    /// CRITCIAL: Set flag to indicate we're using custom I/O
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
    grid_h = static_cast<int16_t>(
        (dec_ctx->height + Config::block_size() - 1) >> cfg.block_shift);

    /// Calculate vertical margin (rows to skip at top/bottom)
    cfg.vertical_margin = static_cast<int>(grid_h * Config::vertical_mask());

    /// Pre-allocate grid to avoid malloc in hot loop
    grid_votes.resize(grid_w * grid_h);

    return true;
  }

  /**
   * @brief Get video duration in seconds.
   */
  double get_duration() {
    return (fmt_ctx->duration != AV_NOPTS_VALUE)
               ? fmt_ctx->duration / static_cast<double>(AV_TIME_BASE)
               : 0.0;
  }

  /**
   * @brief Get video frame rate.
   */
  double get_fps() {
    if (video_stream_idx < 0)
      return 25.0;
    AVRational r = fmt_ctx->streams[video_stream_idx]->avg_frame_rate;
    return (r.den > 0) ? av_q2d(r) : 25.0;
  }

  /**
   * @brief Check if a frame contains significant motion.
   *
   * @attention This is the HOT PATH - called for every decoded frame.
   *
   * `Optimizations`:
   * - All config values pre-cached in local variables
   *
   * - Bounds checking is mandatory (motion vectors can be OOB)
   *
   * - Uses bitwise OR in cluster detection for fewer branches
   *
   * `ALGORITHM`:
   *
   * 1. Extract motion vectors from frame side data
   *
   * 2. Map each significant MV to a grid cell, increment vote
   *
   * 3. Find clusters of adjacent active cells
   *
   * 4. Return true if enough clusters found
   *
   * @param f Decoded frame with motion vector side data
   * @return true if significant motion detected
   */
  bool check_frame(const AVFrame *f) {
    /// Get motion vector side data (may be null for I-frames)
    AVFrameSideData *sd =
        av_frame_get_side_data(f, AV_FRAME_DATA_MOTION_VECTORS);
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

  /**
   * @brief Scan a time range and return timestamps with motion.
   *
   * @attention `OPTIMIZATIONS`:
   *
   * - Pre-computed time_base avoids `av_q2d()` call per frame
   *
   * - `TARGET_FPS`-based frame skipping reduces decode load
   *
   * @param start Start time in seconds
   * @param end End time in seconds
   * @param seek_us Output: accumulated seek time in microseconds
   * @param decode_us Output: accumulated decode time in microseconds
   * @param analyze_us Output: accumulated check_frame time in microseconds
   * @return Vector of timestamps (in seconds) where motion was detected
   */
  std::vector<double> scan_range(double start, double end, long &seek_us,
                                 long &decode_us, long &analyze_us) {
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
        // Decode timing
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
};

// **---- PROCESSING PIPELINE ----**

/**
 * @class ProcessingPipeline
 * @brief Orchestrates the entire video analysis and cutting.
 *
 * @attention `WORKFLOW`:
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

public:
  ProcessingPipeline(std::string in, std::string out)
      : input_path(std::move(in)), output_path(std::move(out)) {}

  int run() {
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

      LOG_INFO("Duration: {} ({:.0f} frames @ {:.1f}fps)",
               format_time(duration), duration * probe.get_fps(),
               probe.get_fps());
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

    // Launch worker threads
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
    double analyze_avg_sec =
        (total_analyze_us.load() / num_threads) / 1000000.0;

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
    TimingCollector::record(fmt::format("  │   ├─decode ({}T×{:.2f}s)",
                                        num_threads, decode_avg_sec),
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

private:
  /**
   * @brief Print summary of what was cut.
   */
  void print_cut_summary() {
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

  /**
   * @brief Execute FFmpeg to produce the cut video.
   *
   * @attention Uses memfd_create to create an in-memory file for the concat
   *            list, avoiding disk I/O entirely.
   */
  void execute_cut(const std::vector<TimeSegment> &segments) {
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
};

// **---- MAIN ----**

int main(int argc, char *argv[]) {
  /// Disable stdout buffering for real-time log visibility
  std::setvbuf(stdout, nullptr, _IONBF, 0);

  if (argc < 3) {
    LOG_WARN("Usage: ./motion_cut <input> <output>");
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
    LOG_INFO("Motion Cut :");
    LOG_INFO("Input: {}", input_arg);
    LOG_INFO("Output: {}", output_arg);

    ProcessingPipeline app(input_arg, output_arg);
    return app.run();
  }
}