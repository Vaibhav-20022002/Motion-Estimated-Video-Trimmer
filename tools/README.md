# tools/

This directory contains standalone utility tools for motion vector analysis.

## Tools

### extract_mvs

Extracts motion vectors from a video file and outputs them as JSON.

```bash
./extract_mvs input.mp4 output.json
```

### motion_scalar

Computes motion scalar values from an extracted motion vector JSON file.

```bash
./motion_scalar motion_vectors.json
```

## Building

Enable tool building with CMake:

```bash
cmake -B build -DBUILD_TOOLS=ON
cmake --build build
```

This will produce `extract_mvs` and `motion_scalar` executables in the build directory which can be execute using above commnads.
