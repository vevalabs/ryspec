from __future__ import annotations

from pathlib import Path

from ryspec import main

REPO_ROOT = Path(__file__).resolve().parents[2]


def test_validate_examples_passes():
    assert main(["validate", str(REPO_ROOT / "examples")]) == 0


def test_validate_invalid_fixtures_all_fail_as_expected():
    fixtures_dir = Path(__file__).resolve().parent / "fixtures" / "invalid"
    assert main(["validate", str(fixtures_dir), "--expect-invalid"]) == 0


def test_validate_data_corpus_valid_half_passes():
    assert main(["validate", str(REPO_ROOT / "data" / "valid")]) == 0


def test_validate_data_corpus_invalid_half_all_fail_as_expected():
    assert main(["validate", str(REPO_ROOT / "data" / "invalid"), "--expect-invalid"]) == 0
