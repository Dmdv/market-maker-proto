"""The transport-independent session driver, tested without a socket.

`SessionDriver` is the half both adapters share, so a defect here is a defect in BOTH arms
of the §6 measurement at once — and the close-code arithmetic it owns is what an operator
reads to tell a strategy decision from a transport fault. Driven with bytes and integers
directly: it holds no socket and no clock, which is exactly what makes that possible.
"""

import json

from mmclient import protocol
from mmclient._session import CLOSE_PROTOCOL, CLOSE_STALE, Codec, SessionDriver
from mmclient.strategy import Strategy

TUNED = Codec(decode=protocol.decode, encode=protocol.encode, name="tuned")


def _driver(stale_ns: int = 10**9) -> SessionDriver:
    return SessionDriver(Strategy(symbol="MOCKUSDT", qty=5, max_qty=10, stale_ns=stale_ns), TUNED)


def _tob(seq: int, md_seq: int, bid_px: int = 100, ask_px: int = 110, *, epoch: int = 1) -> bytes:
    return json.dumps(
        {
            "t": "top_of_book",
            "v": 1,
            "seq": seq,
            "epoch": epoch,
            "md_seq": md_seq,
            "symbol": "MOCKUSDT",
            "bid_px": bid_px,
            "bid_qty": 50,
            "ask_px": ask_px,
            "ask_qty": 50,
        }
    ).encode()


def test_an_undecodable_frame_is_a_protocol_close_not_an_exception() -> None:
    """The engine breaking its own protocol is a 1002 on our initiative. Raising instead
    would hand the adapter an exception to interpret, and the two arms would interpret it
    differently — which is the one thing the shared driver exists to prevent."""
    d = _driver()
    assert d.on_bytes(b"{not json", now_ns=0) == []
    assert d.close_intent is not None
    assert d.close_intent.code == CLOSE_PROTOCOL
    assert "undecodable" in d.close_intent.reason


def test_the_first_close_intent_wins_over_a_later_strategy_stop() -> None:
    """Mirrors the engine's own close-latch discipline. A stale stop arriving after a
    protocol verdict must not rewrite the code the peer will see — an operator reading 4000
    would conclude the feed went quiet when in fact the engine broke framing."""
    d = _driver(stale_ns=1)
    d.on_bytes(_tob(1, 1), now_ns=0)
    d.on_bytes(b"garbage", now_ns=1)
    assert d.close_intent is not None and d.close_intent.code == CLOSE_PROTOCOL

    d.on_timer(now_ns=10**9)  # would be a stale stop on its own
    assert d.close_intent.code == CLOSE_PROTOCOL
    assert "undecodable" in d.close_intent.reason


def test_the_first_close_intent_wins_over_a_later_framing_verdict() -> None:
    """The same rule from the other direction: an adapter's framing verdict — a binary
    frame, a reassembly past the cap — must not overwrite a verdict already reached."""
    d = _driver()
    d.on_bytes(b"garbage", now_ns=0)
    first = d.close_intent
    d.note_protocol_error("reassembled message past the 64 KiB cap")
    assert d.close_intent is first


def test_a_stale_stop_closes_4000_and_a_framing_fault_closes_1002() -> None:
    """The distinction the private code exists for: 4000 is a strategy DECISION, 1002 is a
    verdict on the peer's framing, and a demo that could not tell them apart would report a
    quiet feed and a broken engine identically."""
    stale = _driver(stale_ns=1)
    stale.on_bytes(_tob(1, 1), now_ns=0)
    stale.on_timer(now_ns=10**9)
    assert stale.close_intent is not None and stale.close_intent.code == CLOSE_STALE

    framing = _driver()
    framing.note_protocol_error("binary frame: the mm.v1 wire is text-only")
    assert framing.close_intent is not None and framing.close_intent.code == CLOSE_PROTOCOL


def test_the_outbound_envelope_seq_is_contiguous_across_message_kinds() -> None:
    """The counter belongs to the CONNECTION, so it must not restart or skip between a
    cancel and the new order that replaces it — the engine closes 1002 on a gap."""
    d = _driver()
    frames = d.on_bytes(_tob(1, 1), now_ns=0)
    frames += d.on_bytes(_tob(2, 2, bid_px=101), now_ns=1)
    seqs = [json.loads(f)["seq"] for f in frames]
    assert seqs == list(range(1, len(seqs) + 1))


def test_the_engine_s_epoch_is_adopted_not_assumed() -> None:
    """The engine mints a fresh epoch on every accept, from 1. A driver that assumed 1 was
    correct exactly once: after a reconnect the engine is on 2, the strategy's epoch guard
    drops every tick, and the client quotes nothing, emits no StopQuoting and never exits.
    A silent hang is this client's worst failure — the demo and the benchmark both look like
    a quiet market while it happens."""
    d = _driver()
    frames = d.on_bytes(_tob(1, 1, epoch=7), now_ns=0)

    assert len(frames) == 2
    assert [json.loads(f)["epoch"] for f in frames] == [7, 7]
    assert d.close_intent is None
