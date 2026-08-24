// Shared Memory Lock-Free SPSC Ring Buffer and Flat SBE-style binary protocol for Ultra-Low Latency
// IPC. Supports both macOS and Linux POSIX shared memory (shm_open/mmap).
#pragma once

#include <atomic>
#include <bit>
#include <cassert>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/mman.h>
#include <sys/stat.h>
#include <type_traits>
#include <unistd.h>

namespace mm {

// ============================================================================
// SBE-style Flat Binary Protocol (Zero-Copy 64-byte Cache-Aligned Message Slots)
// ============================================================================

enum class ShmMsgType : std::uint16_t {
  TopOfBook = 1,
  NewOrder = 2,
  CancelOrder = 3,
  OrderAck = 4,
  OrderFill = 5,
  OrderReject = 6,
  CancelReject = 7,
  Heartbeat = 8,
};

#pragma pack(push, 1)

// TopOfBook market data broadcast from Engine to MM client (exactly 64 bytes).
struct ShmTopOfBook {
  std::uint16_t tag{static_cast<std::uint16_t>(ShmMsgType::TopOfBook)};
  std::uint16_t flags{0};
  std::uint32_t seq{0};
  std::uint64_t epoch{0};
  std::uint64_t md_seq{0};
  std::int64_t bid_px{0};
  std::int64_t bid_qty{0};
  std::int64_t ask_px{0};
  std::int64_t ask_qty{0};
  std::int64_t venue_ns{0};
};
static_assert(sizeof(ShmTopOfBook) == 64);

// NewOrder command from MM client to Engine (exactly 64 bytes).
struct ShmNewOrder {
  std::uint16_t tag{static_cast<std::uint16_t>(ShmMsgType::NewOrder)};
  std::uint8_t side{1};      // 1 = Bid, 2 = Ask
  std::uint8_t post_only{0}; // 1 = True, 0 = False
  std::uint32_t seq{0};
  std::uint64_t epoch{0};
  std::uint64_t md_seq{0};
  std::uint64_t cl_id{0}; // 64-bit binary client order identifier
  std::int64_t px{0};
  std::int64_t qty{0};
  std::int64_t send_ts_ns{0};
  std::uint8_t _reserved[8]{0};
};
static_assert(sizeof(ShmNewOrder) == 64);

// CancelOrder command from MM client to Engine (exactly 64 bytes).
struct ShmCancelOrder {
  std::uint16_t tag{static_cast<std::uint16_t>(ShmMsgType::CancelOrder)};
  std::uint16_t _pad{0};
  std::uint32_t seq{0};
  std::uint64_t epoch{0};
  std::uint64_t cl_id{0};
  std::int64_t send_ts_ns{0};
  std::uint8_t _reserved[32]{0};
};
static_assert(sizeof(ShmCancelOrder) == 64);

// OrderAck report from Engine to MM client (exactly 64 bytes).
struct ShmOrderAck {
  std::uint16_t tag{static_cast<std::uint16_t>(ShmMsgType::OrderAck)};
  std::uint16_t _pad{0};
  std::uint32_t seq{0};
  std::uint64_t epoch{0};
  std::uint64_t cl_id{0};
  std::uint64_t svc_ns{0};
  std::int64_t engine_ts_ns{0};
  std::uint8_t _reserved[24]{0};
};
static_assert(sizeof(ShmOrderAck) == 64);

// OrderFill report from Engine to MM client (exactly 64 bytes).
struct ShmOrderFill {
  std::uint16_t tag{static_cast<std::uint16_t>(ShmMsgType::OrderFill)};
  std::uint16_t _pad{0};
  std::uint32_t seq{0};
  std::uint64_t epoch{0};
  std::uint64_t cl_id{0};
  std::uint64_t md_seq{0};
  std::int64_t fill_px{0};
  std::int64_t fill_qty{0};
  std::uint8_t _reserved[16]{0};
};
static_assert(sizeof(ShmOrderFill) == 64);

// OrderReject report from Engine to MM client (exactly 64 bytes).
struct ShmOrderReject {
  std::uint16_t tag{static_cast<std::uint16_t>(ShmMsgType::OrderReject)};
  std::uint16_t code{0};
  std::uint32_t seq{0};
  std::uint64_t epoch{0};
  std::uint64_t cl_id{0};
  std::int64_t engine_ts_ns{0};
  std::uint8_t _reserved[32]{0};
};
static_assert(sizeof(ShmOrderReject) == 64);

// Universal fixed 64-byte slot fitting any message type in single cacheline.
struct alignas(64) ShmSlot {
  union {
    std::uint16_t tag;
    ShmTopOfBook tob;
    ShmNewOrder new_order;
    ShmCancelOrder cancel_order;
    ShmOrderAck ack;
    ShmOrderFill fill;
    ShmOrderReject reject;
    std::uint8_t raw[64];
  };
};
static_assert(sizeof(ShmSlot) == 64);
static_assert(alignof(ShmSlot) == 64);

#pragma pack(pop)

// ============================================================================
// Lock-Free SPSC Ring Memory Layout in Shared Memory
// ============================================================================

struct alignas(64) ShmRingHeader {
  static constexpr std::uint32_t kMagic = 0x53484D52; // "SHMR"

