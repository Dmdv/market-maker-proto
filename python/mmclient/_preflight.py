"""Frame byte policy and RFC 8259 grammar scan — the Python twin of ``frame_preflight.cpp``.

Two bounds, both the C++ side's: the transport cap and :data:`_MAX_NESTING_DEPTH`, checked on
container entry. No token or escape budget — one would reject frames the engine accepts.
"""

import json
import re
from typing import Final

Frame = bytes | bytearray
"""One inbound message; ``bytearray`` is the same bytes, while a ``str`` has already lost the
byte policy. CALLER OBLIGATION: no mutation or aliasing during the call — nothing here copies."""

_UTF8_BOM: Final = b"\xef\xbb\xbf"
_WS: Final = b" \t\n\r"
_MAX_NESTING_DEPTH: Final = 32  # mirrors detail::kMaxNestingDepth
_MAX_TOP_LEVEL_KEYS: Final = 32  # mirrors detail::kMaxTopLevelKeys
_MAX_TAG_DETAIL: Final = 32  # mirrors detail::kMaxTagDetailBytes

# The transport's own cap, restated at the codec so a decoder reached by any other route
# inherits it. The C++ `frame_preflight` has no size test either, for exactly this reason.
_MAX_FRAME_BYTES: Final = 64 * 1024

# Unicode D91 surrogate halves; `_UTF8_TAIL` is the continuation-byte range RFC 3629 reserves —
# C++'s `(b & 0xC0) == 0x80` test, and what :func:`_sanitized_tag` walks back over.
_HIGH_SURROGATE: Final = range(0xD800, 0xDC00)
_LOW_SURROGATE: Final = range(0xDC00, 0xE000)
_UTF8_TAIL: Final = range(0x80, 0xC0)

# One RFC 8259 token in the STRICT grammar productions: `07`/`NaN`/`Infinity` fail to tokenize,
# and a number is MATCHED, never converted. POSSESSIVE quantifiers: same language, no backtracking.
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

# One whole member of a FLAT, escape-free object — every v1 frame — in a single C-level step.
# A fast path, never an authority: `_scan` hands anything this cannot match to the general scanner.
_MEMBER: Final = re.compile(
    rb'[ \t\n\r]*("[^"\\\x00-\x1f]*")[ \t\n\r]*:[ \t\n\r]*'
    rb'("[^"\\\x00-\x1f]*"'
    rb"|-?(?:0|[1-9][0-9]*)(?:\.[0-9]+)?(?:[eE][-+]?[0-9]+)?"
    rb"|true|false|null)[ \t\n\r]*([,}])"
)


def _sanitized_tag(tag: str) -> str:
    """Bound and clean peer text before it reaches an error message or a log line.

    Mirrors ``detail::sanitized_tag``: control bytes become ``?``, and the result is cut at
    :data:`_MAX_TAG_DETAIL` BYTES — not characters — on a code-point boundary, marked U+2026.
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

    The last two are ``frame_preflight``'s own first two verdicts, in its order; the type check
    is the one rule with no C++ twin, because C++ has no ``str`` to confuse with a frame.
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

    The interpreter stack is bounded BEFORE it is spent: every container checks the depth cap
    on ENTRY, so a 100k-deep bomb is rejected at level 33 and never recursed into.
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

    The flat-object member loop is an ACCELERATOR, never an authority: it completes only on
    input the general scanner would also accept, and hands over the moment anything is unusual.
    """
    if b"\\" not in frame and frame[:1] == b"{":
        fields: dict[bytes, bytes] = {}
        pos = 1
        match_member = _MEMBER.match  # bound once: this loop runs per wire field
        while True:
            member = match_member(frame, pos)
            if member is None:
                break
            key, value, end = member.group(1, 2, 3)
            name = key[1:-1]
            if name in fields or len(fields) >= _MAX_TOP_LEVEL_KEYS:
                break
            fields[name] = value
            pos = member.end()
            if end == b"}":
                if frame[pos:].strip(_WS):
                    break
                return fields
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
