"""The TUNED arm: `picows` + `msgspec`, on `uvloop` — the "after" half of an AGGREGATE stack A/B.

picows' callback surface is most of the win, but it hands frames up as it parses them: the
fragmentation policy, the reassembly cap and manual PONGs are OURS to enforce (see `_Listener`).
"""

import asyncio
import time
from collections.abc import Callable

import picows

from mmclient import protocol
from mmclient._session import (
    CLOSE_BAD_PAYLOAD,
    CLOSE_NORMAL,
    CLOSE_PROTOCOL,
    CLOSE_TOO_BIG,
    MAX_MSG_BYTES,
    Codec,
    Ending,
    SessionDriver,
    classify_close_body,
    is_sendable_close_code,
)
from mmclient.strategy import Strategy

__all__ = ["run_client"]

SUBPROTOCOL = "mm.v1"
TIMER_INTERVAL_S = 0.1
# How long to keep reading after our close frame, waiting for the peer's reply — see
# `_Listener.await_peer_close`. Bounded, so an engine that never replies cannot hold us open.
CLOSE_GRACE_S = 1.0

_CONTROL_FRAMES = (picows.WSMsgType.CLOSE, picows.WSMsgType.PING, picows.WSMsgType.PONG)

TUNED_CODEC = Codec(decode=protocol.decode, encode=protocol.encode, name="tuned")


