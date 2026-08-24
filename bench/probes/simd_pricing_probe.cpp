// Microbenchmark probe for SIMD-Vectorized Order Book Pricing (ARM NEON & AVX2).
#include "mm/simd_pricing.hpp"

#include <chrono>
#include <iomanip>
#include <iostream>
#include <vector>

using namespace mm;

int main() {
  const std::size_t iterations = 10'000'000;

  BookDepth4 depth{};
  depth.bid_px[0] = 50000.0;
  depth.bid_qty[0] = 1.5;
  depth.bid_px[1] = 49999.0;
  depth.bid_qty[1] = 3.0;
  depth.bid_px[2] = 49998.0;
  depth.bid_qty[2] = 5.2;
  depth.bid_px[3] = 49997.0;
  depth.bid_qty[3] = 8.1;

  depth.ask_px[0] = 50001.0;
  depth.ask_qty[0] = 1.2;
  depth.ask_px[1] = 50002.0;
  depth.ask_qty[1] = 2.8;
  depth.ask_px[2] = 50003.0;
  depth.ask_qty[2] = 4.9;
  depth.ask_px[3] = 50004.0;
  depth.ask_qty[3] = 7.5;

  // Warmup
  volatile double sink = 0.0;
  for (std::size_t i = 0; i < 100'000; ++i) {
    auto res = SimdPricingEngine::compute(depth, 5.0, static_cast<std::int64_t>(i % 20) - 10);
    sink += res.target_bid;
  }

  // Benchmark
  const auto t0 = std::chrono::steady_clock::now();
  for (std::size_t i = 0; i < iterations; ++i) {
    depth.bid_qty[0] = 1.5 + (i & 7) * 0.1;
    auto res = SimdPricingEngine::compute(depth, 5.0, static_cast<std::int64_t>(i % 20) - 10);
    sink += res.target_bid;
  }
  const auto t1 = std::chrono::steady_clock::now();

  const auto elapsed_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
  const double ns_per_op = static_cast<double>(elapsed_ns) / static_cast<double>(iterations);
  const double ops_per_sec = (static_cast<double>(iterations) / elapsed_ns) * 1e9;

  std::cout << "===============================================================\n";
  std::cout << "  SIMD Order Book Pricing Engine (Multi-Level Micro-price & OBI)\n";
#if defined(MM_HAS_NEON)
  std::cout << "  Vector Architecture: ARM NEON (128-bit FMA Vector Registers)\n";
#elif defined(MM_HAS_AVX2)
  std::cout << "  Vector Architecture: x86_64 AVX2 + FMA (256-bit Vector Registers)\n";
#else
  std::cout << "  Vector Architecture: Generic Scalar (Auto-vectorized)\n";
#endif
  std::cout << "===============================================================\n";
  std::cout << "Total iterations: " << iterations << "\n";
  std::cout << "Total elapsed:    " << elapsed_ns / 1'000'000.0 << " ms\n";
  std::cout << "Latency per calc: " << std::fixed << std::setprecision(2) << ns_per_op << " ns\n";
  std::cout << "Throughput:       " << std::fixed << std::setprecision(2)
            << ops_per_sec / 1'000'000.0 << " M ops/sec\n";
  std::cout << "Sink (anti-dce):  " << sink << "\n";
  std::cout << "===============================================================\n";

  return 0;
}
