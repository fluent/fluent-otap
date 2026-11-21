# Go FlatBuffers can share a vtable between objects whose field sizes differ.
# Keep checking fields against the verified metadata extent and retain flatcc's
# alignment, overflow, offset, and recursive table verification.
# https://github.com/google/flatbuffers/blob/master/go/builder.go
set(verifier "thirdparty/flatcc/src/runtime/verifier.c")
file(READ "${verifier}" source)
set(original "    verify(k2 <= td->tsize, flatcc_verify_error_table_field_out_of_range);")
set(replacement [=[
    verify(k >= soffset_size && k2 <= td->end - td->table,
            flatcc_verify_error_table_field_out_of_range);]=])
string(REGEX MATCHALL "verify\\(k2 <= td->tsize," sites "${source}")
list(LENGTH sites count)
if(count EQUAL 0)
  string(REGEX MATCHALL "verify\\(k >= soffset_size && k2 <= td->end - td->table," sites "${source}")
  list(LENGTH sites count)
  if(NOT count EQUAL 2)
    message(FATAL_ERROR "Unexpected nanoarrow flatcc verifier source")
  endif()
elseif(count EQUAL 2)
  set(old_comment [=[
     * Note that we don't add td.table to k and we test against table
     * size not table end or buffer end. Otherwise it would not be safe
     * to optimized out the k <= k2 check for normal uoffset and voffset
     * configurations.]=])
  set(new_comment [=[
     * Go FlatBuffers can reuse a vtable across differently sized fields.
     * Check against the verified message's end before accessing a field.]=])
  string(REPLACE "${old_comment}" "${new_comment}" source "${source}")
  string(REPLACE "${original}" "${replacement}" source "${source}")
  file(WRITE "${verifier}" "${source}")
else()
  message(FATAL_ERROR "Unexpected nanoarrow flatcc verifier source")
endif()