class _Listener(picows.WSListener):
    """picows' callback surface. Runs on the event loop thread, one call per parsed frame."""

    def __init__(
        self,
        strategy: Strategy,
        codec: Codec,
        on_sample: Callable[[str, int], None] | None,
    ) -> None:
        self._driver = SessionDriver(strategy, codec)
        self._on_sample = on_sample
        self._fragments: list[bytes] = []
        self._fragment_bytes = 0
        # Whether a fragmented message is OPEN — distinct from `_fragments` being non-empty: the
        # opening frame may carry an empty payload, and the rules key on "message in progress".
        self._opened = False
        # THREE STATES, because one flag conflated three questions and silenced PING as well:
        # no DATA (`_data_ended`), nothing at all (`_close_received`), cleanup ran (`_finalized`).
        self._data_ended = False
        self._close_received = False
        self._finalized = False
        # Set when one of OUR callbacks raised. picows transfers those through the same channel as
        # its own parser errors, so without provenance a bug here reads as a peer framing fault.
        self._callback_error: BaseException | None = None
        # A Task rather than a bare Future so it sits in the same asyncio.wait set as the stop
        # watcher; the sentinel coroutine simply awaits it.
        self._done: asyncio.Future[None] = asyncio.get_running_loop().create_future()
        self.finished: asyncio.Task[None] = asyncio.create_task(self._await_done())
        # Separate from `_done`: "the session is over" and "the peer answered our close" are
        # different events, and conflating them is what made the close abnormal.
        self._peer_close: asyncio.Future[None] = asyncio.get_running_loop().create_future()

    async def _await_done(self) -> None:
        await self._done

    async def await_peer_close(self, timeout: float = CLOSE_GRACE_S) -> bool:
        """Keep reading until the peer answers our close frame, or the grace elapses; returns
        whether it ANSWERED, which is what decides if a graceful teardown is worth attempting.

        Dropping the transport with data still queued makes the kernel send RST, discarding our
        close frame — the engine then records 1006 for a deliberate 4000 (RFC 6455 §7.1.1).
        """
        try:
            await asyncio.wait_for(asyncio.shield(self._peer_close), timeout)
        except TimeoutError:
            return False  # an engine that never replies must not hold us open
        return True

    def _note_peer_close(self) -> None:
        if not self._peer_close.done():
            self._peer_close.set_result(None)

    @property
    def driver(self) -> SessionDriver:
        return self._driver

    def on_ws_frame(self, transport: picows.WSTransport, frame: picows.WSFrame) -> None:
        """Every inbound frame. Wraps the dispatch so no exception escapes silently.

        An escaping callback exception left the terminal state UNSET, so picows kept delivering
        buffered frames to the same listener. Recorded for provenance, then re-raised.
        """
        try:
            self._dispatch(transport, frame)
        except BaseException as exc:
            self._callback_error = exc
            self._fragments.clear()
            self._fragment_bytes = 0
            self._opened = False
            self.finish()
            raise

    def _dispatch(self, transport: picows.WSTransport, frame: picows.WSFrame) -> None:
        """The checks, in the order RFC 6455 requires them.

        Order is load-bearing: the terminal filter must not precede the reserved-bit check, and
        it must not silence PING — that made the arms diverge on liveness.
        """
        # After the peer has closed there is nothing left to answer or to decide.
        if self._close_received:
            return
        # RESERVED BITS, FIRST, UNCONDITIONALLY, FOR EVERY FRAME TYPE. We negotiate no extensions,
        # so RFC 6455 §5.2 makes any set RSV bit a connection failure — control frames included.
        if frame.rsv1 or frame.rsv2 or frame.rsv3:
            self._abort(transport, "reserved bit set with no extension negotiated")
            return
        # CONTROL FRAMES STAY LEGAL until the peer closes: RFC 6455 §5.4 permits them
        # mid-fragment, and a PING arriving after our own close still deserves its PONG.
        if frame.msg_type in _CONTROL_FRAMES:
            self._on_control(transport, frame)
            return
        # DATA stops the moment we have decided to close. Two conditions: `end_data` covers the
        # operator-stop path, where a close goes out with no strategy verdict behind it.
        if self._data_ended or self._driver.close_intent is not None:
            return
        self._on_data(transport, frame)

    def _on_control(self, transport: picows.WSTransport, frame: picows.WSFrame) -> None:
        """CLOSE, PING and PONG — legal at any time, including mid-fragment (RFC 6455 §5.4)."""
        if frame.msg_type is picows.WSMsgType.CLOSE:
            self._on_peer_close(transport, frame)
        elif frame.msg_type is picows.WSMsgType.PING:
            # PONGED BY HAND, which is why `enable_auto_pong` is off: the library answers pings
            # BENEATH this callback, past the RSV check above.
            transport.send_pong(frame.get_payload_as_bytes())
        # An unsolicited PONG is legal and has nothing to answer: we send no pings.

    def _on_data(self, transport: picows.WSTransport, frame: picows.WSFrame) -> None:
        if frame.msg_type is picows.WSMsgType.BINARY:
            # 1002, not a skip: the wire is text-only and the engine closes 1002 on a binary
            # frame from us. Skipping the OPENING frame leaves its CONTINUATIONs with no head.
            self._abort(transport, "binary frame: the mm.v1 wire is text-only")
            return
        payload = self._reassemble(transport, frame)
        if payload is None:
            return
        # UTF-8 IS VALIDATED HERE, because picows does not: a TEXT frame that is not valid UTF-8
        # is RFC 6455 §8.1's 1007 — what the engine and `websockets` both answer with, not 1002.
        try:
            payload.decode()
        except UnicodeDecodeError:
            self._driver.note_protocol_error("text frame is not valid UTF-8", CLOSE_BAD_PAYLOAD)
            _close(transport, self._driver)
            self.finish()
            return
        received = time.perf_counter_ns()
        for out in self._driver.on_bytes(payload, received):
            transport.send(picows.WSMsgType.TEXT, out)
        if self._on_sample is not None:
            self._on_sample("decode_dispatch_ns", time.perf_counter_ns() - received)
        if self._driver.close_intent is not None:
            _close(transport, self._driver)
            self.finish()

    def _on_peer_close(self, transport: picows.WSTransport, frame: picows.WSFrame) -> None:
        """Answer a CLOSE the peer initiated, with a code it is legal for us to send.

        Classification is :func:`classify_close_body`, so both arms share one table: picows
        reports an empty body and an encoded 0 alike, so LENGTH decides; 1014 is reserved here.
        """
        # Any partial message dies with the close: a continuation behind it has nothing to attach
        # to. Control frames dispatch BEFORE fragment state, so a mid-fragment CLOSE lands here.
        self._fragments.clear()
        self._fragment_bytes = 0
        self._opened = False
        body = frame.get_payload_as_bytes() or b""
        code, legal = classify_close_body(body)
        if not legal:
            # OUR VERDICT, and it has to be latched as one: answering an illegal code while
            # reporting "the peer went away" made the reconnect policy dial a broken peer again.
            shown = 0 if code is None else code
            self._driver.note_protocol_error(f"peer sent an illegal close code {shown}")
        if not transport.is_close_frame_sent:
            if code is None:
                # Empty body: legal, no status on the wire. Answer 1000, never encoded 0.
                transport.send_close(picows.WSCloseCode.OK)
            elif legal:
                # Status is the first two bytes; anything after is the UTF-8 reason.
                reason = body[2:]
                transport.send_close(picows.WSCloseCode(code), reason)
            else:
                transport.send_close(picows.WSCloseCode.PROTOCOL_ERROR, b"illegal close code")
        self._close_received = True
        self._note_peer_close()
        self.finish()

    def _abort(
        self, transport: picows.WSTransport, reason: str, code: int = CLOSE_PROTOCOL
    ) -> None:
        """Latch a framing verdict, send the close, and end the session.

        Deliberately does NOT `transport.disconnect()`: the exit path drains toward the peer's
        reply first, so an abort lands as the code we chose rather than as 1006.
        """
        self._fragments.clear()
        self._fragment_bytes = 0
        self._opened = False
        self._driver.note_protocol_error(reason, code)
        _close(transport, self._driver)
        self.finish()

    def _reassemble(self, transport: picows.WSTransport, frame: picows.WSFrame) -> bytes | None:
        """Fragment reassembly, with the RFC 6455 §5.4 sequencing rules picows leaves to us.

        Three rules: a CONTINUATION with no message open is an orphan, a new TEXT frame while one
        is open interleaves, and the cap applies to the RUNNING TOTAL. A breach closes 1009.
        """
        chunk = frame.get_payload_as_bytes()
        continuation = frame.msg_type is picows.WSMsgType.CONTINUATION

        if continuation and not self._opened:
            self._abort(transport, "continuation frame with no message open")
            return None
        if not continuation and self._opened:
            self._abort(transport, "new data frame while a fragmented message is open")
            return None
        if frame.fin and not self._opened:
            return chunk  # the common case: one frame, one message, no buffering at all

        self._fragment_bytes += len(chunk)
        if self._fragment_bytes > MAX_MSG_BYTES:
            self._abort(transport, "reassembled message past the 64 KiB cap", CLOSE_TOO_BIG)
            return None
        self._fragments.append(chunk)
        self._opened = not frame.fin
        if not frame.fin:
            return None
        payload = b"".join(self._fragments)
        self._fragments.clear()
        self._fragment_bytes = 0
        return payload

    def on_ws_disconnected(self, transport: picows.WSTransport) -> None:
        # Delegates, so a callback arriving after `run_client` finalized is a no-op rather than
        # a second `on_disconnect()` against whatever session exists by then.
        self.finalize()

    @property
    def callback_error(self) -> BaseException | None:
        """An exception one of OUR callbacks raised; read by teardown so a bug in this module is
        never reported as a peer framing fault."""
        return self._callback_error

    def end_data(self) -> None:
        """Stop letting DATA frames drive the strategy; control frames stay legal.

        Called before any close frame goes out, including on the operator-stop path — otherwise a
        book arriving during the peer-close grace is quoted on a socket already closing.
        """
        self._data_ended = True

    def finish(self) -> None:
        """Idempotent: the close frame, the disconnect and the timer can each arrive first,
        and whichever does ends the session exactly once."""
        self._data_ended = True
        if not self._done.done():
            self._done.set_result(None)

    def finalize(self) -> None:
        """Local end-of-connection cleanup, idempotent and safe to run early.

        picows' `on_ws_disconnected` may arrive LATE, by which time a retry can have built a new
        session on the same Strategy — a late `on_disconnect()` would wipe the NEW session.
        """
        if self._finalized:
            return
        self._finalized = True
        self._driver.on_disconnect()
        self._note_peer_close()
        self.finish()


