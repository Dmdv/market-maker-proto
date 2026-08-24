// Unit tests for Shared Memory SPSC Lock-Free Ring Buffer (mm/shm_ring.hpp).
#include <catch2/catch_test_macros.hpp>

#include "mm/shm_ring.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

using namespace mm;

TEST_CASE("shm_ring: message slot alignment and sizes", "[shm][ull]") {
  STATIC_REQUIRE(sizeof(ShmSlot) == 64);
  STATIC_REQUIRE(alignof(ShmSlot) == 64);
  STATIC_REQUIRE(sizeof(ShmRingHeader) >= 64);
  STATIC_REQUIRE(alignof(ShmRingHeader) == 64);

  ShmSlot slot{};
  slot.tob.tag = static_cast<std::uint16_t>(ShmMsgType::TopOfBook);
  slot.tob.seq = 100;
  slot.tob.epoch = 1;
  slot.tob.md_seq = 42;
  slot.tob.bid_px = 500000;
  slot.tob.bid_qty = 10;
  slot.tob.ask_px = 500010;
  slot.tob.ask_qty = 8;
  slot.tob.venue_ns = 123456789;

  CHECK(slot.tag == static_cast<std::uint16_t>(ShmMsgType::TopOfBook));
  CHECK(slot.tob.bid_px == 500000);
  CHECK(slot.tob.ask_px == 500010);
  CHECK(slot.tob.venue_ns == 123456789);
}

TEST_CASE("shm_ring: create, attach, push, pop single-threaded", "[shm][ull]") {
  const std::string ring_name = "/test_shm_ring_st_" + std::to_string(::getpid());
  auto ring = ShmRing::create(ring_name, 16);
  CHECK(ring.capacity() == 16);
  CHECK(ring.pending() == 0);
  CHECK(ring.dropped() == 0);

  // Attach second handle
  auto reader = ShmRing::attach(ring_name);
  CHECK(reader.capacity() == 16);

  // Push 10 slots
  for (std::uint32_t i = 1; i <= 10; ++i) {
    ShmSlot slot{};
    slot.new_order.tag = static_cast<std::uint16_t>(ShmMsgType::NewOrder);
    slot.new_order.seq = i;
    slot.new_order.cl_id = 1000 + i;
    slot.new_order.px = 400000 + i * 10;
    slot.new_order.qty = i * 2;
    REQUIRE(ring.try_push(slot));
  }

  CHECK(ring.pending() == 10);
  CHECK(reader.pending() == 10);

  // Read back and verify order and fields
  for (std::uint32_t i = 1; i <= 10; ++i) {
    ShmSlot read_slot{};
    REQUIRE(reader.try_pop(read_slot));
    CHECK(read_slot.tag == static_cast<std::uint16_t>(ShmMsgType::NewOrder));
    CHECK(read_slot.new_order.seq == i);
    CHECK(read_slot.new_order.cl_id == 1000 + i);
    CHECK(read_slot.new_order.px == 400000 + i * 10);
    CHECK(read_slot.new_order.qty == i * 2);
  }

  CHECK(reader.pending() == 0);
  ShmSlot empty_slot{};
  CHECK_FALSE(reader.try_pop(empty_slot));

  ring.unlink();
}

TEST_CASE("shm_ring: full backpressure and dropped count", "[shm][ull]") {
  const std::string ring_name = "/test_shm_ring_full_" + std::to_string(::getpid());
  auto ring = ShmRing::create(ring_name, 4);
  CHECK(ring.capacity() == 4);

  for (std::uint32_t i = 1; i <= 4; ++i) {
    ShmSlot slot{};
    slot.tag = static_cast<std::uint16_t>(ShmMsgType::Heartbeat);
    REQUIRE(ring.try_push(slot));
  }
  CHECK(ring.pending() == 4);

  // 5th push with count_drop=true must fail and increment dropped
  ShmSlot overflow{};
  overflow.tag = static_cast<std::uint16_t>(ShmMsgType::Heartbeat);
  CHECK_FALSE(ring.try_push(overflow, /*count_drop=*/true));
  CHECK(ring.dropped() == 1);
  CHECK(ring.pending() == 4);

  // Pop one, then push succeeds
  ShmSlot popped{};
  REQUIRE(ring.try_pop(popped));
  CHECK(ring.pending() == 3);

  CHECK(ring.try_push(overflow));
  CHECK(ring.pending() == 4);
  CHECK(ring.dropped() == 1);

  // Overwrite push advances head
  ShmSlot ow_slot{};
  ow_slot.tob.tag = static_cast<std::uint16_t>(ShmMsgType::TopOfBook);
  ow_slot.tob.seq = 999;
  ring.push_overwrite(ow_slot);
  CHECK(ring.pending() == 4);
  CHECK(ring.dropped() == 2);

  ring.unlink();
}

TEST_CASE("shm_ring: cross-thread lock-free SPSC 1,000,000 messages", "[shm][ull]") {
  const std::string ring_name = "/test_shm_ring_mt_" + std::to_string(::getpid());
  constexpr std::size_t kCount = 1'000'000;
  constexpr std::size_t kCap = 1024;

  auto writer_ring = ShmRing::create(ring_name, kCap);
  auto reader_ring = ShmRing::attach(ring_name);

  std::atomic<bool> start_flag{false};

  std::thread reader_thread([&] {
    while (!start_flag.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
    std::size_t received = 0;
    ShmSlot slot{};
    while (received < kCount) {
      if (reader_ring.try_pop(slot)) {
        if (slot.ack.seq != received) {
          FAIL("Sequence mismatch: expected " << received << " got " << slot.ack.seq);
        }
        ++received;
      } else {
#if defined(__x86_64__) || defined(_M_X64)
        __builtin_ia32_pause();
#elif defined(__aarch64__)
        asm volatile("yield");
#endif
      }
    }
    CHECK(received == kCount);
  });

  std::thread writer_thread([&] {
    while (!start_flag.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
    for (std::size_t i = 0; i < kCount; ++i) {
      ShmSlot slot{};
      slot.ack.tag = static_cast<std::uint16_t>(ShmMsgType::OrderAck);
      slot.ack.seq = static_cast<std::uint32_t>(i);
      slot.ack.cl_id = 5000 + i;
      slot.ack.svc_ns = 120;
      while (!writer_ring.try_push(slot)) {
#if defined(__x86_64__) || defined(_M_X64)
        __builtin_ia32_pause();
#elif defined(__aarch64__)
        asm volatile("yield");
#endif
      }
    }
  });

  start_flag.store(true, std::memory_order_release);
  writer_thread.join();
  reader_thread.join();

  CHECK(reader_ring.pending() == 0);
  CHECK(writer_ring.dropped() == 0);

  writer_ring.unlink();
}

TEST_CASE("shm_ring: invalid capacity and missing segment checks", "[shm][ull]") {
  CHECK_THROWS_AS(ShmRing::create("/test_invalid_zero", 0), std::invalid_argument);
  CHECK_THROWS_AS(ShmRing::create("/test_invalid_huge", 2000000), std::invalid_argument);
  CHECK_THROWS_AS(ShmRing::attach("/non_existent_shm_segment_12345"), std::runtime_error);
}
