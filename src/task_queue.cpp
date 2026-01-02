/**
 * @file task_queue.cpp
 * @brief Thread-safe task queue and result collection implementation
 *
 * @details Provides implementations for:
 * 
 *          - TaskQueue: Work-stealing queue for dynamic load balancing
 * 
 *          - ResultCollector: Thread-safe aggregator for scan results
 */

#include "motion_trim/task_queue.hpp"

#include <iterator>

namespace motion_trim {

// **----- TaskQueue Implementation -----**

void TaskQueue::push(ScanTask task) {
  std::lock_guard<std::mutex> lock(mutex);
  tasks.push(task);
  cv.notify_one();
}

bool TaskQueue::pop(ScanTask &task) {
  std::unique_lock<std::mutex> lock(mutex);
  cv.wait(lock, [this] { return !tasks.empty() || done.load(); });
  if (tasks.empty())
    return false;
  task = tasks.front();
  tasks.pop();
  return true;
}

void TaskQueue::finish() {
  done.store(true);
  cv.notify_all();
}

// **----- ResultCollector Implementation -----**

void ResultCollector::reserve(size_t n) {
  std::lock_guard<std::mutex> lock(mutex);
  timestamps.reserve(n);
}

void ResultCollector::add(std::vector<double> &&results) {
  std::lock_guard<std::mutex> lock(mutex);
  timestamps.insert(timestamps.end(), std::make_move_iterator(results.begin()),
                    std::make_move_iterator(results.end()));
}

std::vector<double> ResultCollector::extract() {
  std::lock_guard<std::mutex> lock(mutex);
  return std::move(timestamps);
}

} // namespace motion_trim
