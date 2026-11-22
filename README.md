# Fluent OTAP

Fluent OTAP is a native C reader and writer for OpenTelemetry Arrow Protocol
(OTAP) batches carrying logs, metrics, and traces. It uses protobuf-c for the
`BatchArrowRecords` envelope and nanoarrow for the Arrow IPC payloads. The
library exposes Arrow C Data Interface schemas and arrays. The caller handles
gRPC transport, retries, and conversion to its own record formats.

## Build and test

Use CMake 3.20 or later, a C compiler, and the Zstandard development package.
CMake fetches a pinned nanoarrow snapshot and can fetch LZ4 when a system
development package is unavailable. CMake also fetches the pinned protobuf-c
runtime for standalone builds. The generated bindings are checked in, so a
regular build needs neither a protobuf compiler nor Go.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

The native suite covers dictionary deltas, schema changes, owned and borrowed
batch lifetimes, malformed input, size limits, and uncompressed, Zstandard,
and LZ4 IPC. Enable the bidirectional interoperability test with Go 1.26 or
later. It generates fixtures with the upstream Go codec, reads and rewrites
them in C, and checks the resulting batches in Go.

```sh
cmake -S . -B build -DFLUENT_OTAP_GO_INTEROP=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

GitHub Actions runs the native suite on Linux x64 and ARM64, macOS Intel and
Apple silicon, and Windows x86, x64, and ARM64. Linux covers GCC and Clang.
The Linux GCC jobs also run strict Valgrind, and Go interoperability runs on
Linux x64, macOS ARM64, and Windows x64. The same memory check can be run
locally.

```sh
valgrind --leak-check=full --show-leak-kinds=all --errors-for-leak-kinds=all \
  --error-exitcode=99 build/tests/fluent-otap-test tests/fixtures
```

## Embed the codec

Add the repository as a CMake subdirectory and link the static target.

```cmake
add_subdirectory(path/to/fluent-otap)
target_link_libraries(my_application PRIVATE fluent-otap::fluent-otap)
```

Tests are enabled by default for standalone builds. Set `FLUENT_OTAP_TESTS=ON`
to enable them in an embedding project. Test registration at the parent build
root also requires the parent to enable CTest.

If a parent project already builds a compatible `fluent-otel-proto` target,
define that target before adding Fluent OTAP. Fluent OTAP then shares its
protobuf-c runtime.

## License

Fluent OTAP source is licensed under the [Apache License 2.0](LICENSE).
The fetched protobuf-c runtime retains its BSD-2-Clause license and copyright
notices. The standalone build fetches that runtime from
[Fluent OTel Proto at 95a3475](https://github.com/fluent/fluent-otel-proto/tree/95a3475b94f8d58a565f018b9ee6895820ceba51/proto_c/protobuf-c).

## Codec API

Include `<fluent-otap/otap.h>` and create one reader or writer per
signal-specific RPC.

- `flb_otap_read()` returns owned schemas and arrays that can outlive the reader.
- `flb_otap_read_views()` returns validated array views for synchronous use.
  Consume the views before destroying the batch, reading the next batch,
  or destroying the reader.
- `flb_otap_batch_destroy()` releases either kind of batch. Create a fresh
  reader after a decode failure or reconnect.
- `flb_otap_write()` takes a batch ID, optional encoded headers, and Arrow tables.
  The writer emits a schema on first use and on schema ID changes. It writes
  full dictionaries for dictionary-encoded columns. The caller supplies unique
  batch IDs and stable schema IDs and releases the output with
  `ArrowBufferReset()`. Recreate the writer on reconnect.

The reader remembers schemas and dictionaries across batches. Its default
limits are 16 MiB of wire bytes, 64 MiB of expanded Arrow buffers per batch, and
64 MiB of expanded dictionaries retained per reader. The generated bindings
also expose `BatchStatus` for the caller's acknowledgments.

## Regenerate bindings

The schema is pinned to [OTel Arrow Go v0.60.3](https://github.com/open-telemetry/otel-arrow/blob/go/v0.60.3/proto/opentelemetry/proto/experimental/arrow/v1/arrow_service.proto).
The C bindings in `proto_c/` were generated with protobuf-c 1.5.2. The generator
also adapts the descriptor initializers to the runtime callback signature.

```sh
python3 tools/generate-proto.py --protoc-c /path/to/protoc-c
python3 tools/generate-proto.py --protoc-c /path/to/protoc-c --check
```

Regenerate the checked-in Go fixtures after a deliberate interoperability update.

```sh
(cd tests/interop && go run -mod=readonly . generate ../fixtures)
```

The pinned nanoarrow build applies a FlatBuffers compatibility adjustment for Go.
It bounds fields against the verified message extent while retaining alignment
and offset checks. The native suite checks that an out-of-bounds IPC header
is rejected.
