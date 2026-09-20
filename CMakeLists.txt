cmake_minimum_required(VERSION 3.25)

project(ryspec
  VERSION 0.1.0
  DESCRIPTION "Grammar and parser for the ryspec specification format"
  HOMEPAGE_URL "https://github.com/vevalabs/ryspec"
  LANGUAGES C)

include(CTest)
include(GNUInstallDirs)

# Building the Python package means running this file: py-build-cmake configures
# and builds it, and takes the wheel's contents from its install rules. That
# build wants one thing out of this project -- the library beside the package,
# with the schema it reads -- so RYSPEC_PYTHON_MODULE says to make that and
# stop: no applications, no tests, no installed headers. It is the only
# difference between the two builds; the C is the same C either way.
option(RYSPEC_PYTHON_MODULE
  "Build only what the Python package holds, and install it into the package" OFF)

find_program(TREE_SITTER_CLI tree-sitter
  DOC "The tree-sitter CLI, needed to regenerate the parser and to run the corpus")

# src/parser.c is generated and src/scanner.c is written by hand; both are
# committed, so nothing has to be generated to build. The runtime below is
# fetched, so a first configure wants the network.
add_library(ryspec SHARED
  src/parser.c
  src/scanner.c)

target_include_directories(ryspec PRIVATE src)

set_target_properties(ryspec PROPERTIES
  C_STANDARD 23
  C_STANDARD_REQUIRED ON
  POSITION_INDEPENDENT_CODE ON
  # Straight into the build directory, which is where the Python test suite
  # and anyone loading the parser by hand expect to find it.
  LIBRARY_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR})

if(NOT RYSPEC_PYTHON_MODULE)
  # libryspec.so -> libryspec.so.0 -> libryspec.so.0.1.0. The soname is the
  # major version, so everything built against 0.x keeps linking until the
  # parser's ABI -- the tree_sitter_ryspec symbol and the node types behind
  # it -- changes shape.
  #
  # The wheel's copy is one plain libryspec.so instead. Nothing links against
  # it: ryspec.semantics opens it by path with ctypes, so a soname would buy a
  # version chain no one reads and two symlinks a wheel has no way to carry.
  set_target_properties(ryspec PROPERTIES
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR})
endif()

# Everything is worth hearing about, except the parameters two files are
# obliged to take and have no use for: generated code, and the external
# scanner, whose four entry points are the runtime's signatures rather than
# ours. The concession is theirs alone, so it sits on those sources rather than
# on the target, which now also carries the database below -- written by hand,
# and held to the full set.
target_compile_options(ryspec PRIVATE
  $<$<C_COMPILER_ID:GNU,Clang,AppleClang>:-Wall;-Wextra>)
set_source_files_properties(src/parser.c src/scanner.c PROPERTIES
  COMPILE_OPTIONS "$<$<C_COMPILER_ID:GNU,Clang,AppleClang>:-Wno-unused-parameter>")

# ---------------------------------------------------------------------------
# The tree-sitter runtime.
#
# Generated code builds against nothing, but *running* a parser needs the
# runtime, and so does the database below. It is fetched from upstream at the
# pinned tag and linked statically, so it leaves nothing behind for the
# installed library to depend on. A runtime already installed on the host is
# used in preference, so a distribution packaging ryspec against its own copy
# gets what it expects; RYSPEC_FETCH_TREE_SITTER forces the fetched one either
# way.

option(RYSPEC_FETCH_TREE_SITTER
  "Fetch and compile the tree-sitter runtime even where the host has one" OFF)

# The pin: the tag, and the SHA-256 of that tag's tarball. These two lines are
# where the runtime's version is recorded, and setup.py reads them back out of
# this file for its own download, so both builds compile the same source and a
# bump is one edit here. The checksum moves with the tag -- change both
# together, then run the suite: src/parser.c is generated at language ABI 14
# and the runtime has to still speak it.
set(TREE_SITTER_VERSION v0.27.0)
set(TREE_SITTER_SHA256 d35c96e68736bd9569d2757c3cc71052485f33082c3825f1aed9d0e86013a159)