  alignas(64) std::atomic<std::uint64_t> head{0}; // Consumer read index
  alignas(64) std::atomic<std::uint64_t> tail{0}; // Producer write index
  alignas(64) std::atomic<std::uint64_t> dropped{0};

  std::uint32_t magic{kMagic};
  std::uint32_t capacity{0};
  std::uint32_t capacity_mask{0};
  std::uint32_t slot_size{sizeof(ShmSlot)};
};
static_assert(sizeof(ShmRingHeader) == 192);
static_assert(alignof(ShmRingHeader) == 64);

// ============================================================================
// ShmRing: Cross-Process Shared Memory SPSC Ring Buffer Controller
// ============================================================================

class ShmRing {
public:
  static constexpr std::size_t kDefaultCapacity = 4096;

  ~ShmRing() { close(); }

  // Non-copyable, movable.
  ShmRing(const ShmRing &) = delete;
  ShmRing &operator=(const ShmRing &) = delete;

  ShmRing(ShmRing &&other) noexcept { *this = std::move(other); }

  ShmRing &operator=(ShmRing &&other) noexcept {
    if (this != &other) {
      close();
      name_ = std::move(other.name_);
      fd_ = other.fd_;
      shm_size_ = other.shm_size_;
      header_ = other.header_;
      slots_ = other.slots_;
      is_creator_ = other.is_creator_;

      other.fd_ = -1;
      other.shm_size_ = 0;
      other.header_ = nullptr;
      other.slots_ = nullptr;
      other.is_creator_ = false;
    }
    return *this;
  }

