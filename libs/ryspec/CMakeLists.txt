# libryspec: the C library, its tests and its installation. The repository's
# CMakeLists.txt declares the project and adds this directory; the tests read
# the documented examples from RYSPEC_REPOSITORY_DIR, the repository root.

include(GNUInstallDirs)
include(FetchContent)

# ---------------------------------------------------------------------------
# tomlc17, the TOML parser.
#
# Fetched from upstream at the pinned tag. The tag and the SHA-256 of its
# tarball move together: change both, then run the tests.
set(TOMLC17_VERSION R260821)
set(TOMLC17_SHA256 c4958fe2664d596ba1cc22589f56f900429fac89ad0738c3468c6e1021e92c1a)

FetchContent_Declare(tomlc17
  URL https://github.com/cktan/tomlc17/archive/refs/tags/${TOMLC17_VERSION}.tar.gz
  URL_HASH SHA256=${TOMLC17_SHA256}
  DOWNLOAD_EXTRACT_TIMESTAMP ON
  # Upstream builds with a Makefile and has no CMakeLists.txt, so MakeAvailable
  # only unpacks the tree; the one source file is compiled below.
  SOURCE_SUBDIR src)
FetchContent_MakeAvailable(tomlc17)

# An object library, so its code goes inside libryspec.a and a consumer links
# one archive. Its header is not part of ryspec's public interface.
add_library(tomlc17 OBJECT ${tomlc17_SOURCE_DIR}/src/tomlc17.c)
target_include_directories(tomlc17 SYSTEM PUBLIC
  $<BUILD_INTERFACE:${tomlc17_SOURCE_DIR}/src>)
set_target_properties(tomlc17 PROPERTIES
  C_STANDARD 17
  C_STANDARD_REQUIRED ON)

# ---------------------------------------------------------------------------
# libryspec.
#
# A thin wrapper over two parsers: tomlc17 reads the document
# (src/toml_doc.c), and the hand-written expression parser
# (src/expr_parser.c) turns each expression into prefix-form TOML that
# tomlc17 reads in turn (src/translate.c); src/toml_doc.c then indexes the
# tree's entities and resolves names over them. The linter reads both:
# src/lint.c walks rules and dispatches the checks, and src/lint_check.c
# holds them.
# tomlc17's types are named in src/toml_doc.h and the private headers built
# on it; the public header names none.

add_library(ryspec STATIC
  src/toml_doc.c
  src/translate.c
  src/expr_parser.c
  src/lint.c
  src/lint_check.c
  $<TARGET_OBJECTS:tomlc17>)
add_library(ryspec::ryspec ALIAS ryspec)

target_include_directories(ryspec
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  PRIVATE
    src
    $<TARGET_PROPERTY:tomlc17,INTERFACE_INCLUDE_DIRECTORIES>)

# The version as one number, as Boost writes its own: 0.1.0 is 100.
math(EXPR ryspec_version_number
  "${PROJECT_VERSION_MAJOR} * 100000 + ${PROJECT_VERSION_MINOR} * 100 + ${PROJECT_VERSION_PATCH}")
target_compile_definitions(ryspec PRIVATE
  RYSPEC_VERSION_NUMBER=${ryspec_version_number})

target_compile_options(ryspec PRIVATE
  $<$<C_COMPILER_ID:GNU,Clang,AppleClang>:-Wall;-Wextra;-Wpedantic>)

set_target_properties(ryspec PROPERTIES
  C_STANDARD 17
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF)

# ---------------------------------------------------------------------------
# Tests.
#
# Every tests/<name>.test.c is a test: an executable <name>.test, run with the
# repository root as its one argument and named <name> in ctest. A test may
# include src/ to test the private interfaces, tomlc17's types among them, as
# rule.test.c does.

if(BUILD_TESTING)
  file(GLOB ryspec_tests CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tests/*.test.c)
  foreach(source IN LISTS ryspec_tests)
    get_filename_component(file ${source} NAME)
    string(REGEX REPLACE "\\.test\\.c$" "" name ${file})
    add_executable(${name}.test ${source})
    target_link_libraries(${name}.test PRIVATE ryspec::ryspec)
    target_include_directories(${name}.test PRIVATE src
      $<TARGET_PROPERTY:tomlc17,INTERFACE_INCLUDE_DIRECTORIES>)
    set_target_properties(${name}.test PROPERTIES C_STANDARD 17 C_STANDARD_REQUIRED ON)
    add_test(NAME ${name} COMMAND ${name}.test ${RYSPEC_REPOSITORY_DIR})
  endforeach()

  # No allocation but tomlc17's: tests/allocations.cmake reads the archive.
  if(CMAKE_NM)
    add_test(NAME allocations
      COMMAND ${CMAKE_COMMAND} -DNM=${CMAKE_NM} -DARCHIVE=$<TARGET_FILE:ryspec>
              -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/allocations.cmake)
  endif()
endif()

# ---------------------------------------------------------------------------
# Installation: the archive, the public headers, and a CMake package, so a
# consumer writes find_package(ryspec) and links ryspec::ryspec.

install(TARGETS ryspec
  EXPORT ryspecTargets
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(DIRECTORY include/ryspec
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT ryspecTargets
  NAMESPACE ryspec::
  FILE ryspecConfig.cmake
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ryspec)

include(CMakePackageConfigHelpers)
write_basic_package_version_file(
  ${CMAKE_CURRENT_BINARY_DIR}/ryspecConfigVersion.cmake
  COMPATIBILITY SameMajorVersion)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/ryspecConfigVersion.cmake
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ryspec)