async def run_client(
    url: str,
    strategy: Strategy,
    *,
    codec: Codec = TUNED_CODEC,
    stop: asyncio.Event,
    on_sample: Callable[[str, int], None] | None = None,
    timer_interval_s: float = TIMER_INTERVAL_S,
) -> Ending:
    """One connection, driven until the strategy stops or `stop` is set.

    Returns WHY it ended: the reconnect policy cannot be derived from a bare return.
    """
    listener: _Listener | None = None

    def factory() -> picows.WSListener:
        nonlocal listener
        listener = _Listener(strategy, codec, on_sample)
        return listener

    transport, _ = await picows.ws_connect(
        factory,
        url,
        # OFF, so RSV validation sees every frame: the library would otherwise answer a PING
        # beneath this listener, including one carrying a reserved bit. `_Listener` ponges by hand.
        enable_auto_pong=False,
        max_frame_size=MAX_MSG_BYTES,
        extra_headers={"Sec-WebSocket-Protocol": SUBPROTOCOL},
    )
    assert listener is not None

    # The try opens HERE, before the handshake check: a raise from `_assert_handshake` used to
    # escape with the transport open, letting a retry drive a second session on the SAME Strategy.
    timer: asyncio.Task[None] | None = None
    stopper: asyncio.Task[bool] | None = None
    stopped = False
    try:
        _assert_handshake(transport)
        timer = asyncio.create_task(_timer(transport, listener, stop, timer_interval_s))
        stopper = asyncio.create_task(stop.wait())
        # The TIMER is in the wait set, and every completed task's result is inspected: left out,
        # a codec or send failure inside it died in a task nobody looked at.
        done, _ = await asyncio.wait(
            {listener.finished, stopper, timer}, return_when=asyncio.FIRST_COMPLETED
        )
        for task in done:
            task.result()
        stopped = stopper in done
    finally:
        if timer is not None:
            timer.cancel()
        if stopper is not None:
            stopper.cancel()
        listener.finished.cancel()
        # DATA OFF BEFORE THE CLOSE FRAME: without it the stop path spent the peer-close grace
        # still decoding books and emitting orders down a socket it had said goodbye on.
        listener.end_data()
        _close(transport, listener.driver)
        # Drain toward the peer's close reply BEFORE tearing the transport down; dropping it with
        # data still queued turns our deliberate 4000 into an abnormal 1006 at the engine.
        answered = False
        try:
            answered = await listener.await_peer_close()
        finally:
            # NESTED, so the transport is torn down even if the drain is cancelled; a cancelled
            # cleanup that skips the disconnect leaks the connection.
            await _teardown(transport, listener, graceful=answered)
    # AFTER teardown, deliberately: computed inside the `try`, the classification froze before the
    # teardown could latch a parser verdict, telling app.py to retry a deterministic fault.
    return _ending(listener.driver, stopped=stopped)


