# src/

This directory contains the source implementations for the Motion Cut library.

## Source Files

| File | Description |
|------|-------------|
| `logging.cpp` | TimingCollector implementation and static mutex |
| `system.cpp` | Cgroup-aware CPU detection for Docker |
| `task_queue.cpp` | TaskQueue and ResultCollector implementations |
| `memory_io.cpp` | File loading and FFmpeg custom I/O callbacks |
| `motion_scanner.cpp` | MotionScanner: video decoding and motion detection |
| `pipeline.cpp` | ProcessingPipeline: full workflow orchestration |
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
