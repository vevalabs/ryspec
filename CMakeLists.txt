cmake_minimum_required(VERSION 3.25)

project(ryspec
  VERSION 0.1.0
  DESCRIPTION "Grammar and parser for the ryspec specification format"
  HOMEPAGE_URL "https://github.com/vevalabs/ryspec"
  LANGUAGES C)

include(CTest)
include(GNUInstallDirs)

find_program(TREE_SITTER_CLI tree-sitter
  DOC "The tree-sitter CLI, needed to regenerate the parser and to run the corpus")

# src/parser.c is generated and src/scanner.c is written by hand; both are
# committed, so a build needs a C compiler and nothing else.
add_library(ryspec SHARED
  src/parser.c
  src/scanner.c)

target_include_directories(ryspec PRIVATE src)

set_target_properties(ryspec PROPERTIES
  C_STANDARD 23
  C_STANDARD_REQUIRED ON
  POSITION_INDEPENDENT_CODE ON
  # libryspec.so -> libryspec.so.0 -> libryspec.so.0.1.0. The soname is the
  # major version, so everything built against 0.x keeps linking until the
  # parser's ABI -- the tree_sitter_ryspec symbol and the node types behind
  # it -- changes shape.
  VERSION ${PROJECT_VERSION}
  SOVERSION ${PROJECT_VERSION_MAJOR}
  # Straight into the build directory, which is where the Python test suite
  # and anyone loading the parser by hand expect to find it.
  LIBRARY_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR})

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
# runtime, and so does the database below. It is vendored at
# vendor/tree-sitter, so this build needs a C compiler and nothing else -- no
# network, no host package. A runtime installed on the host is used in
# preference, so a distribution packaging ryspec against its own copy gets what
# it expects; RYSPEC_VENDORED_TREE_SITTER forces the vendored one either way.

option(RYSPEC_VENDORED_TREE_SITTER
  "Compile the vendored tree-sitter runtime even where the host has one" OFF)

# The tag vendor/tree-sitter was taken from, read from the copy itself rather
# than repeated here, so there is nothing to keep in step: re-vendoring is the
# whole of changing it. src/parser.c is generated at language ABI 14 and this
# runtime speaks 13 through 15, so the two agree; a runtime is only obliged to
# load the ABIs it was built for, which is what the pin is holding.
file(STRINGS ${PROJECT_SOURCE_DIR}/vendor/tree-sitter/VERSION TREE_SITTER_VERSION LIMIT_COUNT 1)

if(NOT RYSPEC_VENDORED_TREE_SITTER)
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
  message(STATUS "tree-sitter runtime: ${TREE_SITTER_VERSION} (vendored)")
  # lib.c is the runtime's own amalgamation -- it includes every other source,
  # so one translation unit is the whole library.
  add_library(tree-sitter-runtime STATIC ${PROJECT_SOURCE_DIR}/vendor/tree-sitter/src/lib.c)
  target_include_directories(tree-sitter-runtime
    PRIVATE ${PROJECT_SOURCE_DIR}/vendor/tree-sitter/src
    SYSTEM PUBLIC ${PROJECT_SOURCE_DIR}/vendor/tree-sitter/include)
  set_target_properties(tree-sitter-runtime PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    POSITION_INDEPENDENT_CODE ON)
  # Vendored source, not ours to tidy.
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
