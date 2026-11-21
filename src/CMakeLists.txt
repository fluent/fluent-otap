add_library(fluent-otap STATIC otap.c ../proto_c/arrow_service.pb-c.c)
add_library(fluent-otap::fluent-otap ALIAS fluent-otap)

target_compile_features(fluent-otap PUBLIC c_std_99)
target_include_directories(fluent-otap PUBLIC
  "${PROJECT_SOURCE_DIR}/include"
  "${PROJECT_SOURCE_DIR}/proto_c")
target_link_libraries(fluent-otap PUBLIC nanoarrow_ipc_static PRIVATE flatccrt)

# An embedding project can share the runtime supplied by fluent-otel-proto.
if(TARGET fluent-otel-proto)
  target_link_libraries(fluent-otap PUBLIC fluent-otel-proto)

  # Older Fluent OTel Proto targets do not export their protobuf-c include path.
  get_target_property(fluent_otel_proto_dir fluent-otel-proto SOURCE_DIR)
  if(EXISTS "${fluent_otel_proto_dir}/../proto_c/protobuf-c/protobuf-c.h")
    target_include_directories(fluent-otap PUBLIC "${fluent_otel_proto_dir}/../proto_c")
  endif()
else()
  set(protobuf_c_dir "${fluent_otap_protobuf_c_SOURCE_DIR}/proto_c")
  add_library(fluent-otap-protobuf-c STATIC "${protobuf_c_dir}/protobuf-c/protobuf-c.c")
  target_compile_features(fluent-otap-protobuf-c PRIVATE c_std_99)
  target_include_directories(fluent-otap-protobuf-c PUBLIC "${protobuf_c_dir}")
  target_link_libraries(fluent-otap PUBLIC fluent-otap-protobuf-c)
endif()
