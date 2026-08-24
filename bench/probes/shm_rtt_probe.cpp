// Microbenchmark probe measuring cross-thread/cross-process Shared Memory SPSC RTT latency.
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

  const std::string req_name = "/shm_bench_req_" + std::to_string(::getpid());
  const std::string resp_name = "/shm_bench_resp_" + std::to_string(::getpid());

  auto req_writer = ShmRing::create(req_name, 4096);
  auto resp_reader = ShmRing::create(resp_name, 4096);

  auto req_reader = ShmRing::attach(req_name);
  auto resp_writer = ShmRing::attach(resp_name);

  std::atomic<bool> running{true};
  std::atomic<bool> ready{false};

  // Mock Engine consumer thread
  std::thread engine_thread([&] {
    ready.store(true, std::memory_order_release);
    ShmSlot in_slot{};
    ShmSlot out_slot{};
    out_slot.ack.tag = static_cast<std::uint16_t>(ShmMsgType::OrderAck);

    while (running.load(std::memory_order_relaxed)) {
      if (req_reader.try_pop(in_slot)) {
        out_slot.ack.seq = in_slot.new_order.seq;
        out_slot.ack.cl_id = in_slot.new_order.cl_id;
        out_slot.ack.engine_ts_ns = in_slot.new_order.send_ts_ns;
        while (!resp_writer.try_push(out_slot)) {
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

  while (!ready.load(std::memory_order_acquire)) {
    std::this_thread::yield();
  }

  // Warmup (10,000 iterations)
  ShmSlot warm_req{};
  warm_req.new_order.tag = static_cast<std::uint16_t>(ShmMsgType::NewOrder);
  ShmSlot warm_resp{};
  for (std::size_t i = 0; i < 10'000; ++i) {
    warm_req.new_order.seq = static_cast<std::uint32_t>(i);
    req_writer.try_push(warm_req);
    while (!resp_reader.try_pop(warm_resp)) {
#if defined(__x86_64__) || defined(_M_X64)
      __builtin_ia32_pause();
#elif defined(__aarch64__)
      asm volatile("yield");
#endif
    }
  }

  // Measurement run
  std::vector<std::int64_t> latencies_ns;
  latencies_ns.reserve(iterations);

  ShmSlot req{};
  req.new_order.tag = static_cast<std::uint16_t>(ShmMsgType::NewOrder);
  ShmSlot resp{};

  for (std::size_t i = 0; i < iterations; ++i) {
    req.new_order.seq = static_cast<std::uint32_t>(i);
    req.new_order.cl_id = 100000 + i;

    const auto t0 = std::chrono::steady_clock::now();
    req_writer.try_push(req);
    while (!resp_reader.try_pop(resp)) {
#if defined(__x86_64__) || defined(_M_X64)
      __builtin_ia32_pause();
#elif defined(__aarch64__)
      asm volatile("yield");
#endif
    }
    const auto t1 = std::chrono::steady_clock::now();

    const auto rtt_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
    latencies_ns.push_back(rtt_ns);
  }

  running.store(false, std::memory_order_release);
  engine_thread.join();

  req_writer.unlink();
  resp_reader.unlink();

  std::sort(latencies_ns.begin(), latencies_ns.end());

  auto pct = [&](double p) -> std::int64_t {
    std::size_t idx = static_cast<std::size_t>(p * (latencies_ns.size() - 1));
    return latencies_ns[idx];
  };

  std::cout << "=== Shared Memory SPSC Lock-Free RTT Benchmark (" << iterations
            << " iterations) ===\n";
  std::cout << "Min:   " << std::setw(6) << latencies_ns.front() << " ns (" << std::fixed
            << std::setprecision(3) << latencies_ns.front() / 1000.0 << " µs)\n";
  std::cout << "p50:   " << std::setw(6) << pct(0.50) << " ns (" << pct(0.50) / 1000.0 << " µs)\n";
  std::cout << "p90:   " << std::setw(6) << pct(0.90) << " ns (" << pct(0.90) / 1000.0 << " µs)\n";
  std::cout << "p99:   " << std::setw(6) << pct(0.99) << " ns (" << pct(0.99) / 1000.0 << " µs)\n";
  std::cout << "p99.9: " << std::setw(6) << pct(0.999) << " ns (" << pct(0.999) / 1000.0
            << " µs)\n";
  std::cout << "Max:   " << std::setw(6) << latencies_ns.back() << " ns (" << pct(1.0) / 1000.0
            << " µs)\n";

  return 0;
}
