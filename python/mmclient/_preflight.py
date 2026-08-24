"""Frame byte policy and RFC 8259 grammar scan — the Python twin of ``frame_preflight.cpp``.

Split out of ``protocol.py`` at the Task 4 gate (the repo's files-under-500-lines
convention, enforced twice already by splitting C++ files mid-gate). The seam is the one
the C++ side already uses: `cpp/src/frame_preflight.cpp` owns the byte policy, the
scanner and the root key policy, while ``cpp/include/mm/protocol.hpp`` owns the messages.
Everything here decides a frame's fate WITHOUT knowing which message it is; the schemas,
the derived rule tables and the four public codec functions stay in ``protocol.py``.

Nothing here is public API: the module is private, and ``protocol.py`` re-exports the one
name a caller needs (:data:`Frame`).

Resource discipline — two bounds, and both of them are the C++ side's:

* :data:`_MAX_FRAME_BYTES` restates the transport cap at the codec, so a decoder reached
  by any other route inherits the bound the engine's transport already enforces;
* :data:`_MAX_NESTING_DEPTH` is checked on entry to every container, so a 100k-deep bomb
  is refused at level 33 and never approaches the interpreter's stack limit.

There is deliberately NO scan-token, token-length or escape budget. This module had all
three until the Task-4 gate, and they made the client REJECT grammar-valid frames that
``cpp/src/frame_preflight.cpp`` accepts — an accept-set divergence in the one direction
that breaks additive forward compatibility, since a future member the engine ignores can
be arbitrarily long or token-dense and would have been refused here. Cross-language parity
is the contract; the pure-Python decode cost those budgets bought is not (the measurement
they were justified by, and the reasoning for their removal, are in
``docs/PENDING_AMENDMENTS`` (p)7).
"""

import json
import re
from typing import Final

Frame = bytes | bytearray
"""One inbound WebSocket message, as the transports hand it over.

``bytearray`` is the same bytes, not a contract violation — a WS library that reassembles
fragments into a mutable buffer must not have to copy it to be decoded. ``str`` and
``memoryview`` are deliberately NOT here: a ``str`` has already lost the byte policy this
module enforces (BOM, UTF-8 validity), and a ``memoryview`` would slip past the guard into
whichever library the arm wraps.

CALLER OBLIGATION — no mutation, no aliasing for the duration of the call. A frame is read
more than once (the scan, then the arm's library parse, and a third time on the reject
path when :func:`protocol._schema_error` re-reads it), and nothing here copies it. Those
reads are not separated by an ``await`` today, but that is a property of the current call
graph rather than a guarantee: a transport whose library reuses ONE receive buffer across
messages must hand over a copy (``bytes(buf)``), not the buffer. Stated rather than
enforced with a defensive copy because the copy would land on the measured §6 path, which
is under separate cost scrutiny; Task 9's picows adapter is where the obligation binds.
"""

_UTF8_BOM: Final = b"\xef\xbb\xbf"
_WS: Final = b" \t\n\r"
_MAX_NESTING_DEPTH: Final = 32  # mirrors detail::kMaxNestingDepth
_MAX_TOP_LEVEL_KEYS: Final = 32  # mirrors detail::kMaxTopLevelKeys
_MAX_TAG_DETAIL: Final = 32  # mirrors detail::kMaxTagDetailBytes

# One byte bound, and it is the transport's own: `_MAX_FRAME_BYTES` restates the cap the
# engine already enforces (`read_message_max` / picows `max_frame_size`, Global
# Constraints) at the codec, so a decoder reached by any other route inherits it. The C++
# side bounds a frame the same way and in the same place — its `frame_preflight` has no
# size test either, because the transport has already refused anything larger.
#
# WHAT IS NOT HERE, and why the cost it used to buy is the price of parity: a scan-token
# budget, a per-token byte charge and an escape cap were all removed at the Task-4 gate as
# accept-set divergences (module docstring; PENDING_AMENDMENTS (p)7). Inside the 64 KiB cap
# the scan is linear in frame length with a per-escape and per-token constant, and the most
# expensive ACCEPTED frame is measured rather than asserted (`bench/probes/py_codec.py`,
# min of 7 x N, which prints each shape's escape/token/byte load next to its timing).
_MAX_FRAME_BYTES: Final = 64 * 1024

