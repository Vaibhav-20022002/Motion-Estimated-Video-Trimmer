# include/motion_trim/

This directory contains the public headers for the Motion Cut library.

## Headers

| File | Description |
|------|-------------|
| `types.hpp` | Core data types (TimeSegment, ScanTask) and constants |
| `config.hpp` | Configuration via environment variables |
| `logging.hpp` | Thread-safe logging macros and timing utilities |
| `system.hpp` | System utilities (CPU detection, time formatting) |
| `task_queue.hpp` | Work-stealing task queue and result collector |
| `memory_io.hpp` | FFmpeg custom I/O from memory buffer |
| `motion_scanner.hpp` | Video decoding and motion vector analysis |
| `pipeline.hpp` | Main processing pipeline orchestration |

## Usage

Include the main pipeline header in your code:

```cpp
#include <motion_trim/pipeline.hpp>

int main() {
    motion_trim::ProcessingPipeline app("input.mp4", "output.mp4");
    return app.run();
}
```
