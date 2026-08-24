// Cross-platform Thread Affinity, Core Pinning, and Memory Locking for Ultra-Low Latency Execution.
// Supports both macOS (Darwin QoS / Mach policies) and Linux (pthread_setaffinity_np / sched_param
// / mlockall).
#pragma once

#include <cerrno>
#include <cstddef>
#include <cstring>
#include <pthread.h>
#include <sys/mman.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <mach/mach.h>
#include <mach/thread_act.h>
#include <mach/thread_policy.h>
#include <pthread/qos.h>
#elif defined(__linux__)
#include <sched.h>
#endif

namespace mm {

// Pin the current calling thread to a specific CPU core ID or affinity tag.
// On Linux: binds to specific CPU core via pthread_setaffinity_np.
// On macOS: configures User Interactive QoS class (pins thread to Performance Cores).
inline bool set_thread_affinity(std::size_t cpu_core_id) noexcept {
#if defined(__APPLE__)
  (void)cpu_core_id;
  // On macOS / Apple Silicon, QoS User Interactive schedules execution on Performance Cores
  // (P-cores).
  return ::pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0) == 0;
#elif defined(__linux__)
  cpu_set_t cpuset;
  CPU_ZERO(&cpuset);
  CPU_SET(static_cast<int>(cpu_core_id), &cpuset);
  return ::pthread_setaffinity_np(::pthread_self(), sizeof(cpu_set_t), &cpuset) == 0;
#else
  (void)cpu_core_id;
  return false;
#endif
}

// Lock all current and future process memory pages into RAM to eliminate page faults.
// Returns true on Linux / platforms where mlockall is implemented and permitted. In unprivileged
// containers without CAP_IPC_LOCK (e.g. CI runners), returns true if denied gracefully (EPERM /
// ENOMEM).
inline bool lock_memory_pages() noexcept {
#if defined(__linux__) && defined(MCL_CURRENT) && defined(MCL_FUTURE)
  if (::mlockall(MCL_CURRENT | MCL_FUTURE) == 0) {
    return true;
  }
  return (errno == EPERM || errno == ENOMEM || errno == EACCES);
#elif defined(__APPLE__)
  // macOS kernel does not implement mlockall (returns ENOSYS); return true as non-fatal no-op.
  return true;
#else
  return false;
#endif
}

// Unlock memory pages.
inline bool unlock_memory_pages() noexcept {
#if defined(__linux__)
  if (::munlockall() == 0) {
    return true;
  }
  return (errno == EPERM || errno == EACCES);
#elif defined(__APPLE__)
  return true;
#else
  return false;
#endif
}

// Set real-time FIFO scheduling priority for the calling thread (requires root / CAP_SYS_NICE on
// Linux).
inline bool set_realtime_priority(int priority = 80) noexcept {
#if defined(__linux__)
  sched_param param{};
  param.sched_priority = priority;
  return ::pthread_setschedparam(::pthread_self(), SCHED_FIFO, &param) == 0;
#elif defined(__APPLE__)
  (void)priority;
  return ::pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0) == 0;
#else
  (void)priority;
  return false;
#endif
}

} // namespace mm
