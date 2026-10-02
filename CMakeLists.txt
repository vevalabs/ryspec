cmake_minimum_required(VERSION 3.25)

project(ryspec
  VERSION 0.1.0
  DESCRIPTION "C library for the ryspec specification format"
  HOMEPAGE_URL "https://github.com/vevalabs/ryspec"
  LANGUAGES C)

include(CTest)
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

add_library(ryspec STATIC
  src/ryspec.c
  $<TARGET_OBJECTS:tomlc17>)
add_library(ryspec::ryspec ALIAS ryspec)

target_include_directories(ryspec
  PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  PRIVATE
    $<TARGET_PROPERTY:tomlc17,INTERFACE_INCLUDE_DIRECTORIES>)

target_compile_definitions(ryspec PRIVATE
  RYSPEC_VERSION_STRING="${PROJECT_VERSION}")

target_compile_options(ryspec PRIVATE
  $<$<C_COMPILER_ID:GNU,Clang,AppleClang>:-Wall;-Wextra;-Wpedantic>)

set_target_properties(ryspec PROPERTIES
  C_STANDARD 17
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF)

# ---------------------------------------------------------------------------
# Tests.

if(BUILD_TESTING)
  add_executable(test_ryspec tests/test_ryspec.c)
  target_link_libraries(test_ryspec PRIVATE ryspec::ryspec)
  set_target_properties(test_ryspec PROPERTIES C_STANDARD 17 C_STANDARD_REQUIRED ON)
  add_test(NAME ryspec COMMAND test_ryspec ${PROJECT_SOURCE_DIR})
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
  ${PROJECT_BINARY_DIR}/ryspecConfigVersion.cmake
  COMPATIBILITY SameMajorVersion)
install(FILES ${PROJECT_BINARY_DIR}/ryspecConfigVersion.cmake
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ryspec)

# ---------------------------------------------------------------------------
# The Python extension, ryspec._core, built when scikit-build-core drives the
# build (pip install .). Its install rules are the `python` component, the
# only one pyproject.toml asks for, so the wheel holds the package and none of
# the C library's own installation above.

option(RYSPEC_PYTHON "Build the ryspec._core Python extension" OFF)

if(RYSPEC_PYTHON OR SKBUILD)
  find_package(Python 3.10 REQUIRED COMPONENTS Interpreter Development.Module)

  # The archive goes inside a shared module, so it must be relocatable.
  set_target_properties(ryspec tomlc17 PROPERTIES POSITION_INDEPENDENT_CODE ON)

  Python_add_library(_core MODULE WITH_SOABI python/ext/core.c)
  target_link_libraries(_core PRIVATE ryspec::ryspec)
  set_target_properties(_core PROPERTIES C_STANDARD 17 C_STANDARD_REQUIRED ON)

  install(TARGETS _core
    LIBRARY DESTINATION ryspec
    COMPONENT python)
  install(FILES schemas/v0/ryspec.schema.json
    DESTINATION ryspec/schemas/v0
    COMPONENT python)
endif()
