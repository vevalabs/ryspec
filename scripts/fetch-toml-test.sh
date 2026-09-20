#!/bin/sh
# Fetch the standard TOML test suite, which python/tests/test_toml_conformance.py runs
# the ryspec parser against. The suite is not vendored: it is 4 MB of other
# people's fixtures, and the conformance test skips when it is absent.
set -eu

REPO="https://github.com/toml-lang/toml-test.git"
DEST="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)/python/tests/vendor/toml-test"

if [ -d "$DEST/.git" ]; then
    git -C "$DEST" pull --ff-only
else
    mkdir -p "$(dirname "$DEST")"
    git clone --depth 1 "$REPO" "$DEST"
fi

echo "toml-test is at $DEST"
