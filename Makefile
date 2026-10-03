# Shortcuts for the commands people type. CMakeLists.txt is the build system
# and the ryspec package is the validator; this adds nothing of its own.
#
#   make            configure and build build/libryspec.a
#   make test       build, then run the C tests with ctest
#   make validate   check the documents that must pass the schema
#   make install    install the ryspec Python package, libryspec binding included
#   make fuzz       hold libryspec's schema check to the JSON Schema, by mutation
#   make clean      drop the build directory
#
# Every variable below is overridable: `make BUILD_TYPE=Debug`,
# `make BUILD_DIR=build-clang CC=clang`, `make validate PYTHON=python3.12`,
# `make install PIP_FLAGS="--user -e"`.

# cmake --build may shell out to make again; its directory chatter says
# nothing here.
MAKEFLAGS += --no-print-directory

CMAKE ?= cmake
CTEST ?= ctest
PYTHON ?= python3
BUILD_DIR ?= build
BUILD_TYPE ?= Release
PIP_FLAGS ?=

# What must validate: every file in these, per data/README.md. An invalid/
# file's verdict is its own header's, so it is not among them.
VALIDATE_PATHS ?= examples data/valid data/malformed data/semantic

.PHONY: all configure build test validate install fuzz clean

all: build

# The cache file stands in for the configure step, so `make` stays quiet on
# every call after the first. Delete the build directory to change BUILD_TYPE.
$(BUILD_DIR)/CMakeCache.txt:
	$(CMAKE) -S . -B $(BUILD_DIR) -DCMAKE_BUILD_TYPE=$(BUILD_TYPE)

configure: $(BUILD_DIR)/CMakeCache.txt

build: configure
	$(CMAKE) --build $(BUILD_DIR)

test: build
	$(CTEST) --test-dir $(BUILD_DIR) --output-on-failure

# Runs the validator from the checkout, so it needs jsonschema and tomli but
# not an installed package, and checks against schemas/v0/ as edited.
validate:
	PYTHONPATH=python/src $(PYTHON) -m ryspec validate -q $(VALIDATE_PATHS)

# pip builds the package through scikit-build-core, which runs CMakeLists.txt
# in a build tree of its own, so this neither uses nor disturbs the one above.
# PIP_FLAGS goes right before the path: `-e` there makes it editable.
install:
	$(PYTHON) -m pip install $(PIP_FLAGS) .

# Needs the package as installed, its extension included: `make install`
# first, after any change to libryspec.
fuzz:
	$(PYTHON) python/tools/schema_fuzz.py

clean:
	rm -rf $(BUILD_DIR)
