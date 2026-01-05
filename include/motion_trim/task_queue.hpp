/**
 * @file task_queue.hpp
 * @brief Thread-safe task queue and result collection
 *
 * @details Provides:
 *          - TaskQueue: Work-stealing queue for dynamic load balancing
 *
 *          - ResultCollector: Thread-safe aggregator for scan results
 */

#ifndef MOTION_TRIM_TASK_QUEUE_HPP
#define MOTION_TRIM_TASK_QUEUE_HPP

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <queue>
#include <vector>

#include "types.hpp"

namespace motion_trim {

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
  void push(ScanTask task);

  /**
   * @brief Pop a task from the queue.
   * @note Blocks until a task is available or queue is finished.
   * @param task Output parameter for the task
   * @return true if a task was retrieved, false if queue is empty and done
   */
  bool pop(ScanTask &task);

  /**
   * @brief Signal that no more tasks will be added.
   * @note Wakes all waiting workers so they can exit.
   */
  void finish();
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
  void reserve(size_t n);

  /**
   * @brief Add results from a worker thread.
   * @attention Uses move semantics to avoid copying the vector.
   */
  void add(std::vector<double> &&results);

  /**
   * @brief Extract all collected results.
   * @attention Moves the internal vector out, leaving collector empty.
   */
  std::vector<double> extract();
};

} // namespace motion_trim

#endif // MOTION_TRIM_TASK_QUEUE_HPP
