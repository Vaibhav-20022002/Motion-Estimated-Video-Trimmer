# include/motion_trim/

This directory contains the public headers for the Motion Trim library.

## Headers

| File | Description |
|------|-------------|
| `types.hpp` | Core data types (TimeSegment, ScanTask) and constants |
| `config.hpp` | Configuration via environment variables |
| `logging.hpp` | Thread-safe logging macros and timing utilities |
| `system.hpp` | System utilities (CPU detection, thread pinning) |
| `task_queue.hpp` | Work-stealing task queue and result collector |
| `memory_io.hpp` | FFmpeg custom I/O from memory buffer |
| `motion_scanner.hpp` | Video decoding and motion vector analysis |
| `pipeline.hpp` | Main processing pipeline orchestration |
| `batch_processor.hpp` | Parallel video stream processing for batch mode |

## Usage

Include the main pipeline header in your code:

```cpp
#include <motion_trim/pipeline.hpp>

int main() {
    motion_trim::ProcessingPipeline app("input.mp4", "output.mp4");
    return app.run();
}
```

For batch processing with parallel streams:

```cpp
#include <motion_trim/batch_processor.hpp>

int main() {
    std::vector<std::string> files = {"video1.mp4", "video2.mp4"};
    motion_trim::BatchProcessor processor(2);  // 2 parallel streams
    return processor.process(files, "/output/dir");
}
```
