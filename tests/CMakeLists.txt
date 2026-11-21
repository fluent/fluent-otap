add_executable(fluent-otap-test otap.c)
target_link_libraries(fluent-otap-test PRIVATE fluent-otap::fluent-otap)
add_test(NAME fluent-otap COMMAND fluent-otap-test "${CMAKE_CURRENT_SOURCE_DIR}/fixtures")

if(FLUENT_OTAP_GO_INTEROP)
  find_program(FLUENT_OTAP_GO_EXECUTABLE go REQUIRED)
  add_test(NAME fluent-otap-go-interop
    COMMAND "${CMAKE_COMMAND}"
      "-DGO_EXECUTABLE=${FLUENT_OTAP_GO_EXECUTABLE}"
      "-DGO_DIR=${CMAKE_CURRENT_SOURCE_DIR}/interop"
      "-DTEST_EXECUTABLE=$<TARGET_FILE:fluent-otap-test>"
      "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/interop"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/interop/run.cmake")
  set_tests_properties(fluent-otap-go-interop PROPERTIES TIMEOUT 300)
endif()