if(NOT RYSPEC_FETCH_TREE_SITTER)
  find_path(TREE_SITTER_INCLUDE_DIR tree_sitter/api.h
    DOC "The tree-sitter runtime headers")
  find_library(TREE_SITTER_LIBRARY tree-sitter
    DOC "The tree-sitter runtime")
endif()

if(TREE_SITTER_INCLUDE_DIR AND TREE_SITTER_LIBRARY)
  message(STATUS "tree-sitter runtime: ${TREE_SITTER_LIBRARY} (host)")
  add_library(tree-sitter-runtime INTERFACE)
  target_include_directories(tree-sitter-runtime SYSTEM INTERFACE ${TREE_SITTER_INCLUDE_DIR})
  target_link_libraries(tree-sitter-runtime INTERFACE ${TREE_SITTER_LIBRARY})
else()
  message(STATUS "tree-sitter runtime: ${TREE_SITTER_VERSION} (fetched)")
  include(FetchContent)
  FetchContent_Declare(tree-sitter
    URL https://github.com/tree-sitter/tree-sitter/archive/refs/tags/${TREE_SITTER_VERSION}.tar.gz
    URL_HASH SHA256=${TREE_SITTER_SHA256}
    DOWNLOAD_EXTRACT_TIMESTAMP ON
    # lib/ carries no CMakeLists.txt, so MakeAvailable unpacks the tree and
    # stops there -- upstream's own build, a shared library with its own
    # install rules, is no part of this one. We compile lib.c ourselves, which
    # is also the file setup.py compiles.
    SOURCE_SUBDIR lib)
  FetchContent_MakeAvailable(tree-sitter)

  # lib.c is the runtime's own amalgamation -- it includes every other source,
  # so one translation unit is the whole library. Static, and linked PRIVATE
  # below: what libryspec needs of it is inside libryspec, and nothing of the
  # runtime is installed or has to be found again at load time.
  add_library(tree-sitter-runtime STATIC ${tree-sitter_SOURCE_DIR}/lib/src/lib.c)
  target_include_directories(tree-sitter-runtime
    PRIVATE ${tree-sitter_SOURCE_DIR}/lib/src
    SYSTEM PUBLIC ${tree-sitter_SOURCE_DIR}/lib/include)
  set_target_properties(tree-sitter-runtime PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    POSITION_INDEPENDENT_CODE ON)
  # Upstream source, not ours to tidy.
  target_compile_options(tree-sitter-runtime PRIVATE
    $<$<C_COMPILER_ID:GNU,Clang,AppleClang>:-w>)
endif()

# ---------------------------------------------------------------------------
# The database: what a document says, read out of its parse tree.
#
# It joins the parser library rather than standing beside it, so libryspec is
# the whole of ryspec in C: one artifact exporting tree_sitter_ryspec and the
# ryspec_* interface of src/database.h.
target_sources(ryspec PRIVATE src/database.c src/loader.c src/semantics.c)
target_link_libraries(ryspec PRIVATE tree-sitter-runtime)
target_include_directories(ryspec PUBLIC
  $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/ryspec>)

# ---------------------------------------------------------------------------
# The Python package.
#
# A wheel holds the library beside the package and nothing else, so the build
# ends here when it is one being built: the applications below, the tests over
# them and the system install rules are all outside what a wheel carries.
# `pip install .` therefore compiles the library and no more.
if(RYSPEC_PYTHON_MODULE)
  # Into the package directory itself -- the install prefix is the wheel's
  # root, so these two land beside __init__.py, which is where
  # ryspec.semantics looks for the library and where DEFAULT_SCHEMA resolves.
  #
  # The schema is installed here rather than left to travel with the package
  # sources because python/src/ryspec/schemas is a symlink out of the package,
  # and a wheel is built by walking that directory rather than following it.
  # So the symlink serves a checkout, and this serves a wheel, from one file.
  install(TARGETS ryspec LIBRARY DESTINATION ryspec)
  install(DIRECTORY schemas/v0 DESTINATION ryspec/schemas)
  return()
