"""Cross-language golden contract: the C++ fixtures in ``tests/golden/`` ARE the wire.

Task 2 wrote those files and pins both C++ codec arms against them byte-for-byte. This
module pins the Python side against the same bytes — the same files, read from the repo
root, never a copy — so a wire-format change cannot land on one side of the boundary
alone:

  * every engine->client fixture decodes to the expected Struct via ``decode`` (msgspec)
    AND ``decode_naive`` (stdlib json): the arms must agree on VALUE, not merely on
    acceptance;
  * every client->engine fixture is reproduced byte-for-byte by ``encode`` AND
    ``encode_naive`` — this is the direction Python actually produces, and the test that
    pins C++/Python wire compatibility;
  * every engine->client fixture is reproduced byte-for-byte from its Struct too, which
    is what proves msgspec's field emission order equals the C++ member DECLARATION
    order (``cpp/include/mm/protocol.hpp``: declaration order IS wire key order);
  * the fixture directory is enumerated, so a fixture added by a later task cannot sit
    unchecked on the Python side.
"""

import json
import re
from pathlib import Path

import msgspec
import pytest
from golden_support import (
    ALL_FIXTURES,
    CLIENT_FIXTURES,
    ENGINE_FIXTURES,
    GOLDEN_DIR,
    read_fixture,
)

from mmclient.protocol import ClientMsg, decode, decode_naive, encode, encode_naive

# Test-local codec ends deliberately absent from the production module: the client never
# DECODES its own commands and never ENCODES engine reports, and widening either
# production surface for a test's benefit would enlarge the untrusted-input attack
# surface (decode) or ship dead code (encode). The Structs under test are production.
_decode_client = msgspec.json.Decoder(ClientMsg).decode
_encode_any = msgspec.json.Encoder().encode


@pytest.mark.parametrize("name", sorted(ENGINE_FIXTURES))
def test_engine_fixture_decodes_through_both_arms(name: str) -> None:
    raw = read_fixture(name)
    expected = ENGINE_FIXTURES[name]
    assert decode(raw) == expected
    assert decode_naive(raw) == expected
    # Same concrete Struct type, not merely an equal value.
    assert type(decode(raw)) is type(expected)
    assert type(decode_naive(raw)) is type(expected)


@pytest.mark.parametrize("name", sorted(CLIENT_FIXTURES))
def test_client_fixture_bytes_are_reproduced_exactly(name: str) -> None:
    """Byte identity in the direction Python produces: both arms emit the engine's form."""
    raw = read_fixture(name)
    msg = CLIENT_FIXTURES[name]
    assert encode(msg) == raw
    assert encode_naive(msg) == raw


@pytest.mark.parametrize("name", sorted(ENGINE_FIXTURES))
def test_engine_struct_declaration_order_matches_the_fixture(name: str) -> None:
    """The Python Structs must declare members in the C++ members' order."""
    assert _encode_any(ENGINE_FIXTURES[name]) == read_fixture(name)


@pytest.mark.parametrize("name", sorted(ALL_FIXTURES))
def test_fixture_key_order_is_tag_then_declaration_order(name: str) -> None:
    raw = read_fixture(name)
    keys = list(json.loads(raw))
    assert keys[0] == "t"
    assert keys == list(json.loads(_encode_any(ALL_FIXTURES[name])))


@pytest.mark.parametrize("name", sorted(CLIENT_FIXTURES))
def test_client_fixture_round_trips(name: str) -> None:
    msg = CLIENT_FIXTURES[name]
    assert _decode_client(encode(msg)) == msg
    assert _decode_client(encode_naive(msg)) == msg
    assert _decode_client(read_fixture(name)) == msg


@pytest.mark.parametrize("name", sorted(ENGINE_FIXTURES))
def test_engine_fixture_round_trips(name: str) -> None:
    msg = ENGINE_FIXTURES[name]
    blob = _encode_any(msg)
    assert decode(blob) == msg
    assert decode_naive(blob) == msg


def test_every_golden_fixture_is_covered() -> None:
    """A fixture added by a later task must not sit unchecked on the Python side."""
    on_disk = {p.stem for p in GOLDEN_DIR.glob("*.json")}
    # The count is asserted first: an equality between two empty sets is also true, and
    # an empty ALL_FIXTURES would silently collect ZERO parametrized cases above.
    assert len(ALL_FIXTURES) == 7
    assert on_disk == set(ALL_FIXTURES)


def test_golden_dir_is_the_directory_the_cpp_suite_reads() -> None:
    """The Python suite must read the SAME files the C++ suite reads, not a copy.

    Asserted against the C++ side's OWN definition — ``CMakeLists.txt``'s
    ``MM_GOLDEN_DIR`` compile definition, which is what ``codec_test_support.hpp`` opens
    the fixtures through. The previous form recomputed ``GOLDEN_DIR``'s own defining
    expression (``__file__.parents[2] / "tests" / "golden"``) and compared it to
    ``GOLDEN_DIR``: identical by construction, true whatever either side did, and never
    touching the C++ path at all. Now a fixture directory moved on either side of the
    boundary fails this test, which is what its docstring always promised.
    """
    repo_root = Path(__file__).resolve().parents[2]
    cmake = (repo_root / "CMakeLists.txt").read_text(encoding="utf-8")
    declared = re.search(r'MM_GOLDEN_DIR="\$\{CMAKE_CURRENT_SOURCE_DIR\}/([^"]+)"', cmake)
    assert declared is not None, "CMakeLists.txt no longer defines MM_GOLDEN_DIR"
    assert declared.group(1) == GOLDEN_DIR.relative_to(repo_root).as_posix()
    assert (GOLDEN_DIR / "tob.json").is_file()
