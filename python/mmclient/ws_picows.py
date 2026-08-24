"""The TUNED arm: `picows` + `msgspec`, on `uvloop`.

The "after" half of the §6 optimization. picows is a Cython WebSocket implementation with a
callback surface rather than an async-iterator one, which is most of the win: no per-frame
coroutine, no queue hop between the parser and the handler.

Everything except bytes-on-a-socket lives in `_session.SessionDriver`, shared with the naive
arm, so the strategy core, the envelope stamping and the safety controls are common-mode.

WHAT ACTUALLY DIFFERS BETWEEN THE ARMS is the whole ratified stack, not two layers of it:
`websockets + asyncio + stdlib json + nlohmann` -> `picows + uvloop + msgspec + glaze`
(02-decision-record.md). This docstring used to say "transport and codec and in nothing else",
which is false in both directions: `app.py` runs the tuned arm on `uvloop.run` and the naive arm
on `asyncio.run`, and the C++ leg moves with the codec flag. The measured delta is therefore an
AGGREGATE stack result and must be reported as one; attributing it to any single layer needs the
per-layer decomposition OPTIMIZATION.md builds from the component microbenchmarks, not this
end-to-end number.

HARDENING (record D2's binding condition on this library). picows hands frames up as it
parses them, which means the fragmentation policy is OURS to enforce:
  * `max_frame_size=MAX_MSG_BYTES` caps a single frame at the transport bound;
  * CONTINUATION frames are reassembled here, against the SAME cap, because a peer can
    otherwise send unlimited 1 KiB fragments and defeat a per-frame limit entirely;
  * `enable_auto_pong` is turned OFF and pings are answered by hand (see `ws_connect` below).
    The library's own auto-pong replies before the frame reaches our handler, which would put
    every PING outside the RSV-bit validation this adapter is required to perform. Answering
    manually costs one dispatch and keeps every inbound frame on one code path.
"""

