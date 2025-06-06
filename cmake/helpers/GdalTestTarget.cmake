macro (gdal_test_target _target)
  set(multiValueArgs FILES)
  cmake_parse_arguments(ARGS "" "" "${multiValueArgs}" ${ARGN})

  add_executable(${_target} ${ARGS_FILES})
  target_link_libraries(${_target} PRIVATE $<TARGET_NAME:${GDAL_LIB_TARGET_NAME}>)
  gdal_standard_includes(${_target})
  target_compile_options(${_target} PRIVATE $<$<COMPILE_LANGUAGE:CXX>:${GDAL_CXX_WARNING_FLAGS}>
                                            $<$<COMPILE_LANGUAGE:C>:${GDAL_C_WARNING_FLAGS}>)
  target_include_directories(${_target} PRIVATE $<TARGET_PROPERTY:appslib,SOURCE_DIR>)
  target_compile_definitions(${_target} PRIVATE -DGDAL_TEST_ROOT_DIR="${GDAL_ROOT_TEST_DIR}")
  add_dependencies(${_target} ${GDAL_LIB_TARGET_NAME} gdal_plugins)
endmacro ()
