"""Unit tests for Shared Memory SPSC Ring Buffer IPC (shm_ipc.py)."""

import os
import struct
import tempfile
from pathlib import Path

import pytest

from mmclient import shm_ipc
from mmclient.shm_ipc import (
    HEADER_MAGIC,
    HEADER_SIZE,
    SLOT_SIZE,
    TAG_CANCEL_ORDER,
    TAG_NEW_ORDER,
    TAG_ORDER_ACK,
    TAG_ORDER_FILL,
    TAG_ORDER_REJECT,
    TAG_TOB,
    ShmCancelOrder,
    ShmNewOrder,
    ShmOrderAck,
    ShmOrderFill,
    ShmOrderReject,
    ShmRingReader,
    ShmRingWriter,
    ShmTopOfBook,
)


def _create_temp_ring(capacity: int = 16) -> tuple[int, str, int]:
    """Helper creating a temporary file formatted as an ShmRing."""
    total_size = HEADER_SIZE + capacity * SLOT_SIZE
    fd, path_str = tempfile.mkstemp(prefix="test_shm_ring_")
    os.ftruncate(fd, total_size)
    path = Path(path_str)
    with path.open("r+b") as f:
        f.write(b"\x00" * total_size)
        f.seek(136)
        f.write(struct.pack("=IIII", HEADER_MAGIC, capacity, capacity - 1, SLOT_SIZE))
    return fd, path_str, total_size


def test_shm_ring_reader_writer_roundtrip() -> None:
    fd, path_str, _ = _create_temp_ring(capacity=8)
    path = Path(path_str)
    try:
        with ShmRingWriter(path_str) as writer, ShmRingReader(path_str) as reader:
            assert writer.capacity == 8
            assert reader.capacity == 8
            assert writer.pending() == 0
            assert reader.pending() == 0
            assert reader.dropped() == 0

            # Push TopOfBook
            tob_slot = shm_ipc._TOB_STRUCT.pack(
                TAG_TOB, 0, 1, 1, 100, 500000, 10, 500010, 8, 123456789
            )
            assert writer.try_push_raw(tob_slot)
            assert writer.pending() == 1
            assert reader.pending() == 1

            # Read TopOfBook
            item = reader.try_pop()
            assert item is not None
            tag, tob = item
            assert tag == TAG_TOB
            assert isinstance(tob, ShmTopOfBook)
            assert tob.bid_px == 500000
            assert tob.ask_px == 500010
            assert tob.venue_ns == 123456789

            # Push NewOrder via helper
            order = ShmNewOrder(
                side=1,
                post_only=1,
                seq=2,
                epoch=1,
                md_seq=100,
                cl_id=555,
                px=499990,
                qty=5,
                send_ts_ns=1000,
            )
            assert writer.push_new_order(order)

            # Push CancelOrder via helper
            cancel = ShmCancelOrder(seq=3, epoch=1, cl_id=555, send_ts_ns=2000)
            assert writer.push_cancel_order(cancel)

            # Pop NewOrder & CancelOrder
            item2 = reader.try_pop()
            assert item2 is not None and item2[0] == TAG_NEW_ORDER
            assert isinstance(item2[1], (bytes, ShmNewOrder))

            item3 = reader.try_pop()
            assert item3 is not None and item3[0] == TAG_CANCEL_ORDER
            assert isinstance(item3[1], (bytes, ShmCancelOrder))
    finally:
        os.close(fd)
        if path.exists():
            path.unlink()