# Unicode D91 surrogate halves, named because a bare 0xDC00 in a comparison says nothing
# about which half of a pair it bounds. `_UTF8_TAIL` is the same idea one layer down: the
# byte values RFC 3629 reserves for a continuation, which is what C++'s `(b & 0xC0) == 0x80`
# test spells and what :func:`_sanitized_tag` walks back over to keep its cut on a boundary.
_HIGH_SURROGATE: Final = range(0xD800, 0xDC00)
_LOW_SURROGATE: Final = range(0xDC00, 0xE000)
_UTF8_TAIL: Final = range(0x80, 0xC0)

# One RFC 8259 token, whitespace-prefixed. Every alternative is the STRICT grammar
# production, so `07`, `+7`, `7.`, `.7`, `0x7`, `NaN` and `Infinity` fail to tokenize
# rather than reaching a library with its own opinion. A number is MATCHED, never
# converted: magnitude cannot decide acceptance.
#
# The string production's ordinary-character run is `++`, not the per-character `|` an
# earlier revision wrote: `re` steps its VM once per alternation, so a 64 KiB string
# token cost ~2.0 ms to MATCH (~330x the canonical frame) where the run form costs
# ~0.25 ms. That 8x is load-bearing now that no budget backstops it, and it is the reason
# a 64 KiB frame stays affordable at all. The quantifiers are POSSESSIVE because the
# nested `(A+|B)*` shape is the classic catastrophic-backtracking figure otherwise — an
# unterminated 20 KiB string would explore every partition of its character run. The
# accepted language is unchanged: `(A|B)*` and `(A++|B)*+` match the same strings, pinned
# by a 400k-case random differential fuzz plus an exhaustive sweep of every byte string
# up to length 5 over an 8-symbol alphabet (0 differences; `bench/probes/py_codec.py`).
_TOKEN: Final = re.compile(
    rb"[ \t\n\r]*(?:"
    rb'(?P<str>"(?:[^"\\\x00-\x1f]++|\\(?:["\\/bfnrt]|u[0-9a-fA-F]{4}))*+")'
    rb"|(?P<num>-?(?:0|[1-9][0-9]*)(?:\.[0-9]+)?(?:[eE][-+]?[0-9]+)?)"
    rb"|(?P<lit>true|false|null)"
    rb"|(?P<punct>[{}\[\]:,])"
    rb")"
)
# Escapes, left to right: `\\` matches the `\.` branch and consumes both bytes, so a
# literal backslash can never be mistaken for the start of a `\u` escape.
_ESCAPE: Final = re.compile(rb"\\(?:u([0-9a-fA-F]{4})|.)")

# One whole member of a FLAT, escape-free object — the shape of every v1 frame — matched
# in a single C-level step. It is a fast path, never an authority: `_scan` hands anything
# this cannot match in full to the general scanner, which decides. The three groups are
# read positionally by that loop and are deliberately UNNAMED: names no reader uses go
# stale silently, and the positional form is the faster of the two.
_MEMBER: Final = re.compile(
    rb'[ \t\n\r]*"([^"\\\x00-\x1f]*)"[ \t\n\r]*:[ \t\n\r]*'
    rb'("[^"\\\x00-\x1f]*"'
    rb"|-?(?:0|[1-9][0-9]*)(?:\.[0-9]+)?(?:[eE][-+]?[0-9]+)?"
    rb"|true|false|null)[ \t\n\r]*([,}])"
)


def _sanitized_tag(tag: str) -> str:
    """Bound and clean peer text before it reaches an error message or a log line.

    Mirrors ``detail::sanitized_tag`` rule for rule: control bytes (< 0x20, 0x7F) become
    ``?``, and the result is cut at :data:`_MAX_TAG_DETAIL` BYTES on a code-point boundary
    with the cut marked by U+2026. An unbounded or control-carrying tag in a log record is
    CWE-117 log forgery plus CWE-400 amplification, and the bound C++ states is a BYTE one.

    Bounding in CHARACTERS — what this did until the Task-4 gate, on the ground that Python
    has already decoded the frame — is a DIFFERENT rule, not a translation of the same one:
    ``"é" * 17`` is 17 characters and 34 bytes, so it came back untruncated here while C++
    cut it to 32 bytes, diverging the reject wording and dropping the amplification bound.

    Replacing control bytes BEFORE the cut is not a reordering of the C++ steps: every byte
    it replaces is a one-byte code point replaced by another, so no byte offset moves.
    """
    clean = "".join("?" if ch < " " or ch == "\x7f" else ch for ch in tag)
    raw = clean.encode()
    if len(raw) <= _MAX_TAG_DETAIL:
        return clean
    take = _MAX_TAG_DETAIL
    while take > 0 and raw[take] in _UTF8_TAIL:  # never cut a sequence in half
        take -= 1
    return raw[:take].decode() + "…"