import asyncio
import socket
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
# How long to keep reading after sending our close frame, waiting for the peer's reply. See
# `_Listener.await_peer_close` for why a close is not finished when the frame has been sent.
# Bounded, because an engine that never replies must not hold the client open.
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
        # Whether a fragmented message is OPEN. Distinct from `_fragments` being non-empty:
        # the opening frame of a fragmented message may carry an empty payload, and the
        # sequencing rules key on "is a message in progress", not on "have we buffered bytes".
        self._opened = False
        # THREE STATES, because one bool conflated three different questions and answered two
        # of them wrongly:
        #   _data_ended    — we have decided to close, so no further DATA frame may drive the
        #                    strategy. A probe fed TEXT(FIN=0) -> CLOSE -> CONTINUATION and
        #                    watched the continuation mutate state and attempt a send AFTER
        #                    our close frame went out.
        #   _close_received — the peer has closed; nothing at all is processed after that.
        #   _finalized     — local end-of-connection work has run.
        # Collapsed into one flag, "we are closing" also silenced PING — so between our own
        # CLOSE and the peer's reply the tuned arm stopped answering pings while the naive arm
        # kept answering them, which is an arm divergence in liveness behaviour.
        self._data_ended = False
        self._close_received = False
        self._finalized = False
        # Set when one of OUR callbacks raised. picows transfers such exceptions through the
        # same channel as its own parser errors, and without provenance the teardown path
        # classified a bug in our code as "the peer's framing is bad" and swallowed it.
        self._callback_error: BaseException | None = None
        # A Task rather than a bare Future so it sits in the same asyncio.wait set as the
        # stop watcher without a type contortion; the sentinel coroutine simply awaits it.
        self._done: asyncio.Future[None] = asyncio.get_running_loop().create_future()
        self.finished: asyncio.Task[None] = asyncio.create_task(self._await_done())
        # Separate from `_done`: "the session is over" and "the peer answered our close" are
        # different events, and conflating them is what made the close abnormal.
        self._peer_close: asyncio.Future[None] = asyncio.get_running_loop().create_future()

    async def _await_done(self) -> None:
        await self._done

    async def await_peer_close(self, timeout: float = CLOSE_GRACE_S) -> bool:
        """Keep reading until the peer answers our close frame, or the grace elapses.

        Returns whether the peer ANSWERED. The caller needs that to decide whether a graceful
        teardown is still worth attempting: against a peer that has gone quiet there is nothing
        left to flush, and asking the OS to try can block for a TCP timeout.

        WITHOUT THIS the engine records 1006 for a close we sent a perfectly good 4000 for.
        Sending a close frame and immediately dropping the transport is not a close handshake:
        the engine publishes a book every 50 ms, so there is virtually always unread data in
        our receive queue, and closing a TCP socket with unread data queued makes the kernel
        send RST rather than FIN. The RST discards whatever is in flight — including the close
        frame we just wrote — so the engine's read fails ECONNRESET and it records "transport
        death without a close handshake" (session.cpp's generic `if (ec)` arm). An operator
        reading that telemetry is told the client crashed when it in fact stopped deliberately
        and said so, which is precisely the distinction close code 4000 exists to draw.

        Staying in the read loop until the peer's CLOSE arrives drains the receive queue and
        lets the socket close with FIN. This is what RFC 6455 §7.1.1 asks for, and what the
        naive arm gets for free because `websockets.close()` does the handshake internally —
        which is the whole reason only this arm was affected.
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

        An escaping callback exception used to leave the terminal state UNSET, so picows —
        which reacts by closing 1011 — then delivered the next already-buffered frame to the
        same listener, which processed it happily. The exception is recorded (so teardown can
        tell our bug from the peer's bad framing) and re-raised.
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

        Getting this order wrong is what produced three separate findings: the terminal filter
        used to run BEFORE the reserved-bit check, so once we were closing an RSV1 frame went
        unjudged; and the same filter silenced PING, so the arms diverged on liveness in the
        window between our close and the peer's reply.
        """
        # After the peer has closed there is nothing left to answer or to decide.
        if self._close_received:
            return
        # RESERVED BITS, FIRST, UNCONDITIONALLY, FOR EVERY FRAME TYPE. We negotiate no
        # extensions, so RFC 6455 §5.2 makes any set RSV bit a connection failure — control
        # frames included, and regardless of whether we are already closing.
        if frame.rsv1 or frame.rsv2 or frame.rsv3:
            self._abort(transport, "reserved bit set with no extension negotiated")
            return
        # CONTROL FRAMES STAY LEGAL until the peer closes — §5.4 permits them mid-fragment, and
        # a PING arriving after our own close still deserves its PONG.
        if frame.msg_type in _CONTROL_FRAMES:
            self._on_control(transport, frame)
            return
        # DATA stops the moment we have decided to close. Two conditions, not one: `end_data`
        # covers the operator-stop path, where a close goes out with no strategy verdict behind
        # it at all.
        if self._data_ended or self._driver.close_intent is not None:
            return
        self._on_data(transport, frame)

    def _on_control(self, transport: picows.WSTransport, frame: picows.WSFrame) -> None:
        """CLOSE, PING and PONG — legal at any time, including mid-fragment (§5.4)."""
        if frame.msg_type is picows.WSMsgType.CLOSE:
            self._on_peer_close(transport, frame)
        elif frame.msg_type is picows.WSMsgType.PING:
            # PONGED BY HAND, which is why `enable_auto_pong` is off. The library answers pings
            # BENEATH this callback, so an auto-pong would reply to a PING carrying a reserved
            # bit before we ever saw it — the one case the RSV check above could not reach.
            transport.send_pong(frame.get_payload_as_bytes())
        # An unsolicited PONG is legal and has nothing to answer: we send no pings.

    def _on_data(self, transport: picows.WSTransport, frame: picows.WSFrame) -> None:
        if frame.msg_type is picows.WSMsgType.BINARY:
            # 1002, not a skip. The wire is text-only (JSON v1) and the engine closes 1002
            # on a binary frame from us, so the rule is symmetric. Skipping was worse than
            # wrong: a fragmented message OPENS with its type frame, so dropping that one
            # left the CONTINUATIONs behind it reassembling into a message with no head —
            # which then reached the decoder as garbage and closed for the wrong reason.
            self._abort(transport, "binary frame: the mm.v1 wire is text-only")
            return
        payload = self._reassemble(transport, frame)
        if payload is None:
            return
        # UTF-8 IS VALIDATED HERE, because picows does not do it for us and the code depends on
        # it. A TEXT frame whose bytes are not valid UTF-8 is RFC 6455 §8.1's 1007 — which is
        # what the engine answers with (Beast's `close_code::bad_payload`) and what `websockets`
        # produces on the naive arm, since it decodes to `str` and rejects the frame itself.
        # Without this the bytes reached the JSON decoder, failed there, and were latched 1002:
        # the same wire fault answered with two different codes depending on which arm was under
        # measurement, which is a divergence between the two §6 arms and not merely a wrong code.
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

        Classification lives in :func:`classify_close_body` so the naive arm and this one share
        one table. Two edges that used to collapse here: picows reports BOTH an empty body and
        an encoded status 0 as `NO_INFO = 0`, so the body length — not the enum — decides; and
        1014 is accepted by both Python libraries but reserved in this Beast, so the shared
        table excludes it and we answer 1002 rather than echo a status the engine rejects.
        """
        # Any partial message dies with the close: a continuation arriving behind it has
        # nothing legitimate to attach to. (F-D: control frames are dispatched BEFORE fragment
        # state, so a CLOSE mid-fragment is handled here rather than as "incomplete fragment".)
        self._fragments.clear()
        self._fragment_bytes = 0
        self._opened = False
        body = frame.get_payload_as_bytes() or b""
        code, legal = classify_close_body(body)
        if not legal:
            # OUR VERDICT, and it has to be latched as one. Answering an illegal code with 1002
            # while still reporting the session as "the peer went away" made the reconnect
            # policy dial a peer whose close handling is broken — which will be just as broken
            # on the next connection.
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

        Deliberately does NOT `transport.disconnect()`: `run_client`'s exit path drains toward
        the peer's reply first, so an abort lands as the code we chose rather than as the 1006
        an immediate teardown produces. See `await_peer_close`.
        """
        self._fragments.clear()
        self._fragment_bytes = 0
        self._opened = False
        self._driver.note_protocol_error(reason, code)
        _close(transport, self._driver)
        self.finish()

    def _reassemble(self, transport: picows.WSTransport, frame: picows.WSFrame) -> bytes | None:
        """Fragment reassembly, with the RFC 6455 §5.4 sequencing rules picows leaves to us.

        `websockets` enforces all of this inside the library, which is why only this arm needed
        it written out — and why the two arms disagreed on every malformed-fragmentation case
        until it was. Three rules, each of which a peer can violate:

        * a CONTINUATION with no message open is an orphan (we were treating a FIN
          continuation as a complete message and quoting on its contents);
        * a new TEXT frame while a message is open interleaves two messages;
        * the cap applies to the RUNNING TOTAL, because a per-frame limit is not a bound at
          all — unlimited 1 KiB continuations reassemble into an unlimited message.

        The size breach closes 1009, not 1002: the message is well-formed and too large, which
        is the distinction the two codes exist to draw, and is what a single oversized frame
        already yields from both libraries.
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
        # Delegates, so a callback arriving after `run_client` already finalized is a no-op
        # rather than a second `on_disconnect()` against whatever session exists by then.
        self.finalize()

    @property
    def callback_error(self) -> BaseException | None:
        """An exception one of OUR callbacks raised, if any. Read by the teardown path so a
        bug in this module is never reported as a peer framing fault."""
        return self._callback_error

    def end_data(self) -> None:
        """Stop letting DATA frames drive the strategy; control frames stay legal.

        Called before any close frame goes out, including on the ordinary operator-stop path.
        That path used to close WITHOUT setting this, so a book arriving during the one-second
        peer-close grace was decoded, quoted on, and its orders written to a socket whose close
        frame had already been sent.
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

        `on_ws_disconnected` is picows' callback and may arrive LATE — after `run_client` has
        already given up waiting and returned. By then the caller's retry can have built a new
        session on the same Strategy, and a late `on_disconnect()` would wipe the NEW session's
        orders. Running it here before returning, and making the callback a no-op afterwards,
        removes the ordering question entirely.
        """
        if self._finalized:
            return
        self._finalized = True
        self._driver.on_disconnect()
        self._note_peer_close()
        self.finish()


def _apply_tcp_nodelay(transport: picows.WSTransport) -> None:
    """Disable Nagle's algorithm on the client socket connection."""
    raw_sock = transport.underlying_transport.get_extra_info("socket")
    if raw_sock is not None:
        raw_sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)


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

    Returns WHY it ended, because the caller's reconnect policy is a different decision from
    the close code and cannot be derived from a bare return.
    """
    listener: _Listener | None = None

    def factory() -> picows.WSListener:
        nonlocal listener
        listener = _Listener(strategy, codec, on_sample)
        return listener

    transport, _ = await picows.ws_connect(
        factory,
        url,
        # OFF, so RSV validation sees every frame. The library would otherwise answer a
        # PING beneath this listener — including one carrying a reserved bit. `_Listener`
        # ponges by hand instead, and the hardening suite still pins that a server ping is
        # answered, because "we do it ourselves" is no more self-evident than "the library
        # does it" was.
        enable_auto_pong=False,
        max_frame_size=MAX_MSG_BYTES,
        extra_headers={"Sec-WebSocket-Protocol": SUBPROTOCOL},
    )
    assert listener is not None

    _apply_tcp_nodelay(transport)

    # The try opens HERE, before the handshake check. `_assert_handshake` raises on a server
    # that upgraded us without accepting `mm.v1`, and that raise used to escape with the
    # transport still open and the listener's task still pending — so `app.py`'s retry dialled
    # a second connection while the refused one was still driving the SAME Strategy object.
    # Two live sessions mutating one strategy is a worse failure than the handshake fault.
    timer: asyncio.Task[None] | None = None
    stopper: asyncio.Task[bool] | None = None
    stopped = False
    try:
        _assert_handshake(transport)
        timer = asyncio.create_task(_timer(transport, listener, stop, timer_interval_s))
        stopper = asyncio.create_task(stop.wait())
        # The TIMER is in the wait set, and every completed task's result is inspected. Left
        # out, a codec or send failure inside it died in a task nobody looked at while
        # `run_client` went on waiting for a session that had already stopped ticking.
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
        # DATA OFF BEFORE THE CLOSE FRAME. Without this the ordinary stop path sent a close and
        # then spent the peer-close grace still decoding books and emitting orders down a socket
        # it had already said goodbye on.
        listener.end_data()
        _close(transport, listener.driver)
        # Drain toward the peer's close reply BEFORE tearing the transport down; dropping it
        # with data still queued turns our deliberate 4000 into an abnormal 1006 at the
        # engine. `await_peer_close` explains the mechanism in full.
        answered = False
        try:
            answered = await listener.await_peer_close()
        finally:
            # NESTED, so the transport is torn down even if the drain is cancelled. A cancelled
            # cleanup that skips the disconnect leaks the connection.
            await _teardown(transport, listener, graceful=answered)
    # AFTER teardown, deliberately. Computed inside the `try` this froze the classification
    # before the teardown could latch a parser verdict: an invalid-UTF-8 CLOSE makes picows
    # send 1007, and a frozen PEER_GONE told app.py to retry a fault that is deterministic.
    return _ending(listener.driver, stopped=stopped)


async def _teardown(transport: picows.WSTransport, listener: _Listener, *, graceful: bool) -> None:
    """Close the transport and make sure the local end-of-connection work has happened.

    A peer that did NOT answer gets no graceful drain: `disconnect(graceful=True)` asks the OS
    to flush and `wait_disconnected()` waits for it, and against a peer that has stopped
    acknowledging both can block until the kernel's TCP timeout — minutes in which the client
    neither trades nor exits. Once the grace has expired the frame is already lost, so there is
    nothing to flush and an abrupt close is the honest move.

    `finalize()` runs HERE rather than being left to picows' `on_ws_disconnected`, because that
    callback can arrive after we have returned — by which time a retry may have built a new
    session on the same Strategy, and a late `on_disconnect()` would wipe the new session's
    orders instead of the dead one's.
    """
    transport.disconnect(graceful=graceful)
    try:
        await asyncio.wait_for(transport.wait_disconnected(), CLOSE_GRACE_S)
    except TimeoutError:
        transport.disconnect(graceful=False)
    except picows.WSProtocolError as exc:
        # PROVENANCE FIRST. picows transfers exceptions from OUR callbacks through the same
        # channel as its own parser errors, so classifying every WSProtocolError as "the peer's
        # framing is bad" swallowed bugs in this module — a callback raising WSProtocolError
        # was reported as a peer fault and suppressed entirely.
        if listener.callback_error is not None:
            raise
        # A genuine parser verdict about the peer. Latched, it reports as DECIDED — terminal,
        # because dialling again asks a peer with a broken framer the same question.
        listener.driver.note_protocol_error(f"peer framing rejected by the parser: {exc}")
    finally:
        listener.finalize()


def _ending(driver: SessionDriver, *, stopped: bool) -> Ending:
    """Why the connection ended.

    A LATCHED INTENT WINS over the stop flag, and the order matters: the timer can latch 4000
    and be part-way through sending its cancels when the operator hits Ctrl-C, and reporting
    STOPPED there would describe a session that closed 4000 on the wire as an ordinary
    shutdown. The wire and the return value must agree on why the session ended.
    """
    if driver.close_intent is not None:
        return Ending.DECIDED
    return Ending.STOPPED if stopped else Ending.PEER_GONE


def _assert_handshake(transport: picows.WSTransport) -> None:
    """The same fluency check the naive arm makes, against picows' response object: a run
    that did not negotiate `mm.v1`, or that negotiated an extension, is not the run being
    measured."""
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
        # WAITS on stop rather than sleeping through it: a plain sleep makes shutdown pay a
        # full interval it has no reason to, and leaves the loop's own exit unreachable in a
        # test because the task is always cancelled before its condition is re-read.
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
    # Shared table: never emit a status we would refuse to echo. A latched intent that somehow
    # carried 1014 or 0 would otherwise leave through a library that accepts it, while the
    # naive arm's websockets rejects 0 and Beast rejects both.
    if not is_sendable_close_code(code):
        code = CLOSE_PROTOCOL
    # WSCloseCode is an IntEnum and accepts the private range, which is what lets 4000 —
    # the stale-stop code that distinguishes a strategy decision from a transport fault —
    # go out through a library enum that has no member for it.
    transport.send_close(picows.WSCloseCode(code), reason[:120].encode())