def test_shm_ring_ack_fill_reject_roundtrip() -> None:
    fd, path_str, _ = _create_temp_ring(capacity=8)
    path = Path(path_str)
    try:
        with ShmRingWriter(path_str) as writer, ShmRingReader(path_str) as reader:
            ack_slot = shm_ipc._ACK_STRUCT.pack(
                TAG_ORDER_ACK, 0, 4, 1, 555, 150, 2000, b"\x00" * 24
            )
            fill_slot = shm_ipc._FILL_STRUCT.pack(
                TAG_ORDER_FILL, 0, 5, 1, 555, 100, 500000, 5, b"\x00" * 16
            )
            rej_slot = shm_ipc._REJECT_STRUCT.pack(
                TAG_ORDER_REJECT, 3, 6, 1, 555, 3000, b"\x00" * 32
            )
            unk_slot = struct.pack("=H62s", 99, b"unknown payload" + b"\x00" * 47)

            assert writer.try_push_raw(ack_slot)
            assert writer.try_push_raw(fill_slot)
            assert writer.try_push_raw(rej_slot)
            assert writer.try_push_raw(unk_slot)

            # Pop Ack
            _, ack = reader.try_pop()  # type: ignore[misc]
            assert isinstance(ack, ShmOrderAck)
            assert ack.svc_ns == 150

            # Pop Fill
            _, fill = reader.try_pop()  # type: ignore[misc]
            assert isinstance(fill, ShmOrderFill)
            assert fill.fill_px == 500000

            # Pop Reject
            _, rej = reader.try_pop()  # type: ignore[misc]
            assert isinstance(rej, ShmOrderReject)
            assert rej.code == 3

            # Pop Unknown
            tag_unk, unk_raw = reader.try_pop()  # type: ignore[misc]
            assert tag_unk == 99
            assert isinstance(unk_raw, bytes)

            # Ring now empty
            assert reader.try_pop() is None
            assert reader.try_pop_raw() is None
    finally:
        os.close(fd)
        if path.exists():
            path.unlink()


def test_shm_ring_full_backpressure_and_drop() -> None:
    fd, path_str, size = _create_temp_ring(capacity=2)
    path = Path(path_str)
    try:
        writer = ShmRingWriter(fd, total_size=size)
        reader = ShmRingReader(fd, total_size=size)

        slot1 = b"\x01" * 64
        slot2 = b"\x02" * 64
        slot3 = b"\x03" * 64

        assert writer.try_push_raw(slot1)
        assert writer.try_push_raw(slot2)
        assert writer.pending() == 2

        # Push without count_drop when full
        assert not writer.try_push_raw(slot3, count_drop=False)
        assert reader.dropped() == 0

        # Push with count_drop when full
        assert not writer.try_push_raw(slot3, count_drop=True)
        assert reader.dropped() == 1

        # Invalid slot length raises
        with pytest.raises(ValueError, match="Slot must be 64 bytes"):
            writer.try_push_raw(b"too short")

        writer.close()
        reader.close()
    finally:
        os.close(fd)
        if path.exists():
            path.unlink()


def test_shm_ring_invalid_magic() -> None:
    fd, path_str, _ = _create_temp_ring(capacity=4)
    path = Path(path_str)
    try:
        with path.open("r+b") as f:
            f.seek(136)
            f.write(struct.pack("=I", 0x12345678))

        with pytest.raises(ValueError, match="Invalid SHM ring magic"):
            ShmRingReader(path_str)

        with pytest.raises(ValueError, match="Invalid SHM ring magic"):
            ShmRingWriter(path_str)
    finally:
        os.close(fd)
        if path.exists():
            path.unlink()


def test_shm_path_candidates(monkeypatch: pytest.MonkeyPatch) -> None:
    # 1. Existing absolute file
    p1 = shm_ipc._shm_path("/tmp")
    assert p1 == "/tmp"

    # 2. Mock candidate 0 existing (/dev/shm)
    def mock_exists_dev(self: Path) -> bool:
        return "/dev/shm" in str(self)

    monkeypatch.setattr(Path, "exists", mock_exists_dev)
    assert "/dev/shm" in shm_ipc._shm_path("my_ring")

    # 3. Mock no candidate existing, fallback to /tmp
    monkeypatch.setattr(Path, "exists", lambda self: False)
    assert "/tmp" in shm_ipc._shm_path("my_ring")