  // Create a brand new Shared Memory SPSC Ring.
  static ShmRing create(const std::string &name, std::size_t min_capacity = kDefaultCapacity,
                        bool unlink_first = true) {
    if (min_capacity == 0) {
      throw std::invalid_argument("ShmRing: capacity must be > 0");
    }
    if (min_capacity > 1048576) {
      throw std::invalid_argument("ShmRing: capacity exceeds maximum 1048576");
    }
    const std::uint32_t capacity = static_cast<std::uint32_t>(std::bit_ceil(min_capacity));
    const std::size_t total_size = sizeof(ShmRingHeader) + capacity * sizeof(ShmSlot);

    const std::string posix_name = normalize_name(name);
    if (unlink_first) {
      ::shm_unlink(posix_name.c_str());
    }

    int fd = ::shm_open(posix_name.c_str(), O_CREAT | O_RDWR | O_EXCL, 0666);
    if (fd < 0) {
      fd = ::shm_open(posix_name.c_str(), O_RDWR, 0666);
      if (fd < 0) {
        throw std::runtime_error("ShmRing: failed to create shm segment " + posix_name + ": " +
                                 std::strerror(errno));
      }
    }

    if (::ftruncate(fd, static_cast<off_t>(total_size)) != 0) {
      ::close(fd);
      ::shm_unlink(posix_name.c_str());
      throw std::runtime_error("ShmRing: failed to ftruncate shm segment: " +
                               std::string(std::strerror(errno)));
    }

    void *addr = ::mmap(nullptr, total_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (addr == MAP_FAILED) {
      ::close(fd);
      ::shm_unlink(posix_name.c_str());
      throw std::runtime_error("ShmRing: failed to mmap shm segment: " +
                               std::string(std::strerror(errno)));
    }

    auto *header = reinterpret_cast<ShmRingHeader *>(addr);
    std::memset(addr, 0, total_size);
    header->magic = ShmRingHeader::kMagic;
    header->capacity = capacity;
    header->capacity_mask = capacity - 1;
    header->slot_size = sizeof(ShmSlot);

    auto *slots =
        reinterpret_cast<ShmSlot *>(reinterpret_cast<char *>(addr) + sizeof(ShmRingHeader));

    ShmRing ring;
    ring.name_ = posix_name;
    ring.fd_ = fd;
    ring.shm_size_ = total_size;
    ring.header_ = header;
    ring.slots_ = slots;
    ring.is_creator_ = true;
    return ring;
  }

  // Attach to an existing Shared Memory SPSC Ring.
  static ShmRing attach(const std::string &name) {
    const std::string posix_name = normalize_name(name);
    int fd = ::shm_open(posix_name.c_str(), O_RDWR, 0666);
    if (fd < 0) {
      throw std::runtime_error("ShmRing: failed to attach to shm segment " + posix_name + ": " +
                               std::strerror(errno));
    }

    struct stat st{};
    if (::fstat(fd, &st) != 0 || st.st_size < static_cast<off_t>(sizeof(ShmRingHeader))) {
      ::close(fd);
      throw std::runtime_error("ShmRing: invalid shm segment size for " + posix_name);
    }

    const std::size_t total_size = static_cast<std::size_t>(st.st_size);
    void *addr = ::mmap(nullptr, total_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (addr == MAP_FAILED) {
      ::close(fd);
      throw std::runtime_error("ShmRing: failed to mmap shm segment: " +
                               std::string(std::strerror(errno)));
    }

    auto *header = reinterpret_cast<ShmRingHeader *>(addr);
    if (header->magic != ShmRingHeader::kMagic || header->capacity == 0 ||
        header->slot_size != sizeof(ShmSlot)) {
      ::munmap(addr, total_size);
      ::close(fd);
      throw std::runtime_error("ShmRing: corrupted header in shm segment " + posix_name);
    }

    auto *slots =
        reinterpret_cast<ShmSlot *>(reinterpret_cast<char *>(addr) + sizeof(ShmRingHeader));

    ShmRing ring;
    ring.name_ = posix_name;
    ring.fd_ = fd;
    ring.shm_size_ = total_size;
    ring.header_ = header;
    ring.slots_ = slots;
    ring.is_creator_ = false;
    return ring;
  }

  // Producer: non-blocking try_push.
  // Returns true on success; returns false if full.
  // If count_drop is true, increments dropped counter on full.
  bool try_push(const ShmSlot &slot, bool count_drop = false) noexcept {
    assert(header_ != nullptr && slots_ != nullptr);
    const std::uint64_t tail = header_->tail.load(std::memory_order_relaxed);
    const std::uint64_t head = header_->head.load(std::memory_order_acquire);
    if (tail - head >= header_->capacity) {
      if (count_drop) {
        header_->dropped.store(header_->dropped.load(std::memory_order_relaxed) + 1,
                               std::memory_order_relaxed);
      }
      return false;
    }
    slots_[tail & header_->capacity_mask] = slot;
    header_->tail.store(tail + 1, std::memory_order_release);
    return true;
  }

  // Market Data (Lossy / Overwrite-Oldest) Push:
  // If the ring is full, advances head by one and writes the slot so the newest tick always wins.
  void push_overwrite(const ShmSlot &slot) noexcept {
    assert(header_ != nullptr && slots_ != nullptr);
    const std::uint64_t tail = header_->tail.load(std::memory_order_relaxed);
    const std::uint64_t head = header_->head.load(std::memory_order_acquire);
    if (tail - head >= header_->capacity) {
      header_->head.store(head + 1, std::memory_order_release);
      header_->dropped.store(header_->dropped.load(std::memory_order_relaxed) + 1,
                             std::memory_order_relaxed);
    }
    slots_[tail & header_->capacity_mask] = slot;
    header_->tail.store(tail + 1, std::memory_order_release);
  }

  // Consumer: non-blocking try_pop.
  // Returns true if a slot was read; false if empty.
  [[nodiscard]] bool try_pop(ShmSlot &slot) noexcept {
    assert(header_ != nullptr && slots_ != nullptr);
    const std::uint64_t head = header_->head.load(std::memory_order_relaxed);
    const std::uint64_t tail = header_->tail.load(std::memory_order_acquire);
    if (tail == head) {
      return false;
    }
    slot = slots_[head & header_->capacity_mask];
    header_->head.store(head + 1, std::memory_order_release);
    return true;
  }

  [[nodiscard]] std::size_t pending() const noexcept {
    if (!header_)
      return 0;
    const std::uint64_t head = header_->head.load(std::memory_order_relaxed);
    const std::uint64_t tail = header_->tail.load(std::memory_order_acquire);
    return static_cast<std::size_t>(tail - head);
  }

  [[nodiscard]] std::uint64_t dropped() const noexcept {
    return header_ ? header_->dropped.load(std::memory_order_relaxed) : 0;
  }

  [[nodiscard]] std::uint32_t capacity() const noexcept { return header_ ? header_->capacity : 0; }

  [[nodiscard]] const std::string &name() const noexcept { return name_; }

  void unlink() noexcept {
    if (!name_.empty()) {
      ::shm_unlink(name_.c_str());
    }
  }

private:
  ShmRing() = default;

  void close() noexcept {
    if (header_ != nullptr && shm_size_ > 0) {
      ::munmap(header_, shm_size_);
      header_ = nullptr;
      slots_ = nullptr;
      shm_size_ = 0;
    }
    if (fd_ >= 0) {
      ::close(fd_);
      fd_ = -1;
    }
  }

  static std::string normalize_name(const std::string &name) {
    if (name.empty()) {
      throw std::invalid_argument("ShmRing: name cannot be empty");
    }
    return (name[0] == '/') ? name : ("/" + name);
  }

  std::string name_;
  int fd_{-1};
  std::size_t shm_size_{0};
  ShmRingHeader *header_{nullptr};
  ShmSlot *slots_{nullptr};
  bool is_creator_{false};
};

} // namespace mm
