cmake_minimum_required(VERSION 3.20)
project(fluent-otap C)

include(FetchContent)
if(POLICY CMP0135)
  cmake_policy(SET CMP0135 NEW)
endif()

option(FLUENT_OTAP_TESTS "Build Fluent OTAP unit tests" ${PROJECT_IS_TOP_LEVEL})
option(FLUENT_OTAP_GO_INTEROP "Test interoperability with the upstream Go codec" OFF)

if(FLUENT_OTAP_GO_INTEROP AND NOT FLUENT_OTAP_TESTS)
  message(FATAL_ERROR "FLUENT_OTAP_GO_INTEROP requires FLUENT_OTAP_TESTS")
endif()

find_path(FLUENT_OTAP_LZ4_INCLUDE_DIR lz4frame.h)
find_library(FLUENT_OTAP_LZ4_LIBRARY lz4)
if(FLUENT_OTAP_LZ4_INCLUDE_DIR AND FLUENT_OTAP_LZ4_LIBRARY)
  set(LZ4_INCLUDE_DIRS "${FLUENT_OTAP_LZ4_INCLUDE_DIR}")
  set(LZ4_LIBRARIES "${FLUENT_OTAP_LZ4_LIBRARY}")
else()
  set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)
  set(LZ4_BUILD_CLI OFF)
  set(BUILD_STATIC_LIBS ON)
  FetchContent_Declare(
    fluent_otap_lz4
    URL https://codeload.github.com/lz4/lz4/tar.gz/ebb370ca83af193212df4dcbadcc5d87bc0de2f0
    URL_HASH SHA256=eb1a93e934d4fd29df6e2061ba0bf447568561764d758f4a5662c0e29370ffa9
    SOURCE_SUBDIR build/cmake)
  FetchContent_MakeAvailable(fluent_otap_lz4)
  set(LZ4_INCLUDE_DIRS "${fluent_otap_lz4_SOURCE_DIR}/lib")
  set(LZ4_LIBRARIES lz4_static)
endif()

# This snapshot has delta dictionary decoding, dictionary writing, and IPC compression.
set(NANOARROW_IPC ON)
set(NANOARROW_IPC_WITH_ZSTD ON)
set(NANOARROW_IPC_WITH_LZ4 ON)
FetchContent_Declare(
  nanoarrow
  URL https://codeload.github.com/apache/arrow-nanoarrow/tar.gz/c84a84247a82636d81eb47e04d15cf90362214f9
  URL_HASH SHA256=2d9a2d6810fd17c09e5a0a610bac60b87f5c323c13c0c04ce0a5887d461b7ec5
  PATCH_COMMAND "${CMAKE_COMMAND}" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/apply-nanoarrow-patch.cmake")
FetchContent_MakeAvailable(nanoarrow)

if(NOT TARGET fluent-otel-proto)
  # Use the same protobuf-c runtime snapshot as Fluent OTel Proto.
  FetchContent_Declare(
    fluent_otap_protobuf_c
    URL https://codeload.github.com/fluent/fluent-otel-proto/tar.gz/95a3475b94f8d58a565f018b9ee6895820ceba51
    URL_HASH SHA256=4aa792b02c3939e7fcf77ff4289720fb58f2925c840406a644d75a468616ea67
    SOURCE_SUBDIR proto_c/protobuf-c)
  FetchContent_MakeAvailable(fluent_otap_protobuf_c)
endif()

add_subdirectory(src)

if(FLUENT_OTAP_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
