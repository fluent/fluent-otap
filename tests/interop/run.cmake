set(go_fixtures "${WORK_DIR}/go")
set(c_fixtures "${WORK_DIR}/c")
file(MAKE_DIRECTORY "${go_fixtures}" "${c_fixtures}")

execute_process(
  COMMAND "${GO_EXECUTABLE}" run -mod=readonly . generate "${go_fixtures}"
  WORKING_DIRECTORY "${GO_DIR}"
  COMMAND_ERROR_IS_FATAL ANY)
execute_process(
  COMMAND "${TEST_EXECUTABLE}" "${go_fixtures}" "${c_fixtures}"
  COMMAND_ERROR_IS_FATAL ANY)
execute_process(
  COMMAND "${GO_EXECUTABLE}" run -mod=readonly . verify "${c_fixtures}"
  WORKING_DIRECTORY "${GO_DIR}"
  COMMAND_ERROR_IS_FATAL ANY)
