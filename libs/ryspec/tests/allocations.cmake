# The allocations test: libryspec allocates nothing of its own, and only
# tomlc17 does. Fails when an object of the archive, but tomlc17's, refers to
# an allocator. A document's one block comes through tomlc17's allocator,
# by the function pointer in its options, so it names none.
#
#   cmake -DNM=<nm> -DARCHIVE=<libryspec.a> -P allocations.cmake

execute_process(
  COMMAND ${NM} -A -u ${ARCHIVE}
  OUTPUT_VARIABLE symbols
  RESULT_VARIABLE status)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "${NM} failed on ${ARCHIVE}")
endif()

string(REPLACE "\n" ";" lines "${symbols}")
set(found "")
foreach(line IN LISTS lines)
  if(line MATCHES "tomlc17")
    continue()
  endif()
  if(line MATCHES "[ \t]_?(malloc|calloc|realloc|free|strdup|strndup|aligned_alloc|reallocarray)$")
    list(APPEND found "${line}")
  endif()
endforeach()

if(found)
  list(JOIN found "\n  " found)
  message(FATAL_ERROR "libryspec allocates of its own:\n  ${found}")
endif()
