#!/bin/sh
# Refresh vendor/tree-sitter from an upstream tag.
#
# The runtime is vendored because the Python package builds it: a wheel is
# compiled from an sdist, an sdist cannot fetch anything, and the database
# needs a runtime to walk a tree. CMake uses the same copy, so the two builds
# compile identical source.
#
#   scripts/vendor-tree-sitter.sh             # the pinned tag, cloned
#   scripts/vendor-tree-sitter.sh v0.27.1     # a different tag
#   scripts/vendor-tree-sitter.sh v0.27.0 /path/to/tree-sitter   # a checkout
#
# Only what lib/src/lib.c pulls in is copied -- the runtime's Rust, WASM and
# editor bindings are no part of this. The tag is written to
# vendor/tree-sitter/VERSION, which is where CMake reads it, so nothing else
# records the version and nothing else has to be edited. Run the test suite
# afterwards.

set -eu

VERSION="${1:-v0.27.0}"
SOURCE="${2:-}"
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
TARGET="$ROOT/vendor/tree-sitter"

CLONED=""
if [ -z "$SOURCE" ]; then
    SOURCE=$(mktemp -d)
    CLONED="$SOURCE"
    git clone --depth 1 --branch "$VERSION" https://github.com/tree-sitter/tree-sitter.git "$SOURCE"
fi

if [ ! -f "$SOURCE/lib/src/lib.c" ]; then
    echo "$0: $SOURCE does not look like a tree-sitter checkout" >&2
    exit 1
fi

rm -rf "$TARGET"
mkdir -p "$TARGET/include/tree_sitter" "$TARGET/src/portable" "$TARGET/src/unicode"

cp "$SOURCE/LICENSE" "$TARGET/LICENSE"
cp "$SOURCE/lib/include/tree_sitter/api.h" "$TARGET/include/tree_sitter/"
cp "$SOURCE"/lib/src/*.c "$SOURCE"/lib/src/*.h "$TARGET/src/"
cp "$SOURCE"/lib/src/portable/*.h "$TARGET/src/portable/"
cp "$SOURCE"/lib/src/unicode/* "$TARGET/src/unicode/"
echo "$VERSION" > "$TARGET/VERSION"

[ -n "$CLONED" ] && rm -rf "$CLONED"

echo "vendored tree-sitter $VERSION into vendor/tree-sitter"
