# Motion-Estimated Video Trimmer

A high-performance video processing tool that automatically removes static footage from surveillance/CCTV videos by analyzing motion vectors.

## Features

- **Motion Vector Analysis** - Uses H.264/HEVC motion vectors to detect activity (no pixel-level processing)
- **Parallel Batch Processing** - Process multiple videos simultaneously with CPU isolation
- **Producer-Consumer Architecture** - Parallel scanning with sequential FFmpeg writes
- **Docker Deployment** - Easy containerized deployment with environment configuration

## Quick Start

```bash
# Build the Docker image
sudo docker compose build

# Process a single video
docker compose run --rm motion-trim /input/video.mp4 /output/video.mp4

# Process a directory of videos
docker compose run --rm motion-trim /input /output
```

## Configuration

All settings are in `config/motion_trim.env`:

| Variable | Default | Description |
|----------|---------|-------------|
| `PARALLEL_STREAMS` | 0 (auto) | Number of concurrent video processing streams |
| `THREADS_PER_STREAM` | 0 (auto) | Worker threads and CPUs per stream |
| `MV_THRESHOLD` | 5 | Minimum motion magnitude to count |
| `CLUSTERS_NEEDED` | 4 | Adjacent active blocks to trigger motion |
| `MAX_GAP_SEC` | 5 | Max seconds between motion to keep |
| `MIN_SAVINGS_PCT` | 10 | Minimum savings % to actually cut video |

## Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                     BATCH PROCESSING                        │
├─────────────────────────────────────────────────────────────┤
│                                                             │
│  ┌─────────────┐   ┌─────────────┐   ┌─────────────┐        │
│  │  Stream 0   │   │  Stream 1   │   │  Stream 2   │        │
│  │  CPUs 0,1   │   │  CPUs 2,3   │   │  CPUs 4,5   │        │
│  └──────┬──────┘   └──────┬──────┘   └──────┬──────┘        │
│         │                 │                 │               │
│         └─────────────────┼─────────────────┘               │
│                           ↓                                 │
│                   ┌───────────────┐                         │
│                   │  FFmpeg Queue │                         │
│                   └───────┬───────┘                         │
│                           ↓                                 │
│                   ┌───────────────┐                         │
│                   │ FFmpeg Worker │                         │    
│                   │   Sequential  │                         │
│                   └───────────────┘                         │
│                                                             │
└─────────────────────────────────────────────────────────────┘
```

## Performance

With 8 CPUs and 3 parallel streams:
- **Wall-clock speedup**: ~2.75x faster than sequential
- **CPU utilization**: Near 100% across all cores

## Technical Details

### Motion Detection Algorithm
1. Decode video with FFmpeg (skip visual processing)
2. Extract motion vectors from H.264/HEVC side data
3. Map vectors to spatial grid (16x16 blocks)
4. Detect clusters of adjacent active blocks
5. Build time segments with motion

### Thread Model
- Each stream has dedicated CPU cores
- Worker threads pinned to stream's CPU set
- FFmpeg processes also pinned via `taskset`


## License

MIT License
