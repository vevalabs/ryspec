# Shortcuts for the CMake build. CMakeLists.txt is the build system; this is a
# front door for the commands people type, and adds nothing of its own.
#
#   make            configure and build build/libryspec.so and build/ryspec-parse
#   make generate   regenerate src/parser.c from src/grammar.json
#   make test       ctest: the grammar corpus, the data corpus, and the Python suite
#   make wheel      build the Python package into dist/
#   make sdist      build the source distribution into dist/
#   make install    install the library, ryspec-parse and the queries under $(PREFIX)
#   make clean      drop the build directory and dist/
#
# Every variable below is overridable: `make BUILD_TYPE=Debug`, `make install
# PREFIX=$HOME/.local`, `make BUILD_DIR=build-clang CC=clang`.

# cmake --build shells out to make again; the sub-make's directory chatter
# says nothing here.
MAKEFLAGS += --no-print-directory

CMAKE ?= cmake
CTEST ?= ctest
PYTHON ?= python3
BUILD_DIR ?= build
DIST_DIR ?= dist
BUILD_TYPE ?= Release
PREFIX ?= /usr/local

.PHONY: all configure build generate test wheel sdist install clean

all: build

# The cache file stands in for the configure step: reconfiguring a current
# build tree is a no-op, but skipping it keeps `make` quiet on every call
# after the first. Delete the build directory to change BUILD_TYPE.
$(BUILD_DIR)/CMakeCache.txt:
	$(CMAKE) -S . -B $(BUILD_DIR) -DCMAKE_BUILD_TYPE=$(BUILD_TYPE)

configure: $(BUILD_DIR)/CMakeCache.txt

build: configure
	$(CMAKE) --build $(BUILD_DIR)

generate: configure
	$(CMAKE) --build $(BUILD_DIR) --target generate

test: build
	$(CTEST) --test-dir $(BUILD_DIR) --output-on-failure

# The wheel is CMake's too -- py-build-cmake runs CMakeLists.txt with
# RYSPEC_PYTHON_MODULE=ON, in a build tree of its own under build/, so this
# neither uses nor disturbs the one above. It goes through `python -m build`
# rather than pip so the backend runs in the isolated environment it declares.
wheel:
	$(PYTHON) -m build --wheel --outdir $(DIST_DIR) .

# What `pip install` off the sdist compiles, which is a subset of this
# repository: CMakeLists.txt, src/ and schemas/, listed under
# [tool.py-build-cmake.sdist] and not the applications, the corpus or the
# grammar tests. So the honest check of that list is building the sdist and
# then a wheel out of it, which `$(PYTHON) -m build` with neither flag does.
sdist:
	$(PYTHON) -m build --sdist --outdir $(DIST_DIR) .

install: build
	$(CMAKE) --install $(BUILD_DIR) --prefix $(PREFIX)

# build/ holds both trees when BUILD_DIR is the default: the one above, and
# py-build-cmake's cache under build/wheel.
clean:
	rm -rf $(BUILD_DIR) $(DIST_DIR)
