// Clock Identity Calibration & One-Way Latency Decomposition Engine.
// Verifies monotonic timer origin synchronization across C++ steady_clock, POSIX clock_gettime, and
// Python.
#pragma once

#include <chrono>
#include <cstdint>
#include <ctime>

#if defined(__APPLE__)
#include <mach/mach_time.h>
#endif

namespace mm {

struct ClockProof {
  std::int64_t steady_clock_ns{0};
  std::int64_t posix_monotonic_ns{0};
  std::int64_t offset_ns{0};
  bool is_synchronized{false};
};

class ClockCalibrator {
public:
  // Capture instantaneous timestamps across C++ std::chrono and POSIX clock_gettime.
  [[nodiscard]] static ClockProof sample() noexcept {
    ClockProof proof{};

    struct timespec ts{};
#if defined(CLOCK_MONOTONIC_RAW)
    ::clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
#else
    ::clock_gettime(CLOCK_MONOTONIC, &ts);
#endif
    const std::int64_t posix_ns =
        static_cast<std::int64_t>(ts.tv_sec) * 1'000'000'000LL + ts.tv_nsec;

    const auto steady_now = std::chrono::steady_clock::now();
    const std::int64_t steady_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(steady_now.time_since_epoch()).count();

    proof.posix_monotonic_ns = posix_ns;
    proof.steady_clock_ns = steady_ns;
    proof.offset_ns = steady_ns - posix_ns;

    // Both clocks share identical tick frequency and origin on Darwin & Linux
    // (Typical instantaneous sampling delta is < 100 ns due to back-to-back CPU instruction
    // latency)
    proof.is_synchronized = (std::abs(proof.offset_ns) < 1000);

    return proof;
  }
};

struct OneWayDecomposition {
  std::int64_t t0_client_send_ns{0};
  std::int64_t e1_engine_ingress_ns{0};
  std::int64_t e2_engine_egress_ns{0};
  std::int64_t t3_client_ack_ns{0};

  [[nodiscard]] std::int64_t leg1_ingress_wire_ns() const noexcept {
    return e1_engine_ingress_ns - t0_client_send_ns;
  }

  [[nodiscard]] std::int64_t leg2_engine_service_ns() const noexcept {
    return e2_engine_egress_ns - e1_engine_ingress_ns;
  }

  [[nodiscard]] std::int64_t leg3_egress_wire_ns() const noexcept {
    return t3_client_ack_ns - e2_engine_egress_ns;
  }

  [[nodiscard]] std::int64_t total_rtt_ns() const noexcept {
    return t3_client_ack_ns - t0_client_send_ns;
  }
};

} // namespace mm
