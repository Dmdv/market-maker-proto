// End-to-end Ultra-Low Latency Benchmark Probe for Native C++ Strategy over Shared Memory.
// Measures M3 (Tick-to-Order), M2 (Engine Service Time), and M1 (Order-to-Ack RTT) in nanoseconds.
#include "mm/affinity.hpp"
#include "mm/shm_ring.hpp"
#include "mm/strategy_native.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

using namespace mm;

int main(int argc, char **argv) {
  std::size_t iterations = 100'000;
  if (argc > 1) {
    iterations = std::stoul(argv[1]);
  }

  const std::string md_name = "/shm_probe_md_" + std::to_string(::getpid());
  const std::string oe_name = "/shm_probe_oe_" + std::to_string(::getpid());
  const std::string rep_name = "/shm_probe_rep_" + std::to_string(::getpid());

  auto md_writer = ShmRing::create(md_name, 4096);
  auto oe_writer = ShmRing::create(oe_name, 4096);
  auto rep_writer = ShmRing::create(rep_name, 4096);

  auto md_reader = ShmRing::attach(md_name);
  auto oe_reader = ShmRing::attach(oe_name);
  auto rep_reader = ShmRing::attach(rep_name);

  std::atomic<bool> running{true};
  std::atomic<bool> engine_ready{false};
  std::atomic<bool> strat_ready{false};

  std::vector<std::int64_t> tick_to_order_latencies;
  tick_to_order_latencies.reserve(iterations);

  std::vector<std::int64_t> rtt_latencies;
  rtt_latencies.reserve(iterations);

  // 1. Mock Matching Engine Thread
  std::thread engine_thread([&] {
    set_thread_affinity(1);
    engine_ready.store(true, std::memory_order_release);

    ShmSlot in_order{};
    ShmSlot out_ack{};
    out_ack.ack.tag = static_cast<std::uint16_t>(ShmMsgType::OrderAck);

    while (running.load(std::memory_order_relaxed)) {
      if (oe_reader.try_pop(in_order)) {
        if (in_order.tag == static_cast<std::uint16_t>(ShmMsgType::NewOrder)) {
          out_ack.ack.seq = in_order.new_order.seq;
          out_ack.ack.cl_id = in_order.new_order.cl_id;
          out_ack.ack.engine_ts_ns = in_order.new_order.send_ts_ns;
          while (!rep_writer.try_push(out_ack)) {
#if defined(__x86_64__) || defined(_M_X64)
            __builtin_ia32_pause();
#elif defined(__aarch64__)
            asm volatile("yield");
#endif
          }
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

  // 2. Native Strategy Quoting Thread
  std::thread strat_thread([&] {
    set_thread_affinity(2);
    strat_ready.store(true, std::memory_order_release);

    NativeMarketMaker mm;
    ShmSlot in_slot{};

    while (running.load(std::memory_order_relaxed)) {
      // Poll Market Data
      if (md_reader.try_pop(in_slot)) {
        if (in_slot.tag == static_cast<std::uint16_t>(ShmMsgType::TopOfBook)) {
          const auto decision = mm.on_tob(in_slot.tob);
          if (decision.has_bid_order) {
            ShmSlot order_slot{};
            order_slot.new_order = decision.new_bid;
            const auto t_order = std::chrono::steady_clock::now().time_since_epoch().count();
            order_slot.new_order.send_ts_ns = t_order;

            const auto t_tick = in_slot.tob.venue_ns;
            if (t_tick > 0) {
              tick_to_order_latencies.push_back(t_order - t_tick);
            }

            while (!oe_writer.try_push(order_slot)) {
#if defined(__x86_64__) || defined(_M_X64)
              __builtin_ia32_pause();
#elif defined(__aarch64__)
              asm volatile("yield");
#endif
            }
          }
        }
      }

      // Poll Execution Reports
      if (rep_reader.try_pop(in_slot)) {
        if (in_slot.tag == static_cast<std::uint16_t>(ShmMsgType::OrderAck)) {
          const auto now_ns = std::chrono::steady_clock::now().time_since_epoch().count();
          if (in_slot.ack.engine_ts_ns > 0) {
            rtt_latencies.push_back(now_ns - in_slot.ack.engine_ts_ns);
          }
          mm.on_ack(in_slot.ack);
        }
      }
    }
  });

  while (!engine_ready.load(std::memory_order_acquire) ||
         !strat_ready.load(std::memory_order_acquire)) {
    std::this_thread::yield();
  }

  // Warmup (1,000 ticks)
  for (std::size_t i = 0; i < 1'000; ++i) {
    ShmSlot warm_tob{};
    warm_tob.tob.tag = static_cast<std::uint16_t>(ShmMsgType::TopOfBook);
    warm_tob.tob.md_seq = i + 1;
    warm_tob.tob.bid_px = 500000 + (i % 10);
    warm_tob.tob.ask_px = 500020 + (i % 10);
    warm_tob.tob.venue_ns = 0; // Warmup flag
    md_writer.try_push(warm_tob);
    std::this_thread::sleep_for(std::chrono::microseconds(10));
  }

  tick_to_order_latencies.clear();
  rtt_latencies.clear();

  // Benchmark Run (iterations ticks)
  for (std::size_t i = 0; i < iterations; ++i) {
    ShmSlot tob{};
    tob.tob.tag = static_cast<std::uint16_t>(ShmMsgType::TopOfBook);
    tob.tob.md_seq = i + 1000;
    tob.tob.bid_px = 500000 + (i % 20);
    tob.tob.ask_px = 500020 + (i % 20);
    tob.tob.venue_ns = std::chrono::steady_clock::now().time_since_epoch().count();

    while (!md_writer.try_push(tob)) {
#if defined(__x86_64__) || defined(_M_X64)
      __builtin_ia32_pause();
#elif defined(__aarch64__)
      asm volatile("yield");
#endif
    }

    // Paced at 5 microseconds interval (~200,000 msg/sec)
    auto target_time = std::chrono::steady_clock::now() + std::chrono::microseconds(5);
    while (std::chrono::steady_clock::now() < target_time) {
#if defined(__x86_64__) || defined(_M_X64)
      __builtin_ia32_pause();
#elif defined(__aarch64__)
      asm volatile("yield");
#endif
    }
  }

  // Allow draining
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  running.store(false, std::memory_order_release);
  engine_thread.join();
  strat_thread.join();

  md_writer.unlink();
  oe_writer.unlink();
  rep_writer.unlink();

  auto summarize = [](std::vector<std::int64_t> &data, const std::string &name) {
    if (data.empty()) {
      std::cout << name << ": No samples collected\n";
      return;
    }
    std::sort(data.begin(), data.end());
    auto pct = [&](double p) -> std::int64_t {
      std::size_t idx = static_cast<std::size_t>(p * (data.size() - 1));
      return data[idx];
    };

    std::cout << "=== " << name << " (" << data.size() << " samples) ===\n";
    std::cout << "Min:   " << std::setw(6) << data.front() << " ns (" << std::fixed
              << std::setprecision(3) << data.front() / 1000.0 << " µs)\n";
    std::cout << "p50:   " << std::setw(6) << pct(0.50) << " ns (" << pct(0.50) / 1000.0
              << " µs)\n";
    std::cout << "p90:   " << std::setw(6) << pct(0.90) << " ns (" << pct(0.90) / 1000.0
              << " µs)\n";
    std::cout << "p99:   " << std::setw(6) << pct(0.99) << " ns (" << pct(0.99) / 1000.0
              << " µs)\n";
    std::cout << "p99.9: " << std::setw(6) << pct(0.999) << " ns (" << pct(0.999) / 1000.0
              << " µs)\n";
    std::cout << "Max:   " << std::setw(6) << data.back() << " ns (" << data.back() / 1000.0
              << " µs)\n\n";
  };

  summarize(tick_to_order_latencies, "M3 Tick-to-Order (Market Data -> Order Send)");
  summarize(rtt_latencies, "M1 Order-to-Ack Round-Trip Latency");

  return 0;
}
