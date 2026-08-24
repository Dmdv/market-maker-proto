// One-Way Latency Decomposition Benchmark Probe.
// Decomposes end-to-end RTT into Leg 1 (Ingress), Leg 2 (Engine M2), and Leg 3 (Egress) at
// nanosecond resolution.
#include "mm/affinity.hpp"
#include "mm/clock_calibration.hpp"
#include "mm/shm_ring.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <string>
#include <thread>
#include <vector>

using namespace mm;

int main(int argc, char **argv) {
  std::size_t iterations = 100'000;
  if (argc > 1) {
    iterations = std::stoul(argv[1]);
  }

  // 1. Verify Clock Identity
  auto proof = ClockCalibrator::sample();
  std::cout << "===============================================================\n";
  std::cout << "  CLOCK IDENTITY PROOF & CALIBRATION\n";
  std::cout << "===============================================================\n";
  std::cout << "C++ std::chrono::steady_clock: " << proof.steady_clock_ns << " ns\n";
  std::cout << "POSIX clock_gettime monotonic:  " << proof.posix_monotonic_ns << " ns\n";
  std::cout << "Instantaneous Sampling Delta:   " << proof.offset_ns << " ns\n";
  std::cout << "Clock Origin Synchronization:   "
            << (proof.is_synchronized ? "VERIFIED (0ns drift)" : "DEVIATING") << "\n";
  std::cout << "===============================================================\n\n";

  const std::string oe_name = "/shm_decomp_oe_" + std::to_string(::getpid());
  const std::string rep_name = "/shm_decomp_rep_" + std::to_string(::getpid());

  auto oe_writer = ShmRing::create(oe_name, 4096);
  auto rep_writer = ShmRing::create(rep_name, 4096);

  auto oe_reader = ShmRing::attach(oe_name);
  auto rep_reader = ShmRing::attach(rep_name);

  std::atomic<bool> running{true};
  std::atomic<bool> engine_ready{false};
  std::atomic<bool> client_ready{false};

  std::vector<std::int64_t> leg1_ingress;
  std::vector<std::int64_t> leg2_engine;
  std::vector<std::int64_t> leg3_egress;
  std::vector<std::int64_t> total_rtt;

  leg1_ingress.reserve(iterations);
  leg2_engine.reserve(iterations);
  leg3_egress.reserve(iterations);
  total_rtt.reserve(iterations);

  // Engine Thread
  std::thread engine_thread([&] {
    set_thread_affinity(1);
    engine_ready.store(true, std::memory_order_release);

    ShmSlot in_order{};
    ShmSlot out_ack{};
    out_ack.ack.tag = static_cast<std::uint16_t>(ShmMsgType::OrderAck);

    while (running.load(std::memory_order_relaxed)) {
      if (oe_reader.try_pop(in_order)) {
        const auto e1 = std::chrono::steady_clock::now().time_since_epoch().count();

        // Simulate Matching Engine State Transition
        out_ack.ack.seq = in_order.new_order.seq;
        out_ack.ack.cl_id = in_order.new_order.cl_id;
        out_ack.ack.svc_ns = e1; // e1

        const auto e2 = std::chrono::steady_clock::now().time_since_epoch().count();
        out_ack.ack.engine_ts_ns = e2; // e2

        while (!rep_writer.try_push(out_ack)) {
#if defined(__x86_64__) || defined(_M_X64)
          __builtin_ia32_pause();
#elif defined(__aarch64__)
          asm volatile("yield");
#endif
        }
      } else {
#if defined(__x86_64__) || defined(_M_X64)
        __builtin_ia32_pause();
#elif defined(__aarch64__)
        asm volatile("yield");
#endif
      }
    }
  });

  // Client Thread
  std::thread client_thread([&] {
    set_thread_affinity(2);
    client_ready.store(true, std::memory_order_release);

    ShmSlot order{};
    order.new_order.tag = static_cast<std::uint16_t>(ShmMsgType::NewOrder);
    order.new_order.side = 1;
    order.new_order.px = 500000;
    order.new_order.qty = 10;

    ShmSlot ack{};

    // Warmup
    for (std::size_t i = 0; i < 1'000; ++i) {
      order.new_order.seq = i + 1;
      order.new_order.cl_id = i + 1;
      order.new_order.send_ts_ns = std::chrono::steady_clock::now().time_since_epoch().count();
      oe_writer.try_push(order);
      while (!rep_reader.try_pop(ack)) {
#if defined(__x86_64__) || defined(_M_X64)
        __builtin_ia32_pause();
#elif defined(__aarch64__)
        asm volatile("yield");
#endif
      }
    }

    // Benchmark Run
    for (std::size_t i = 0; i < iterations; ++i) {
      order.new_order.seq = i + 2000;
      order.new_order.cl_id = i + 2000;
      const auto t0 = std::chrono::steady_clock::now().time_since_epoch().count();
      order.new_order.send_ts_ns = t0;

      while (!oe_writer.try_push(order)) {
#if defined(__x86_64__) || defined(_M_X64)
        __builtin_ia32_pause();
#elif defined(__aarch64__)
        asm volatile("yield");
#endif
      }

      while (!rep_reader.try_pop(ack)) {
#if defined(__x86_64__) || defined(_M_X64)
        __builtin_ia32_pause();
#elif defined(__aarch64__)
        asm volatile("yield");
#endif
      }
      const auto t3 = std::chrono::steady_clock::now().time_since_epoch().count();

      const auto e1 = ack.ack.svc_ns;
      const auto e2 = ack.ack.engine_ts_ns;

      leg1_ingress.push_back(e1 - t0);
      leg2_engine.push_back(e2 - e1);
      leg3_egress.push_back(t3 - e2);
      total_rtt.push_back(t3 - t0);
    }
  });

  while (!engine_ready.load(std::memory_order_acquire) ||
         !client_ready.load(std::memory_order_acquire)) {
    std::this_thread::yield();
  }

  client_thread.join();
  running.store(false, std::memory_order_release);
  engine_thread.join();

  oe_writer.unlink();
  rep_writer.unlink();

  auto print_leg = [](std::vector<std::int64_t> &data, const std::string &name) {
    std::sort(data.begin(), data.end());
    auto pct = [&](double p) -> std::int64_t {
      std::size_t idx = static_cast<std::size_t>(p * (data.size() - 1));
      return data[idx];
    };

    std::cout << std::left << std::setw(35) << name << " | Min: " << std::setw(5) << data.front()
              << " ns"
              << " | p50: " << std::setw(5) << pct(0.50) << " ns"
              << " | p90: " << std::setw(5) << pct(0.90) << " ns"
              << " | p99: " << std::setw(5) << pct(0.99) << " ns"
              << " | p99.9: " << std::setw(5) << pct(0.999) << " ns\n";
  };

  std::cout << "==================================================================================="
               "====================\n";
  std::cout << "  ONE-WAY LATENCY DECOMPOSITION BREAKDOWN (" << iterations << " iterations)\n";
  std::cout << "==================================================================================="
               "====================\n";
  print_leg(leg1_ingress, "Leg 1: Ingress Transit (t0 -> e1)");
  print_leg(leg2_engine, "Leg 2: Engine Service Time (e1 -> e2)");
  print_leg(leg3_egress, "Leg 3: Egress Transit (e2 -> t3)");
  std::cout << "-----------------------------------------------------------------------------------"
               "--------------------\n";
  print_leg(total_rtt, "TOTAL RTT (t0 -> t3)");
  std::cout << "==================================================================================="
               "====================\n";

  return 0;
}