def _check_frame_bytes(data: Frame) -> None:
    """Byte policy: a frame is exact bytes, within the transport cap, BOM-free and UTF-8.

    The last two are ``frame_preflight``'s own first two verdicts, in its order; the cap is
    the transport's, restated here (:data:`_MAX_FRAME_BYTES`), and the type check is the one
    rule with no C++ twin because C++ has no ``str`` to confuse with a frame.
    """
    if not isinstance(data, (bytes, bytearray)):
        raise TypeError("frames are bytes: a str would bypass the BOM/UTF-8 byte policy")
    if len(data) > _MAX_FRAME_BYTES:
        raise ValueError(f"malformed: frame exceeds the {_MAX_FRAME_BYTES}-byte cap")
    if data[:3] == _UTF8_BOM:
        raise ValueError("malformed: leading UTF-8 BOM")
    try:
        data.decode()  # rejects overlongs, encoded surrogates and truncations
    except UnicodeDecodeError as exc:
        raise ValueError(f"malformed: {exc}") from exc


def _check_escapes(token: bytes) -> None:
    """Escaped surrogates must form ADJACENT pairs — the verdict both C++ arms share."""
    pending = -1  # end offset of a high surrogate still awaiting its low half
    for match in _ESCAPE.finditer(token):
        digits = match.group(1)
        code = int(digits, 16) if digits else -1
        if pending >= 0:
            if match.start() != pending or code not in _LOW_SURROGATE:
                raise ValueError("malformed: unpaired high surrogate escape")
            pending = -1
        elif code in _LOW_SURROGATE:
            raise ValueError("malformed: unpaired low surrogate escape")
        elif code in _HIGH_SURROGATE:
            pending = match.end()
    if pending >= 0:
        raise ValueError("malformed: unpaired high surrogate escape")


def _record(top: dict[bytes, bytes], key: bytes, value: bytes) -> None:
    """Top-level key policy (codec.hpp): literal names, no duplicates, bounded count."""
    if b"\\" in key:
        raise ValueError("malformed: escape sequence in a top-level member name")
    name = key[1:-1]  # unquoted: an escape-free key is its own literal spelling
    if name in top:
        raise ValueError(f"malformed: duplicate top-level key {_sanitized_tag(name.decode())}")
    if len(top) == _MAX_TOP_LEVEL_KEYS:
        raise ValueError("malformed: too many top-level keys")
    top[name] = value


