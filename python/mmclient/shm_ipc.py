"""Shared Memory Lock-Free SPSC Ring Buffer & SBE Flat Binary IPC (Python).

Zero-copy in-memory exchange of 64-byte message slots between C++ Engine and Python Client.
"""

from __future__ import annotations

import mmap
import os
import struct
from pathlib import Path
from typing import Final, NamedTuple

__all__ = [
    "TAG_CANCEL_ORDER",
    "TAG_NEW_ORDER",
    "TAG_ORDER_ACK",
    "TAG_ORDER_FILL",
    "TAG_ORDER_REJECT",
    "TAG_TOB",
    "ShmCancelOrder",
    "ShmNewOrder",
    "ShmOrderAck",
    "ShmOrderFill",
    "ShmOrderReject",
    "ShmRingReader",
    "ShmRingWriter",
    "ShmTopOfBook",
]

TAG_TOB: Final[int] = 1
TAG_NEW_ORDER: Final[int] = 2
TAG_CANCEL_ORDER: Final[int] = 3
TAG_ORDER_ACK: Final[int] = 4
TAG_ORDER_FILL: Final[int] = 5
TAG_ORDER_REJECT: Final[int] = 6
TAG_CANCEL_REJECT: Final[int] = 7
TAG_HEARTBEAT: Final[int] = 8

HEADER_MAGIC: Final[int] = 0x53484D52  # "SHMR"
HEADER_SIZE: Final[int] = 192
SLOT_SIZE: Final[int] = 64

# Byte offsets in ShmRingHeader
_OFF_HEAD: Final[int] = 0
_OFF_TAIL: Final[int] = 64
_OFF_DROPPED: Final[int] = 128
_OFF_MAGIC: Final[int] = 136
_OFF_CAPACITY: Final[int] = 140
_OFF_MASK: Final[int] = 144
_OFF_SLOT_SIZE: Final[int] = 148

_U64: Final[struct.Struct] = struct.Struct("=Q")
_U32: Final[struct.Struct] = struct.Struct("=I")

# Message pack/unpack structs (each exactly 64 bytes)
_TOB_STRUCT: Final[struct.Struct] = struct.Struct("=HHIQQqqqqq")
_NEW_ORDER_STRUCT: Final[struct.Struct] = struct.Struct("=HBBIQQQqqq8s")
_CANCEL_ORDER_STRUCT: Final[struct.Struct] = struct.Struct("=HHIQQq32s")
_ACK_STRUCT: Final[struct.Struct] = struct.Struct("=HHIQQQq24s")
_FILL_STRUCT: Final[struct.Struct] = struct.Struct("=HHIQQQqq16s")
_REJECT_STRUCT: Final[struct.Struct] = struct.Struct("=HHIQQq32s")

assert _TOB_STRUCT.size == SLOT_SIZE
assert _NEW_ORDER_STRUCT.size == SLOT_SIZE
assert _CANCEL_ORDER_STRUCT.size == SLOT_SIZE
assert _ACK_STRUCT.size == SLOT_SIZE
assert _FILL_STRUCT.size == SLOT_SIZE
assert _REJECT_STRUCT.size == SLOT_SIZE


class ShmTopOfBook(NamedTuple):
    flags: int
    seq: int
    epoch: int
    md_seq: int
    bid_px: int
    bid_qty: int
    ask_px: int
    ask_qty: int
    venue_ns: int


class ShmNewOrder(NamedTuple):
    side: int  # 1 = Bid, 2 = Ask
    post_only: int
    seq: int
    epoch: int
    md_seq: int
    cl_id: int
    px: int
    qty: int
    send_ts_ns: int


class ShmCancelOrder(NamedTuple):
    seq: int
    epoch: int
    cl_id: int
    send_ts_ns: int


class ShmOrderAck(NamedTuple):
    seq: int
    epoch: int
    cl_id: int
    svc_ns: int
    engine_ts_ns: int


class ShmOrderFill(NamedTuple):
    seq: int
    epoch: int
    cl_id: int
    md_seq: int
    fill_px: int
    fill_qty: int


class ShmOrderReject(NamedTuple):
    code: int
    seq: int
    epoch: int
    cl_id: int
    engine_ts_ns: int


