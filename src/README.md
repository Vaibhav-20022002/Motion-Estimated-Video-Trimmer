# src/

This directory contains the source implementations for the Motion Trim library.

## Source Files

| File | Description |
|------|-------------|
| `logging.cpp` | TimingCollector implementation and static mutex |
| `system.cpp` | Cgroup-aware CPU detection, thread pinning |
| `task_queue.cpp` | TaskQueue and ResultCollector implementations |
| `memory_io.cpp` | File loading and FFmpeg custom I/O callbacks |
| `motion_scanner.cpp` | MotionScanner: video decoding and motion detection |
| `pipeline.cpp` | ProcessingPipeline: full workflow orchestration |
| `batch_processor.cpp` | BatchProcessor: parallel video stream processing |
| `main.cpp` | Application entry point |

## Build

Build using CMake:

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

Or use Docker:

```bash
docker-compose build
```
