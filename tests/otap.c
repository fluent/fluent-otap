/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

/*  Fluent OTAP
 *  ===========
 *  Copyright (C) 2026 The Fluent Bit Authors
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <fluent-otap/otap.h>

#define PAYLOAD(name) OPENTELEMETRY__PROTO__EXPERIMENTAL__ARROW__V1__ARROW_PAYLOAD_TYPE__##name
#define BATCH(name) opentelemetry__proto__experimental__arrow__v1__batch_arrow_records__##name

typedef Opentelemetry__Proto__Experimental__Arrow__V1__BatchArrowRecords batch_message;

#define CHECK(condition)                                                             \
    do {                                                                             \
        if (!(condition)) {                                                          \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition);            \
            exit(EXIT_FAILURE);                                                      \
        }                                                                            \
    } while (0)

#define OK(operation)                                                                \
    do {                                                                             \
        int code = (operation);                                                       \
        if (code != 0) {                                                             \
            fprintf(stderr, "%s:%d: %s => %d: %s\n",                                 \
                    __FILE__, __LINE__, #operation, code, error.message);             \
            exit(EXIT_FAILURE);                                                      \
        }                                                                            \
    } while (0)

static struct ArrowBufferView bytes(const void *data, int64_t size)
{
    struct ArrowBufferView view;

    view.data.data = data;
    view.size_bytes = size;
    return view;
}

static void fixture_path(char *path, size_t size, const char *dir, const char *name, size_t index)
{
    int length;

    length = snprintf(path, size, "%s/%s-%zu.pb", dir, name, index);
    CHECK(length > 0 && (size_t) length < size);
}

static struct ArrowBuffer read_fixture(const char *dir, const char *name, size_t index)
{
    char path[1024];
    long size;
    FILE *file;
    struct ArrowBuffer input;
    struct ArrowError error = {0};

    fixture_path(path, sizeof(path), dir, name, index);
    file = fopen(path, "rb");
    CHECK(file != NULL);
    CHECK(fseek(file, 0, SEEK_END) == 0);
    size = ftell(file);
    CHECK(size > 0);
    CHECK(fseek(file, 0, SEEK_SET) == 0);

    ArrowBufferInit(&input);
    OK(ArrowBufferResize(&input, size, 0));
    input.size_bytes = size;
    CHECK(fread(input.data, 1, size, file) == (size_t) size);
    CHECK(fclose(file) == 0);
    return input;
}

static void write_fixture(const char *dir, const char *name, size_t index,
                          const struct ArrowBuffer *wire)
{
    char path[1024];
    FILE *file;

    fixture_path(path, sizeof(path), dir, name, index);
    file = fopen(path, "wb");
    CHECK(file != NULL);
    CHECK(fwrite(wire->data, 1, wire->size_bytes, file) == (size_t) wire->size_bytes);
    CHECK(fclose(file) == 0);
}

static void build_table(struct ArrowSchema *schema, struct ArrowArray *array, int64_t value)
{
    struct ArrowError error = {0};

    ArrowSchemaInit(schema);
    OK(ArrowSchemaSetTypeStruct(schema, 1));
    OK(ArrowSchemaSetType(schema->children[0], NANOARROW_TYPE_INT64));
    OK(ArrowSchemaSetName(schema->children[0], "value"));
    OK(ArrowArrayInitFromSchema(array, schema, &error));
    OK(ArrowArrayStartAppending(array));
    OK(ArrowArrayAppendInt(array->children[0], value));
    OK(ArrowArrayFinishElement(array));
    OK(ArrowArrayFinishBuildingDefault(array, &error));
}

static int64_t table_value(const struct flb_otap_table *table)
{
    struct ArrowArrayView view;
    struct ArrowError error = {0};
    int64_t value;

    OK(ArrowArrayViewInitFromSchema(&view, &table->schema, &error));
    OK(ArrowArrayViewSetArray(&view, &table->array, &error));
    value = ArrowArrayViewGetIntUnsafe(view.children[0], 0);
    ArrowArrayViewReset(&view);
    return value;
}

static void test_writer(enum ArrowIpcCompressionType codec)
{
    static const uint8_t headers[] = {0x82, 0x00, 0x80};
    struct ArrowBufferView metadata = bytes(headers, sizeof(headers));
    struct ArrowSchema schema;
    struct ArrowSchema attributes_schema;
    struct ArrowArray array;
    struct ArrowArray attributes;
    struct ArrowBuffer first;
    struct ArrowBuffer second;
    struct ArrowBuffer failed;
    struct ArrowBuffer changed;
    struct ArrowError error = {0};
    struct flb_otap_table_view input[2];
    struct flb_otap_writer *writer;
    struct flb_otap_reader *reader;
    struct flb_otap_batch *batch;
    struct flb_otap_batch *prior;

    build_table(&schema, &array, 42);
    build_table(&attributes_schema, &attributes, 17);
    writer = flb_otap_writer_create(FLB_OTAP_LOGS, codec);
    reader = flb_otap_reader_create(FLB_OTAP_LOGS, NULL);
    CHECK(writer != NULL && reader != NULL);

    input[0] = (struct flb_otap_table_view) {
        PAYLOAD(LOG_ATTRS), "attrs", &attributes_schema, &attributes
    };
    input[1] = (struct flb_otap_table_view) {PAYLOAD(LOGS), "root", &schema, &array};
    OK(flb_otap_write(writer, INT64_MIN, metadata, input, 2, &first, &error));
    OK(flb_otap_read(reader, bytes(first.data, first.size_bytes), &prior, &error));
    ArrowBufferReset(&first);

    input[0].schema_id = "attrs-new";
    OK(ArrowSchemaSetName(schema.children[0], "renamed"));
    CHECK(flb_otap_write(writer, 2, metadata, input, 2, &failed, &error) == EINVAL);
    CHECK(failed.data == NULL);
    ArrowBufferReset(&failed);

    OK(ArrowSchemaSetName(schema.children[0], "value"));
    OK(flb_otap_write(writer, 2, metadata, input, 2, &second, &error));
    OK(flb_otap_read(reader, bytes(second.data, second.size_bytes), &batch, &error));
    CHECK(batch->id == 2);
    CHECK(strcmp(batch->tables[0].schema_id, "attrs-new") == 0);
    CHECK(table_value(&batch->tables[0]) == 17);
    flb_otap_batch_destroy(batch);
    ArrowBufferReset(&second);

    OK(ArrowSchemaSetName(schema.children[0], "renamed"));
    input[1].schema_id = "root-new";
    OK(flb_otap_write(writer, 3, metadata, &input[1], 1, &changed, &error));
    OK(flb_otap_read(reader, bytes(changed.data, changed.size_bytes), &batch, &error));
    CHECK(batch->n_tables == 1 && table_value(&batch->tables[0]) == 42);
    CHECK(strcmp(batch->tables[0].schema.children[0]->name, "renamed") == 0);
    ArrowBufferReset(&changed);
    flb_otap_batch_destroy(batch);

    flb_otap_reader_destroy(reader);
    CHECK(prior->id == INT64_MIN && prior->n_tables == 2);
    CHECK(prior->headers.size_bytes == sizeof(headers));
    CHECK(memcmp(prior->headers.data.data, headers, sizeof(headers)) == 0);
    CHECK(table_value(&prior->tables[1]) == 42);
    CHECK(strcmp(prior->tables[1].schema.children[0]->name, "value") == 0);
    flb_otap_batch_destroy(prior);

    flb_otap_writer_destroy(writer);
    ArrowArrayRelease(&attributes);
    ArrowSchemaRelease(&attributes_schema);
    ArrowArrayRelease(&array);
    ArrowSchemaRelease(&schema);
}

static void check_dictionary_delta(const struct ArrowBuffer *wire)
{
    struct ArrowIpcDecoder decoder;
    struct ArrowBufferView ipc;
    struct ArrowError error = {0};
    batch_message *batch;
    ProtobufCBinaryData record;
    size_t index;
    int64_t message_size;
    int found;

    batch = BATCH(unpack)(NULL, wire->size_bytes, wire->data);
    CHECK(batch != NULL);
    OK(ArrowIpcDecoderInit(&decoder));
    found = 0;
    for (index = 0; index < batch->n_arrow_payloads; index++) {
        record = batch->arrow_payloads[index]->record;
        ipc = bytes(record.data, record.len);
        while (ipc.size_bytes > 0) {
            OK(ArrowIpcDecoderVerifyHeader(&decoder, ipc, &error));
            if (decoder.message_type == NANOARROW_IPC_MESSAGE_TYPE_DICTIONARY_BATCH) {
                OK(ArrowIpcDecoderDecodeHeader(&decoder, ipc, &error));
                if (decoder.dictionary->is_delta) {
                    found = 1;
                }
            }
            message_size = decoder.header_size_bytes + decoder.body_size_bytes;
            CHECK(message_size > 0 && message_size <= ipc.size_bytes);
            ipc.data.as_uint8 += message_size;
            ipc.size_bytes -= message_size;
        }
    }
    CHECK(found);
    ArrowIpcDecoderReset(&decoder);
    BATCH(free_unpacked)(batch, NULL);
}

static void check_original_log_dictionary(const struct flb_otap_table *table)
{
    struct ArrowError error = {0};
    struct ArrowArrayView view;
    struct ArrowArrayView *column;
    struct ArrowStringView value;
    int64_t index;
    int64_t dictionary_index;

    OK(ArrowArrayViewInitFromSchema(&view, &table->schema, &error));
    OK(ArrowArrayViewSetArray(&view, &table->array, &error));
    for (index = 0; index < view.n_children; index++) {
        if (strcmp(table->schema.children[index]->name, "severity_text") == 0) {
            break;
        }
    }
    CHECK(index < view.n_children);
    column = view.children[index];
    CHECK(column->dictionary != NULL);
    dictionary_index = ArrowArrayViewGetIntUnsafe(column, 0);
    value = ArrowArrayViewGetStringUnsafe(column->dictionary, dictionary_index);
    CHECK(value.size_bytes == 8 && memcmp(value.data, "notice-0", 8) == 0);
    ArrowArrayViewReset(&view);
}

static void test_upstream(const char *dir, const char *output_dir, const char *name,
                          enum flb_otap_signal signal, size_t count,
                          flb_otap_payload_type root, int64_t rows)
{
    struct flb_otap_reader *reader;
    struct flb_otap_writer *writer;
    struct flb_otap_reader *roundtrip;
    struct flb_otap_reader *view_reader;
    struct flb_otap_batch *batch;
    struct flb_otap_batch *output;
    struct flb_otap_batch *borrowed;
    struct flb_otap_batch *first;
    struct flb_otap_table_view view[46];
    struct ArrowBuffer input;
    struct ArrowBuffer encoded;
    struct ArrowError error = {0};
    size_t index;
    size_t table;
    size_t root_index;
    size_t first_root;

    first = NULL;
    first_root = 0;
    reader = flb_otap_reader_create(signal, NULL);
    roundtrip = flb_otap_reader_create(signal, NULL);
    view_reader = flb_otap_reader_create(signal, NULL);
    writer = flb_otap_writer_create(signal, NANOARROW_IPC_COMPRESSION_TYPE_ZSTD);
    CHECK(reader != NULL && writer != NULL && roundtrip != NULL && view_reader != NULL);

    for (index = 0; index < count; index++) {
        input = read_fixture(dir, name, index);
        if (signal == FLB_OTAP_LOGS && index == 1) {
            check_dictionary_delta(&input);
        }
        OK(flb_otap_read(reader, bytes(input.data, input.size_bytes), &batch, &error));
        ArrowBufferReset(&input);

        CHECK(batch->n_tables > 1 && batch->n_tables <= sizeof(view) / sizeof(view[0]));
        root_index = batch->n_tables;
        for (table = 0; table < batch->n_tables; table++) {
            view[table] = (struct flb_otap_table_view) {
                batch->tables[table].type,
                batch->tables[table].schema_id,
                &batch->tables[table].schema,
                &batch->tables[table].array
            };
            if (view[table].type == root) {
                root_index = table;
                CHECK(view[table].array->length == rows);
            }
        }
        CHECK(root_index != batch->n_tables);

        OK(flb_otap_write(writer, batch->id, batch->headers, view, batch->n_tables,
                          &encoded, &error));
        if (output_dir != NULL) {
            write_fixture(output_dir, name, index, &encoded);
        }
        OK(flb_otap_read(roundtrip, bytes(encoded.data, encoded.size_bytes), &output, &error));
        OK(flb_otap_read_views(view_reader, bytes(encoded.data, encoded.size_bytes),
                               &borrowed, &error));
        ArrowBufferReset(&encoded);
        CHECK(borrowed->id == batch->id && borrowed->n_tables == batch->n_tables);
        CHECK(borrowed->tables[root_index].type == root);
        CHECK(borrowed->tables[root_index].schema.release == NULL);
        CHECK(borrowed->tables[root_index].array.release == NULL);
        CHECK(borrowed->tables[root_index].view_schema != NULL);
        CHECK(borrowed->tables[root_index].view != NULL);
        CHECK(borrowed->tables[root_index].view->length == rows);
        flb_otap_batch_destroy(borrowed);
        CHECK(output->id == batch->id && output->n_tables == batch->n_tables);
        CHECK(output->tables[root_index].type == root &&
              output->tables[root_index].array.length == rows);
        flb_otap_batch_destroy(output);

        if (index == 0) {
            first = batch;
            first_root = root_index;
        }
        else {
            if (signal == FLB_OTAP_LOGS) {
                CHECK((strcmp(batch->tables[root_index].schema_id,
                              first->tables[first_root].schema_id) == 0) == (index == 1));
            }
            flb_otap_batch_destroy(batch);
        }
    }

    flb_otap_reader_destroy(reader);
    CHECK(first->tables[first_root].array.length == rows);
    if (signal == FLB_OTAP_LOGS) {
        check_original_log_dictionary(&first->tables[first_root]);
    }
    flb_otap_batch_destroy(first);
    flb_otap_reader_destroy(roundtrip);
    flb_otap_reader_destroy(view_reader);
    flb_otap_writer_destroy(writer);
}

static void expect_read_error(enum flb_otap_signal signal, struct ArrowBufferView wire,
                              const struct flb_otap_read_options *options, int expected)
{
    struct flb_otap_reader *reader;
    struct flb_otap_batch *batch;
    struct ArrowError error = {0};
    int result;

    reader = flb_otap_reader_create(signal, options);
    CHECK(reader != NULL);
    result = flb_otap_read(reader, wire, &batch, &error);
    if (result != expected) {
        fprintf(stderr, "expected %d, received %d: %s\n", expected, result, error.message);
        exit(EXIT_FAILURE);
    }
    CHECK(batch == NULL && error.message[0] != '\0');
    CHECK(flb_otap_read(reader, wire, &batch, &error) == EINVAL);
    CHECK(batch == NULL);
    flb_otap_reader_destroy(reader);
}

static void test_read_errors(const char *dir)
{
    struct ArrowBuffer initial;
    struct ArrowBuffer continuation;
    struct ArrowBuffer truncated;
    struct ArrowBufferView wire;
    struct ArrowError error = {0};
    struct flb_otap_read_options options;
    batch_message *message;
    size_t saved_size;
    size_t packed_size;

    initial = read_fixture(dir, "logs", 0);
    continuation = read_fixture(dir, "logs", 1);
    wire = bytes(initial.data, initial.size_bytes);
    expect_read_error(FLB_OTAP_TRACES, wire, NULL, EINVAL);
    expect_read_error(FLB_OTAP_LOGS, bytes(initial.data, initial.size_bytes - 1), NULL, EINVAL);
    expect_read_error(FLB_OTAP_LOGS, bytes(continuation.data, continuation.size_bytes),
                      NULL, EINVAL);

    flb_otap_read_options_init(&options);
    options.max_wire_bytes = initial.size_bytes - 1;
    expect_read_error(FLB_OTAP_LOGS, wire, &options, EMSGSIZE);
    flb_otap_read_options_init(&options);
    options.max_batch_bytes = 2;
    expect_read_error(FLB_OTAP_LOGS, wire, &options, EMSGSIZE);
    flb_otap_read_options_init(&options);
    options.max_dictionary_bytes = 2;
    expect_read_error(FLB_OTAP_LOGS, wire, &options, EMSGSIZE);

    message = BATCH(unpack)(NULL, initial.size_bytes, initial.data);
    CHECK(message != NULL && message->n_arrow_payloads > 0);
    saved_size = message->arrow_payloads[message->n_arrow_payloads - 1]->record.len;
    message->arrow_payloads[message->n_arrow_payloads - 1]->record.len--;
    packed_size = BATCH(get_packed_size)(message);
    ArrowBufferInit(&truncated);
    OK(ArrowBufferResize(&truncated, packed_size, 0));
    CHECK(BATCH(pack)(message, truncated.data) == packed_size);
    expect_read_error(FLB_OTAP_LOGS, bytes(truncated.data, packed_size), NULL, EINVAL);
    message->arrow_payloads[message->n_arrow_payloads - 1]->record.len = saved_size;

    /* The FlatBuffer root offset must stay within the verified IPC metadata. */
    memset(message->arrow_payloads[0]->record.data + 8, 0xff, 4);
    packed_size = BATCH(get_packed_size)(message);
    OK(ArrowBufferResize(&truncated, packed_size, 0));
    CHECK(BATCH(pack)(message, truncated.data) == packed_size);
    expect_read_error(FLB_OTAP_LOGS, bytes(truncated.data, packed_size), NULL, EINVAL);
    BATCH(free_unpacked)(message, NULL);

    ArrowBufferReset(&truncated);
    ArrowBufferReset(&continuation);
    ArrowBufferReset(&initial);
}

int main(int argc, char **argv)
{
    const char *output;

    CHECK(argc == 2 || argc == 3);
    output = argc == 3 ? argv[2] : NULL;

    test_writer(NANOARROW_IPC_COMPRESSION_TYPE_NONE);
    test_writer(NANOARROW_IPC_COMPRESSION_TYPE_ZSTD);
    test_writer(NANOARROW_IPC_COMPRESSION_TYPE_LZ4_FRAME);
    test_upstream(argv[1], output, "logs", FLB_OTAP_LOGS, 3, PAYLOAD(LOGS), 16);
    test_upstream(argv[1], output, "metrics", FLB_OTAP_METRICS, 1, PAYLOAD(UNIVARIATE_METRICS), 1);
    test_upstream(argv[1], output, "traces", FLB_OTAP_TRACES, 1, PAYLOAD(SPANS), 1);
    test_read_errors(argv[1]);
    return EXIT_SUCCESS;
}