def _shm_path(name: str) -> str:
    raw_path = Path(name)
    if raw_path.exists():
        return str(raw_path)
    cleaned = name.lstrip("/")
    candidates = (
        Path(f"/dev/shm/{cleaned}"),
        Path(f"/tmp/{cleaned}"),
        Path(f"/tmp/boost_interprocess/{cleaned}"),
    )
    for c in candidates:
        if c.exists():
            return str(c)
    return str(candidates[0]) if Path("/dev/shm").exists() else str(candidates[1])


class ShmRingReader:
    """Zero-copy consumer reading 64-byte slots from POSIX Shared Memory SPSC Ring."""

    def __init__(self, fd_or_path: int | str, total_size: int | None = None) -> None:
        if isinstance(fd_or_path, str):
            path = _shm_path(fd_or_path)
            self._fd = os.open(path, os.O_RDWR)
            self._close_fd = True
            st = os.fstat(self._fd)
            self._size = st.st_size
        else:
            self._fd = fd_or_path
            self._close_fd = False
            self._size = total_size or os.fstat(self._fd).st_size

        self._buf = mmap.mmap(
            self._fd, self._size, mmap.MAP_SHARED, mmap.PROT_READ | mmap.PROT_WRITE
        )

        magic = _U32.unpack_from(self._buf, _OFF_MAGIC)[0]
        if magic != HEADER_MAGIC:
            self.close()
            raise ValueError(f"Invalid SHM ring magic: {hex(magic)}")

        self.capacity = _U32.unpack_from(self._buf, _OFF_CAPACITY)[0]
        self.capacity_mask = _U32.unpack_from(self._buf, _OFF_MASK)[0]
        self.slot_size = _U32.unpack_from(self._buf, _OFF_SLOT_SIZE)[0]

    def close(self) -> None:
        self._buf.close()
        if self._close_fd and self._fd >= 0:
            os.close(self._fd)
            self._fd = -1

    def __enter__(self) -> ShmRingReader:
        return self

    def __exit__(self, *args: object) -> None:
        self.close()

    def pending(self) -> int:
        head = int(_U64.unpack_from(self._buf, _OFF_HEAD)[0])
        tail = int(_U64.unpack_from(self._buf, _OFF_TAIL)[0])
        return max(0, tail - head)

    def dropped(self) -> int:
        return int(_U64.unpack_from(self._buf, _OFF_DROPPED)[0])

    def try_pop_raw(self) -> bytes | None:
        head = _U64.unpack_from(self._buf, _OFF_HEAD)[0]
        tail = _U64.unpack_from(self._buf, _OFF_TAIL)[0]
        if head == tail:
            return None
        offset = HEADER_SIZE + (head & self.capacity_mask) * SLOT_SIZE
        slot = self._buf[offset : offset + SLOT_SIZE]
        _U64.pack_into(self._buf, _OFF_HEAD, head + 1)
        return slot

    def try_pop(self) -> tuple[int, object] | None:
        slot = self.try_pop_raw()
        if slot is None:
            return None
        tag = struct.unpack_from("=H", slot, 0)[0]
        return tag, _unpack_payload(tag, slot)


def _unpack_tob(slot: bytes) -> ShmTopOfBook:
    _, flags, seq, epoch, md_seq, bid_px, bid_qty, ask_px, ask_qty, venue_ns = _TOB_STRUCT.unpack(
        slot
    )
    return ShmTopOfBook(flags, seq, epoch, md_seq, bid_px, bid_qty, ask_px, ask_qty, venue_ns)


def _unpack_new_order(slot: bytes) -> ShmNewOrder:
    _, side, post_only, seq, epoch, md_seq, cl_id, px, qty, send_ts_ns, _ = (
        _NEW_ORDER_STRUCT.unpack(slot)
    )
    return ShmNewOrder(side, post_only, seq, epoch, md_seq, cl_id, px, qty, send_ts_ns)


def _unpack_cancel_order(slot: bytes) -> ShmCancelOrder:
    _, _, seq, epoch, cl_id, send_ts_ns, _ = _CANCEL_ORDER_STRUCT.unpack(slot)
    return ShmCancelOrder(seq, epoch, cl_id, send_ts_ns)


def _unpack_ack(slot: bytes) -> ShmOrderAck:
    _, _, seq, epoch, cl_id, svc_ns, engine_ts_ns, _ = _ACK_STRUCT.unpack(slot)
    return ShmOrderAck(seq, epoch, cl_id, svc_ns, engine_ts_ns)


def _unpack_fill(slot: bytes) -> ShmOrderFill:
    _, _, seq, epoch, cl_id, md_seq, fill_px, fill_qty, _ = _FILL_STRUCT.unpack(slot)
    return ShmOrderFill(seq, epoch, cl_id, md_seq, fill_px, fill_qty)


