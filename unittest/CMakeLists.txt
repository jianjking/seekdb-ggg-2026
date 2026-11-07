find_library(SEEKDB_GTEST_LIBRARY
  NAMES gtest
  PATHS "${DEP_DIR}/lib"
  NO_DEFAULT_PATH
  REQUIRED)
find_package(Threads REQUIRED)

set(SEEKDB_PRETEST_TARGETS "")
foreach(module IN LISTS SEEKDB_PRETEST_MODULES)
  string(TOUPPER "${module}" module_upper)
  set(prefix "SEEKDB_PRETEST_${module_upper}")
  set(test_target "${${prefix}_TARGET}")
  file(GLOB test_sources CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/${module}/test_*.cpp")
  if(NOT test_sources)
    message(FATAL_ERROR "No test sources for ${test_target}")
  endif()
  list(SORT test_sources)

  add_executable(${test_target} EXCLUDE_FROM_ALL
    "${CMAKE_CURRENT_SOURCE_DIR}/all_tests_main.cpp"
    ${test_sources})
  set_target_properties(${test_target} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/${module}"
    UNITY_BUILD ON
    UNITY_BUILD_MODE BATCH
    UNITY_BUILD_BATCH_SIZE "${${prefix}_UNITY_SIZE}")
  if(${prefix}_UNITY_EXCEPTIONS)
    set_source_files_properties(${${prefix}_UNITY_EXCEPTIONS} PROPERTIES
      SKIP_UNITY_BUILD_INCLUSION ON)
  endif()
  target_include_directories(${test_target} PRIVATE "${DEP_DIR}/include")
  target_link_libraries(${test_target} PRIVATE
    oceanbase
    "${SEEKDB_GTEST_LIBRARY}"
    Threads::Threads
    ${CMAKE_DL_LIBS})

  math(EXPR last_shard "${${prefix}_SHARD_COUNT} - 1")
  foreach(shard RANGE ${last_shard})
    set(test_name "${test_target}_shard_${shard}")
    add_test(NAME "${test_name}"
      COMMAND $<TARGET_FILE:${test_target}>
        "--gtest_output=xml:${CMAKE_CURRENT_BINARY_DIR}/${module}/shard-${shard}.xml")
    set_tests_properties("${test_name}" PROPERTIES
      WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/${module}"
      ENVIRONMENT
        "GTEST_TOTAL_SHARDS=${${prefix}_SHARD_COUNT};GTEST_SHARD_INDEX=${shard}")
  endforeach()
  list(APPEND SEEKDB_PRETEST_TARGETS "${test_target}")
endforeach()

add_custom_target(pretest DEPENDS ${SEEKDB_PRETEST_TARGETS})
configure_file(run_tests.sh.in "${CMAKE_CURRENT_BINARY_DIR}/run_tests.sh" @ONLY
  FILE_PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE
                   GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)
