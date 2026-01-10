/**
 * @file cacheLineSize.cpp
 * @brief Utility to detect CPU cache line size at runtime/compile time.
 *
 * @details This tool is used by CMake to detect the cache line size of the
 *          host system. It employs multiple strategies:
 *
 *          1. sysctl (macOS)
 *
 *          2. sysfs (Linux)
 *
 *          3. GetLogicalProcessorInformation (Windows)
 *
 *          4. CPUID (x86/x86_64)
 *
 *          5. Compile-time builtin fallback
 */

#include <cstddef>
#include <iostream>

/// macOS: sysctlbyname for cache-line size
#if defined(__APPLE__)
#include <sys/sysctl.h>
#endif

/// Linux: reading from sysfs
#if defined(__linux__)
#include <fstream>
#endif

/// Windows: GetLogicalProcessorInformation
#if defined(_MSC_VER)
#include <vector>
#include <windows.h>
#endif

// **----- FALLBACKS -----**

/**
 * @brief Compile-time fallback for cache line size.
 * @note Uses __builtin_hardware_destructive_interference_size if available,
 *       otherwise defaults to 64 bytes.
 */
constexpr size_t compile_cacheline =
#if defined(__has_builtin) &&                                                  \
    __has_builtin(__builtin_hardware_destructive_interference_size)
    __builtin_hardware_destructive_interference_size();
#else
    64;
#endif

// **----- DETECTION STRATEGIES -----**

#if defined(__x86_64__) || defined(__i386__)
#include <cpuid.h>
/**
 * @brief Detect cache line size using x86 CPUID.
 * @return Cache line size in bytes, or 0 if detection fails.
 */
size_t detect_x86() {
  unsigned eax, ebx, ecx, edx;
  __cpuid(1, eax, ebx, ecx, edx);
  return ((ebx >> 8) & 0xFF) * 8;
}
#else
constexpr size_t detect_x86() { return 0; }
#endif

#if defined(__linux__)
/**
 * @brief Detect cache line size using Linux sysfs.
 * @return Cache line size in bytes, or 0 if detection fails.
 */
size_t detect_linux_sysfs() {
  std::ifstream f(
      "/sys/devices/system/cpu/cpu0/cache/index0/coherency_line_size");
  size_t v = 0;
  if (f >> v)
    return v; //< bytes
  return 0;
}
#endif

/**
 * @brief Get the cache line size using the best available method.
 * @return Detected cache line size in bytes.
 */
size_t get_cacheline_runtime() {
  size_t cls = 0;

  /// macOS (Intel & Apple Silicon)
#if defined(__APPLE__)
  size_t len = sizeof(cls);
  if (sysctlbyname("hw.cachelinesize", &cls, &len, nullptr, 0) == 0)
    return cls;
#endif

    /// Linux fallback: sysfs
#if defined(__linux__)
  cls = detect_linux_sysfs();
  if (cls > 0)
    return cls;
#endif

    /// Windows: WinAPI Cache info
#if defined(_MSC_VER)
  DWORD needed = 0;
  GetLogicalProcessorInformation(nullptr, &needed);
  std::vector<char> buf(needed);
  if (GetLogicalProcessorInformation(
          reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION *>(buf.data()),
          &needed)) {
    auto *info =
        reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION *>(buf.data());
    size_t count = needed / sizeof(*info);
    for (size_t i = 0; i < count; ++i) {
      if (info[i].Relationship == RelationCache && info[i].Cache.Level == 1)
        return info[i].Cache.LineSize;
    }
  }
#endif

  /// x86 CPUID fallback
  if (size_t x = detect_x86())
    return x;

  /// Final fallback: compile-time guess
  return compile_cacheline;
}

// **---- MAIN ----**

int main() {
  std::cout << get_cacheline_runtime();
  return 0;
}
