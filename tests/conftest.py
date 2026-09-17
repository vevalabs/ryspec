from __future__ import annotations

import json
import tomllib
from pathlib import Path

import pytest
from jsonschema.validators import validator_for

REPO_ROOT = Path(__file__).resolve().parent.parent
SCHEMA_PATH = REPO_ROOT / "schemas" / "v0" / "ryspec.schema.json"


@pytest.fixture(scope="session")
def validator():
    schema = json.loads(SCHEMA_PATH.read_text())
    validator_cls = validator_for(schema)
    validator_cls.check_schema(schema)
    return validator_cls(schema)


def load_toml(path: Path) -> dict:
    return tomllib.loads(path.read_text())