def _unpack_reject(slot: bytes) -> ShmOrderReject:
    _, code, seq, epoch, cl_id, engine_ts_ns, _ = _REJECT_STRUCT.unpack(slot)
    return ShmOrderReject(code, seq, epoch, cl_id, engine_ts_ns)


_DECODERS: Final = {
    TAG_TOB: _unpack_tob,
    TAG_NEW_ORDER: _unpack_new_order,
    TAG_CANCEL_ORDER: _unpack_cancel_order,
    TAG_ORDER_ACK: _unpack_ack,
    TAG_ORDER_FILL: _unpack_fill,
    TAG_ORDER_REJECT: _unpack_reject,
}


def _unpack_payload(tag: int, slot: bytes) -> object:
    decoder = _DECODERS.get(tag)
    return decoder(slot) if decoder is not None else slot


class ShmRingWriter:
    """Producer writing 64-byte slots into POSIX Shared Memory SPSC Ring."""

    def __init__(self, fd_or_path: int | str, total_size: int | None = None) -> None:
        if isinstance(fd_or_path, str):
            path = _shm_path(fd_or_path)
            self._fd = os.open(path, os.O_RDWR)
            self._close_fd = True
            st = os.fstat(self._fd)
            self._size = st.st_size
        else:
            self._fd = fd_or_path
            self._close_fd = False
            self._size = total_size or os.fstat(self._fd).st_size

        self._buf = mmap.mmap(
            self._fd, self._size, mmap.MAP_SHARED, mmap.PROT_READ | mmap.PROT_WRITE
        )

        magic = _U32.unpack_from(self._buf, _OFF_MAGIC)[0]
        if magic != HEADER_MAGIC:
            self.close()
            raise ValueError(f"Invalid SHM ring magic: {hex(magic)}")

        self.capacity = _U32.unpack_from(self._buf, _OFF_CAPACITY)[0]
        self.capacity_mask = _U32.unpack_from(self._buf, _OFF_MASK)[0]
        self.slot_size = _U32.unpack_from(self._buf, _OFF_SLOT_SIZE)[0]

    def close(self) -> None:
        self._buf.close()
        if self._close_fd and self._fd >= 0:
            os.close(self._fd)
            self._fd = -1

    def __enter__(self) -> ShmRingWriter:
        return self

    def __exit__(self, *args: object) -> None:
        self.close()

    def pending(self) -> int:
        head = int(_U64.unpack_from(self._buf, _OFF_HEAD)[0])
        tail = int(_U64.unpack_from(self._buf, _OFF_TAIL)[0])
        return max(0, tail - head)

    def try_push_raw(self, slot_bytes: bytes, *, count_drop: bool = False) -> bool:
        if len(slot_bytes) != SLOT_SIZE:
            raise ValueError(f"Slot must be {SLOT_SIZE} bytes, got {len(slot_bytes)}")
        head = _U64.unpack_from(self._buf, _OFF_HEAD)[0]
        tail = _U64.unpack_from(self._buf, _OFF_TAIL)[0]
        if tail - head >= self.capacity:
            if count_drop:
                dropped = _U64.unpack_from(self._buf, _OFF_DROPPED)[0]
                _U64.pack_into(self._buf, _OFF_DROPPED, dropped + 1)
            return False
        offset = HEADER_SIZE + (tail & self.capacity_mask) * SLOT_SIZE
        self._buf[offset : offset + SLOT_SIZE] = slot_bytes
        _U64.pack_into(self._buf, _OFF_TAIL, tail + 1)
        return True

    def push_new_order(self, order: ShmNewOrder) -> bool:
        slot = _NEW_ORDER_STRUCT.pack(
            TAG_NEW_ORDER,
            order.side,
            order.post_only,
            order.seq,
            order.epoch,
            order.md_seq,
            order.cl_id,
            order.px,
            order.qty,
            order.send_ts_ns,
            b"\x00" * 8,
        )
        return self.try_push_raw(slot)

    def push_cancel_order(self, cancel: ShmCancelOrder) -> bool:
        slot = _CANCEL_ORDER_STRUCT.pack(
            TAG_CANCEL_ORDER,
            0,
            cancel.seq,
            cancel.epoch,
            cancel.cl_id,
            cancel.send_ts_ns,
            b"\x00" * 32,
        )
        return self.try_push_raw(slot)