endif()

# ---------------------------------------------------------------------------
# The two applications over it.

# ryspec-parse answers the grammar's question, ryspec-db the loader's.
foreach(application ryspec-parse ryspec-db)
  add_executable(${application} cli/${application}.c)
  target_link_libraries(${application} PRIVATE ryspec tree-sitter-runtime)
  set_target_properties(${application} PROPERTIES
    C_STANDARD 23
    C_STANDARD_REQUIRED ON
    RUNTIME_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR})
  target_compile_definitions(${application} PRIVATE RYSPEC_VERSION="${PROJECT_VERSION}")
  target_compile_options(${application} PRIVATE
    $<$<C_COMPILER_ID:GNU,Clang,AppleClang>:-Wall;-Wextra>)
endforeach()

# The grammar is authored at src/grammar.json, which is also what the CLI
# reads, so generating rewrites the parser beside it and leaves the grammar
# alone. Out of the default build: the parser it writes is committed.
if(TREE_SITTER_CLI)
  add_custom_target(generate
    COMMAND ${TREE_SITTER_CLI} generate src/grammar.json
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
    COMMENT "Regenerating src/parser.c from src/grammar.json"
    VERBATIM)
endif()

if(BUILD_TESTING)
  # Both suites run from the source directory: `tree-sitter test` reads
  # test/corpus, and pytest is configured by the pyproject.toml beside it.
  if(TREE_SITTER_CLI)
    add_test(NAME grammar-corpus
      COMMAND ${TREE_SITTER_CLI} test
      WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
  endif()

  # The corpus under data/, run through the application. data-corpus is the
  # whole of it at once, each file held to what its own header says the grammar
  # owes it; the two beside it are the halves where the verdict is uniform, and
  # are what the application's other two modes are for.
  add_test(NAME data-valid
      COMMAND ryspec-parse ${PROJECT_SOURCE_DIR}/data/valid)
    add_test(NAME data-malformed
      COMMAND ryspec-parse --expect-malformed ${PROJECT_SOURCE_DIR}/data/malformed)
    add_test(NAME data-corpus
      COMMAND ryspec-parse --corpus ${PROJECT_SOURCE_DIR}/data)
  add_test(NAME examples-parse
    COMMAND ryspec-parse ${PROJECT_SOURCE_DIR}/examples)

  # The same corpus through the database, which answers the loader's question
  # rather than the grammar's. db-semantic reads each file's
  # #:expect-semantic-error header; the schema and grammar fixtures beside them
  # are not the database's to judge and it says nothing about those.
  add_test(NAME db-valid
    COMMAND ryspec-db ${PROJECT_SOURCE_DIR}/data/valid ${PROJECT_SOURCE_DIR}/examples)
  add_test(NAME db-semantic
    COMMAND ryspec-db --corpus ${PROJECT_SOURCE_DIR}/data)
  add_test(NAME db-fixtures
    COMMAND ryspec-db --expect-invalid ${PROJECT_SOURCE_DIR}/python/tests/fixtures/invalid)
  # The namespace doing its work: two documents declaring the same local names,
  # held in one database at once and kept apart by their namespaces.
  add_test(NAME db-namespaces
    COMMAND ryspec-db --together ${PROJECT_SOURCE_DIR}/data/valid/namespaces)

  # The pytest on PATH rather than `python -m pytest`: the interpreter CMake
  # finds is not necessarily the environment the suite is installed into.
  find_program(PYTEST_EXECUTABLE pytest)
  if(PYTEST_EXECUTABLE)
    add_test(NAME python-suite
      COMMAND ${PYTEST_EXECUTABLE} -q
      WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
  endif()
endif()

install(TARGETS ryspec
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(TARGETS ryspec-parse ryspec-db
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
# The library exports more than tree_sitter_ryspec now, so it ships a header.
install(FILES src/database.h
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/ryspec)
install(FILES queries/highlights.scm
  DESTINATION ${CMAKE_INSTALL_DATADIR}/ryspec/queries)
