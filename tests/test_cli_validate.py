from __future__ import annotations

from pathlib import Path

from ryspec import main

REPO_ROOT = Path(__file__).resolve().parent.parent


def test_validate_examples_passes():
    assert main(["validate", str(REPO_ROOT / "examples")]) == 0


def test_validate_invalid_fixtures_all_fail_as_expected():
    fixtures_dir = REPO_ROOT / "tests" / "fixtures" / "invalid"
    assert main(["validate", str(fixtures_dir), "--expect-invalid"]) == 0