class _Scan:
    """RFC 8259 grammar scan over one frame, recording the root object's value spans.

    The one resource this scan can consume without a bound is the interpreter stack, and it
    is bounded BEFORE it is spent: every container checks the depth cap on ENTRY, so a
    100k-deep bomb is rejected at level 33 and never recursed into (assignment §3:
    malformed input is rejected, never fatal). There is deliberately no WORK budget on top
    of that — ``frame_preflight.cpp`` has none, and one here would reject frames the engine
    accepts (module docstring).
    """

    __slots__ = ("data", "pos")

    def __init__(self, data: Frame) -> None:
        self.data = data
        self.pos = 0

    def token(self) -> tuple[str, bytes, int]:
        """Next token as ``(kind, text, start)``; anything else is malformed."""
        match = _TOKEN.match(self.data, self.pos)
        if match is None:
            raise ValueError(f"malformed: invalid JSON at byte {self.pos}")
        kind = match.lastgroup
        if kind is None:  # pragma: no cover - every _TOKEN alternative is a named group
            raise ValueError(f"malformed: invalid JSON at byte {self.pos}")
        start, self.pos = match.span(kind)
        text = match.group(kind)
        if kind == "str" and b"\\" in text:
            _check_escapes(text)
        return kind, text, start

    def value(self, kind: str, text: bytes, depth: int) -> None:
        """Consume the remainder of a value whose first token has already been read."""
        if kind != "punct":
            return
        if text == b"{":
            self.members(depth, None)
        elif text == b"[":
            self.elements(depth)
        else:
            raise ValueError(f"malformed: expected a value, found {text.decode()!r}")

    def members(self, depth: int, top: dict[bytes, bytes] | None) -> None:
        """An object; a non-None ``top`` collects the ROOT's key policy and value spans."""
        self._enter(depth)
        kind, text, _ = self.token()
        if kind == "punct" and text == b"}":
            return
        while True:
            if kind != "str":
                raise ValueError("malformed: object member name must be a string")
            key = text
            self._expect(b":")
            kind, text, start = self.token()
            self.value(kind, text, depth + 1)
            if top is not None:
                _record(top, key, bytes(self.data[start : self.pos]))
            if self._closed(b"}"):
                return
            kind, text, _ = self.token()

    def elements(self, depth: int) -> None:
        self._enter(depth)
        kind, text, _ = self.token()
        if kind == "punct" and text == b"]":
            return
        while True:
            self.value(kind, text, depth + 1)
            if self._closed(b"]"):
                return
            kind, text, _ = self.token()

    def _closed(self, closer: bytes) -> bool:
        kind, text, _ = self.token()
        if kind == "punct" and text == closer:
            return True
        if kind == "punct" and text == b",":
            return False
        raise ValueError(f"malformed: expected ',' or {closer.decode()!r}")

    def _expect(self, punct: bytes) -> None:
        kind, text, _ = self.token()
        if kind != "punct" or text != punct:
            raise ValueError(f"malformed: expected {punct.decode()!r}")

    @staticmethod
    def _enter(depth: int) -> None:
        if depth > _MAX_NESTING_DEPTH:
            raise ValueError("malformed: nesting depth exceeds the limit")


def _scan(frame: Frame) -> dict[bytes, bytes]:
    """Grammar + key policy over the whole frame; returns the root's value spans.

    A flat, escape-free object — the shape of every v1 message — is walked here by a
    single-regex member loop, because the general scanner below costs ~4x as much per
    frame in pure Python and this preflight sits on the measured §6 path. The loop is an
    ACCELERATOR, never an authority: it only ever completes on input the general scanner
    would also accept, and hands over the moment anything is unusual (an escape anywhere
    in the frame, a nested value, a duplicate or 33rd key, a token it cannot match, junk
    after the closing brace). Rejection messages therefore always come from the general
    scanner, and ``test_preflight.py`` pins the two against each other frame by frame.
    """
    if b"\\" not in frame and frame[:1] == b"{":
        fields: dict[bytes, bytes] = {}
        pos = 1
        match_member = _MEMBER.match  # bound once: this loop runs per wire field
        while True:
            member = match_member(frame, pos)
            if member is None:
                break
            name, value, end = member.group(1, 2, 3)
            if name in fields or len(fields) >= _MAX_TOP_LEVEL_KEYS:
                break
            fields[name] = value
            pos = member.end()
            if end == b"}":
                if pos == len(frame) or not frame[pos:].strip(_WS):
                    return fields
                break
    return _scan_general(frame)


def _scan_general(frame: Frame) -> dict[bytes, bytes]:
    """The scan of record: full RFC 8259 grammar, byte policy and root key policy."""
    scan = _Scan(frame)
    kind, text, _ = scan.token()
    fields: dict[bytes, bytes] = {}
    root_is_object = kind == "punct" and text == b"{"
    if root_is_object:
        scan.members(1, fields)
    else:
        scan.value(kind, text, 1)
    # Grammar first, root type second: a parse error outranks the root-type error.
    if frame[scan.pos :].strip(_WS):
        raise ValueError("malformed: trailing data after the top-level value")
    if not root_is_object:
        raise ValueError("malformed: root is not an object")
    return fields


def _decode_string_span(span: bytes) -> str:
    """Unescape one grammar-validated JSON string token."""
    if b"\\" not in span:
        return span[1:-1].decode()
    text: str = json.loads(span)
    return text
