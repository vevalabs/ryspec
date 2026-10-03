cmake_minimum_required(VERSION 3.25)

project(ryspec
  VERSION 0.1.0
  DESCRIPTION "C library for the ryspec specification format"
  HOMEPAGE_URL "https://github.com/vevalabs/ryspec"
  LANGUAGES C)

include(CTest)

# build/compile_commands.json, which clangd reads (.clangd).
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(RYSPEC_REPOSITORY_DIR ${PROJECT_SOURCE_DIR})

add_subdirectory(libs/ryspec)

# ---------------------------------------------------------------------------
# The Python extension, ryspec._core, built when scikit-build-core drives the
# build (pip install .). Its install rules are the `python` component, the
# only one pyproject.toml asks for, so the wheel holds the package and none of
# the C library's own installation, in libs/ryspec.

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
