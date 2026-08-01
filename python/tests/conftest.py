"""Makes the engine fixture visible to the integration suite.

Pytest resolves fixtures by NAME through the conftest chain, not by import: a test that
`import`s the module a fixture lives in still cannot request it. `engine` is defined in
engine_fixture.py — beside the process-lifecycle code it belongs with — and re-exported here so
pytest can find it. The alternative, importing `engine` into every test module that uses it,
looks like an unused import and is one `ruff --fix` away from silently un-registering the
fixture.
"""

import sys
from pathlib import Path

# `bench/` is a sibling of `python/`, so the harness is not on the path pytest builds from the
# rootdir. Added here rather than in each test module: a sys.path edit at module scope is the
# kind of line a later import-sorting pass moves above the import that depends on it.
_BENCH = Path(__file__).resolve().parents[2] / "bench"
if str(_BENCH) not in sys.path:
    sys.path.insert(0, str(_BENCH))

from engine_fixture import live_engine  # noqa: E402

__all__ = ["live_engine"]