async def _teardown(transport: picows.WSTransport, listener: _Listener, *, graceful: bool) -> None:
    """Close the transport and make sure the local end-of-connection work has happened.

    A peer that did NOT answer gets no graceful drain — both calls can block to the kernel's TCP
    timeout. `finalize()` runs HERE because `on_ws_disconnected` can arrive after we return.
    """
    transport.disconnect(graceful=graceful)
    try:
        await asyncio.wait_for(transport.wait_disconnected(), CLOSE_GRACE_S)
    except TimeoutError:
        transport.disconnect(graceful=False)
    except picows.WSProtocolError as exc:
        # PROVENANCE FIRST: picows sends OUR callbacks' exceptions through the same channel as
        # its own parser errors, so treating every WSProtocolError as a peer fault hid bugs here.
        if listener.callback_error is not None:
            raise
        # A genuine parser verdict about the peer. Latched, it reports as DECIDED — terminal,
        # because dialling again asks a peer with a broken framer the same question.
        listener.driver.note_protocol_error(f"peer framing rejected by the parser: {exc}")
    finally:
        listener.finalize()


def _ending(driver: SessionDriver, *, stopped: bool) -> Ending:
    """Why the connection ended.

    A LATCHED INTENT WINS over the stop flag: the timer can latch 4000 and still be sending its
    cancels when the operator hits Ctrl-C, and the wire and the return value must agree.
    """
    if driver.close_intent is not None:
        return Ending.DECIDED
    return Ending.STOPPED if stopped else Ending.PEER_GONE


def _assert_handshake(transport: picows.WSTransport) -> None:
    """The same fluency check the naive arm makes, against picows' response object: a run that did
    not negotiate `mm.v1`, or that negotiated an extension, is not the run being measured."""
    headers = transport.response.headers
    negotiated = headers.get("Sec-WebSocket-Protocol")
    if negotiated != SUBPROTOCOL:
        msg = f"server did not accept {SUBPROTOCOL!r} (got {negotiated!r})"
        raise RuntimeError(msg)
    extensions = headers.get("Sec-WebSocket-Extensions")
    if extensions:
        msg = f"server negotiated extensions, which the measured run forbids: {extensions}"
        raise RuntimeError(msg)


async def _timer(
    transport: picows.WSTransport,
    listener: _Listener,
    stop: asyncio.Event,
    interval_s: float = TIMER_INTERVAL_S,
) -> None:
    while True:
        # WAITS on stop rather than sleeping through it: a plain sleep makes shutdown pay a full
        # interval, and leaves the loop's own exit unreachable in a test.
        try:
            await asyncio.wait_for(stop.wait(), interval_s)
        except TimeoutError:
            pass  # the interval elapsed: do the tick
        else:
            return  # stop was set
        for frame in listener.driver.on_timer(time.perf_counter_ns()):
            transport.send(picows.WSMsgType.TEXT, frame)
        if listener.driver.close_intent is not None:
            _close(transport, listener.driver)
            listener.finish()
            return


def _close(transport: picows.WSTransport, driver: SessionDriver) -> None:
    if transport.is_close_frame_sent or transport.is_disconnected:
        return
    intent = driver.close_intent
    code = intent.code if intent is not None else CLOSE_NORMAL
    reason = intent.reason if intent is not None else "client shutdown"
    # Shared table: never emit a status we would refuse to echo. A latched intent carrying 1014
    # or 0 would otherwise leave through a library that accepts it.
    if not is_sendable_close_code(code):
        code = CLOSE_PROTOCOL
    # WSCloseCode is an IntEnum and accepts the private range, which is what lets 4000 — the
    # stale-stop code — go out through a library enum that has no member for it.
    transport.send_close(picows.WSCloseCode(code), reason[:120].encode())
