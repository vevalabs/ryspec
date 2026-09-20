# Shortcuts for the CMake build. CMakeLists.txt is the build system; this is a
# front door for the commands people type, and adds nothing of its own.
#
#   make            configure and build build/libryspec.so and build/ryspec-parse
#   make generate   regenerate src/parser.c from src/grammar.json
#   make test       ctest: the grammar corpus, the data corpus, and the Python suite
#   make install    install the library, ryspec-parse and the queries under $(PREFIX)
#   make clean      drop the build directory
#
# Every variable below is overridable: `make BUILD_TYPE=Debug`, `make install
# PREFIX=$HOME/.local`, `make BUILD_DIR=build-clang CC=clang`.

# cmake --build shells out to make again; the sub-make's directory chatter
# says nothing here.
MAKEFLAGS += --no-print-directory

CMAKE ?= cmake
CTEST ?= ctest
BUILD_DIR ?= build
BUILD_TYPE ?= Release
PREFIX ?= /usr/local

.PHONY: all configure build generate test install clean

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

install: build
	$(CMAKE) --install $(BUILD_DIR) --prefix $(PREFIX)

clean:
	rm -rf $(BUILD_DIR)
