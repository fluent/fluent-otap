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
#include <stdlib.h>
#include <string.h>

#include <fluent-otap/otap.h>
#include <nanoarrow/ipc/flatcc_generated.h>

#define BATCH(name) opentelemetry__proto__experimental__arrow__v1__batch_arrow_records__##name
#define PAYLOAD(name) OPENTELEMETRY__PROTO__EXPERIMENTAL__ARROW__V1__ARROW_PAYLOAD_TYPE__##name
#define IPC(name) NANOARROW_IPC_MESSAGE_TYPE_##name
#define FB(name) FLATBUFFERS_WRAP_NAMESPACE(org_apache_arrow_flatbuf, name)
#define TABLE_SLOTS (PAYLOAD(SPAN_LINK_ATTRS) + 1)
#define MIB (INT64_C(1024) * 1024)

typedef Opentelemetry__Proto__Experimental__Arrow__V1__BatchArrowRecords batch_message;
typedef Opentelemetry__Proto__Experimental__Arrow__V1__ArrowPayload payload_message;

struct otap_read_table {
    char *schema_id;
    struct ArrowIpcDecoder decoder;
    struct ArrowIpcDictionaries dictionaries;
    struct ArrowSchema schema;
    struct ArrowBuffer dictionary_ids;
    int64_t *dictionary_sizes;
    int64_t dictionary_bytes;
};

struct otap_write_table {
    char *schema_id;
    struct ArrowBuffer schema;
};

struct flb_otap_reader {
    enum flb_otap_signal signal;
    struct flb_otap_read_options options;
    int failed;
    int64_t dictionary_bytes;
    struct otap_read_table tables[TABLE_SLOTS];
};

struct flb_otap_writer {
    enum flb_otap_signal signal;
    enum ArrowIpcCompressionType compression;
    struct otap_write_table tables[TABLE_SLOTS];
};

static int error_code(struct ArrowError *error, int code, const char *message)
{
    ArrowErrorSet(error, "%s", message);
    return code;
}

static char *copy_string(const char *value)
{
    char *copy;
    size_t size;

    size = strlen(value) + 1;
    copy = malloc(size);
    if (copy != NULL) {
        memcpy(copy, value, size);
    }
    return copy;
}

static int signal_valid(enum flb_otap_signal signal)
{
    return signal >= FLB_OTAP_LOGS && signal <= FLB_OTAP_TRACES;
}

static int type_allowed(enum flb_otap_signal signal, flb_otap_payload_type type)
{
    if (type == PAYLOAD(RESOURCE_ATTRS) || type == PAYLOAD(SCOPE_ATTRS)) {
        return 1;
    }
    switch (signal) {
    case FLB_OTAP_LOGS:
        return type == PAYLOAD(LOGS) || type == PAYLOAD(LOG_ATTRS);
    case FLB_OTAP_METRICS:
        return type >= PAYLOAD(UNIVARIATE_METRICS) && type <= PAYLOAD(METRIC_ATTRS);
    case FLB_OTAP_TRACES:
        return type >= PAYLOAD(SPANS) && type <= PAYLOAD(SPAN_LINK_ATTRS);
    default:
        return 0;
    }
}

static int root_present(enum flb_otap_signal signal, const unsigned char *seen)
{
    switch (signal) {
    case FLB_OTAP_LOGS:
        return seen[PAYLOAD(LOGS)];
    case FLB_OTAP_METRICS:
        return seen[PAYLOAD(UNIVARIATE_METRICS)] || seen[PAYLOAD(MULTIVARIATE_METRICS)];
    case FLB_OTAP_TRACES:
        return seen[PAYLOAD(SPANS)];
    default:
        return 0;
    }
}

static int register_table(enum flb_otap_signal signal, flb_otap_payload_type type,
                          const char *schema_id, unsigned char *seen, struct ArrowError *error)
{
    if (!type_allowed(signal, type) || seen[type] || schema_id == NULL || schema_id[0] == '\0') {
        return error_code(error, EINVAL, "Invalid, duplicate, or unnamed OTAP table");
    }
    seen[type] = 1;
    return 0;
}

void flb_otap_read_options_init(struct flb_otap_read_options *options)
{
    options->max_wire_bytes = 16 * MIB;
    options->max_batch_bytes = 64 * MIB;
    options->max_dictionary_bytes = 64 * MIB;
}

static void read_table_reset(struct otap_read_table *table)
{
    if (table->decoder.private_data != NULL) {
        ArrowIpcDecoderReset(&table->decoder);
    }
    if (table->dictionaries.private_data != NULL) {
        ArrowIpcDictionariesReset(&table->dictionaries);
    }
    if (table->schema.release != NULL) {
        ArrowSchemaRelease(&table->schema);
    }
    if (table->dictionary_ids.allocator.free != NULL) {
        ArrowBufferReset(&table->dictionary_ids);
    }
    free(table->dictionary_sizes);
    free(table->schema_id);
    memset(table, 0, sizeof(*table));
}

struct flb_otap_reader *flb_otap_reader_create(enum flb_otap_signal signal,
                                             const struct flb_otap_read_options *options)
{
    struct flb_otap_reader *reader;
    struct flb_otap_read_options defaults;

    if (options == NULL) {
        flb_otap_read_options_init(&defaults);
        options = &defaults;
    }
    if (!signal_valid(signal) || options->max_wire_bytes <= 0 || options->max_batch_bytes <= 0 ||
        options->max_dictionary_bytes <= 0) {
        return NULL;
    }
    reader = calloc(1, sizeof(*reader));
    if (reader != NULL) {
        reader->signal = signal;
        reader->options = *options;
    }
    return reader;
}

void flb_otap_reader_destroy(struct flb_otap_reader *reader)
{
    size_t index;

    if (reader != NULL) {
        for (index = 0; index < TABLE_SLOTS; index++) {
            read_table_reset(&reader->tables[index]);
        }
        free(reader);
    }
}

static int read_schema(struct otap_read_table *table, struct ArrowError *error)
{
    size_t count;
    int result;
    struct ArrowIpcDictionaryEncodings encodings;

    result = ArrowIpcDecoderDecodeSchemaWithDictionaries(&table->decoder, &table->schema,
                                                         &encodings, error);
    if (result != 0) {
        return result;
    }
    result = ArrowIpcDictionaryEncodingsUniqueIds(&encodings, &table->dictionary_ids);
    if (result == 0) {
        result = ArrowIpcDictionariesInit(&table->dictionaries, &encodings, error);
    }
    if (result == 0) {
        result = ArrowIpcDecoderSetSchemaWithDictionaries(&table->decoder, &table->schema,
                                                          &encodings, error);
    }
    ArrowIpcDictionaryEncodingsReset(&encodings);
    if (result != 0) {
        return result;
    }

    count = table->dictionary_ids.size_bytes / sizeof(int64_t);
    if (count > 0) {
        table->dictionary_sizes = calloc(count, sizeof(int64_t));
        if (table->dictionary_sizes == NULL) {
            return error_code(error, ENOMEM, "Cannot allocate OTAP dictionary state");
        }
    }
    return ArrowIpcDecoderSetEndianness(&table->decoder, table->decoder.endianness);
}

static uint64_t read_le64(const uint8_t *bytes)
{
    uint64_t value;
    unsigned int index;

    value = 0;
    for (index = 0; index < 8; index++) {
        value |= (uint64_t) bytes[index] << (index * 8);
    }
    return value;
}

/* nanoarrow allocates expanded buffers before invoking its decompressor. */
static int measure_buffers(const struct ArrowIpcDecoder *decoder,
                           const struct ArrowBufferView *message, struct ArrowBufferView body,
                           int64_t remaining, int64_t *expanded, struct ArrowError *error)
{
    FB(Message_table_t) header;
    FB(RecordBatch_table_t) record;
    flatbuffers_generic_t metadata;
    FB(Buffer_vec_t) buffers;
    FB(Buffer_struct_t) buffer;
    size_t index;
    int64_t length;
    int64_t offset;
    uint64_t size;

    /* The caller has verified the FlatBuffer header and checked the IPC framing. */
    header = FB(Message_as_root)(message->data.as_uint8 + 8);
    metadata = FB(Message_header_get)(header);
    if (decoder->message_type == IPC(DICTIONARY_BATCH)) {
        record = FB(DictionaryBatch_data)((FB(DictionaryBatch_table_t)) metadata);
    }
    else {
        record = (FB(RecordBatch_table_t)) metadata;
    }
    buffers = FB(RecordBatch_buffers)(record);
    *expanded = 0;
    for (index = 0; index < FB(Buffer_vec_len)(buffers); index++) {
        buffer = FB(Buffer_vec_at)(buffers, index);
        offset = FB(Buffer_offset)(buffer);
        length = FB(Buffer_length)(buffer);
        if (offset < 0 || offset > body.size_bytes || offset % 8 != 0 ||
            length < 0 || length > body.size_bytes - offset) {
            return error_code(error, EINVAL, "Invalid Arrow IPC buffer bounds");
        }
        size = length;
        if (length > 0 && decoder->codec != NANOARROW_IPC_COMPRESSION_TYPE_NONE) {
            if (length < 8) {
                return error_code(error, EINVAL, "Truncated Arrow IPC compression prefix");
            }
            size = read_le64(body.data.as_uint8 + offset);
            if (size == UINT64_MAX) {
                size = length - 8;
            }
            else if (size > INT64_MAX) {
                return error_code(error, EINVAL, "Invalid Arrow IPC uncompressed buffer size");
            }
        }
        if (size > (uint64_t) remaining) {
            return error_code(error, EMSGSIZE, "OTAP expanded batch exceeds limit");
        }
        remaining -= (int64_t) size;
        *expanded += (int64_t) size;
    }
    return 0;
}

static int read_dictionary(struct flb_otap_reader *reader, struct otap_read_table *table,
                           struct ArrowBufferView body, int64_t expanded, struct ArrowError *error)
{
    const int64_t *ids;
    size_t index;
    size_t count;
    int64_t previous;
    int result;

    ids = (const int64_t *) table->dictionary_ids.data;
    count = table->dictionary_ids.size_bytes / sizeof(int64_t);
    for (index = 0; index < count; index++) {
        if (ids[index] == table->decoder.dictionary->id) {
            break;
        }
    }
    if (index == count) {
        return error_code(error, EINVAL, "Unknown Arrow IPC dictionary ID");
    }

    previous = table->dictionary_sizes[index];
    if (table->decoder.dictionary->is_delta) {
        previous = 0;
    }
    if (expanded > reader->options.max_dictionary_bytes - (reader->dictionary_bytes - previous)) {
        return error_code(error, EMSGSIZE, "OTAP retained dictionaries exceed limit");
    }
    result = ArrowIpcDecoderDecodeDictionary(&table->decoder, body, NANOARROW_VALIDATION_LEVEL_FULL,
                                             &table->dictionaries, error);
    if (result == 0) {
        table->dictionary_sizes[index] += expanded - previous;
        table->dictionary_bytes += expanded - previous;
        reader->dictionary_bytes += expanded - previous;
    }
    return result;
}

static int read_payload(struct flb_otap_reader *reader, const payload_message *payload,
                        int64_t *remaining, int borrow_views, struct flb_otap_table *out,
                        struct ArrowError *error)
{
    int result;
    int64_t expanded;
    int64_t message_size;
    struct ArrowBufferView message;
    struct ArrowBufferView body;
    struct otap_read_table *table;
    struct ArrowIpcDecoder *decoder;
    struct ArrowArrayView *view;

    table = &reader->tables[payload->type];
    if (table->schema_id == NULL || strcmp(table->schema_id, payload->schema_id) != 0) {
        reader->dictionary_bytes -= table->dictionary_bytes;
        read_table_reset(table);
        table->schema_id = copy_string(payload->schema_id);
        if (table->schema_id == NULL) {
            return error_code(error, ENOMEM, "Cannot allocate OTAP schema ID");
        }
        result = ArrowIpcDecoderInit(&table->decoder);
        if (result != 0) {
            return error_code(error, result, "Cannot initialize Arrow IPC decoder");
        }
    }

    decoder = &table->decoder;
    message.data.as_uint8 = payload->record.data;
    message.size_bytes = (int64_t) payload->record.len;
    while (message.size_bytes > 0) {
        if (message.size_bytes < 8 || message.data.as_uint8[0] != 0xff ||
            message.data.as_uint8[1] != 0xff || message.data.as_uint8[2] != 0xff ||
            message.data.as_uint8[3] != 0xff) {
            return error_code(error, EINVAL, "Invalid Arrow IPC continuation marker");
        }

        result = ArrowIpcDecoderVerifyHeader(decoder, message, error);
        if (result != 0) {
            return result;
        }
        if (decoder->header_size_bytes < 8 || decoder->header_size_bytes % 8 != 0 ||
            decoder->body_size_bytes < 0 || decoder->body_size_bytes % 8 != 0 ||
            decoder->body_size_bytes > message.size_bytes - decoder->header_size_bytes) {
            return error_code(error, EINVAL, "Invalid or truncated Arrow IPC message");
        }
        body.data.as_uint8 = message.data.as_uint8 + decoder->header_size_bytes;
        body.size_bytes = decoder->body_size_bytes;
        message_size = decoder->header_size_bytes + body.size_bytes;

        result = ArrowIpcDecoderDecodeHeader(decoder, message, error);
        if (result != 0) {
            return result;
        }

        if (decoder->message_type == IPC(SCHEMA)) {
            if (table->schema.release != NULL || body.size_bytes != 0) {
                return error_code(error, EINVAL, "Unexpected Arrow IPC schema message");
            }
            result = read_schema(table, error);
        }
        else {
            if (table->schema.release == NULL ||
                (decoder->message_type != IPC(DICTIONARY_BATCH) &&
                 decoder->message_type != IPC(RECORD_BATCH))) {
                return error_code(error, EINVAL, "Expected an OTAP schema or table message");
            }
            result = measure_buffers(decoder, &message, body, *remaining, &expanded, error);
            if (result != 0) {
                return result;
            }
            *remaining -= expanded;
            if (decoder->message_type == IPC(DICTIONARY_BATCH)) {
                result = read_dictionary(reader, table, body, expanded, error);
            }
            else {
                if (message_size != message.size_bytes) {
                    return error_code(error, EINVAL, "Expected one final Arrow IPC record batch");
                }
                if (borrow_views) {
                    result = ArrowIpcDecoderDecodeArrayViewWithDictionaries(
                                 decoder, body, -1, &table->dictionaries, &view, error);
                    if (result == 0) {
                        result = ArrowArrayViewValidate(view, NANOARROW_VALIDATION_LEVEL_FULL, error);
                    }
                    if (result == 0) {
                        out->view = view;
                        out->view_schema = &table->schema;
                    }
                }
                else {
                    result = ArrowIpcDecoderDecodeArrayWithDictionaries(
                                 decoder, body, -1, &table->dictionaries, &out->array,
                                 NANOARROW_VALIDATION_LEVEL_FULL, error);
                }
            }
        }

        if (result != 0) {
            return result;
        }
        message.data.as_uint8 += message_size;
        message.size_bytes -= message_size;
    }

    if (out->view == NULL && out->array.release == NULL) {
        return error_code(error, EINVAL, "OTAP table is missing a record batch");
    }
    out->type = payload->type;
    out->schema_id = payload->schema_id;
    if (out->view != NULL) {
        return 0;
    }
    return ArrowSchemaDeepCopy(&table->schema, &out->schema);
}

void flb_otap_batch_destroy(struct flb_otap_batch *batch)
{
    size_t index;
    struct flb_otap_table *table;

    if (batch == NULL) {
        return;
    }
    for (index = 0; index < batch->n_tables; index++) {
        table = &batch->tables[index];
        if (table->array.release != NULL) {
            ArrowArrayRelease(&table->array);
        }
        if (table->schema.release != NULL) {
            ArrowSchemaRelease(&table->schema);
        }
    }
    free(batch->tables);
    BATCH(free_unpacked)(batch->private_data, NULL);
    free(batch);
}

static int decode_batch(struct flb_otap_reader *reader, struct ArrowBufferView wire,
                        int borrow_views, struct flb_otap_batch **out, struct ArrowError *error)
{
    struct flb_otap_batch *batch;
    batch_message *message;
    const payload_message *payload;
    unsigned char seen[TABLE_SLOTS] = {0};
    size_t index;
    int64_t remaining;
    int result;

    if (wire.size_bytes <= 0 || wire.data.data == NULL) {
        return error_code(error, EINVAL, "Empty OTAP protobuf batch");
    }
    if (wire.size_bytes > reader->options.max_wire_bytes || (uint64_t) wire.size_bytes > SIZE_MAX) {
        return error_code(error, EMSGSIZE, "OTAP protobuf batch exceeds limit");
    }

    message = BATCH(unpack)(NULL, (size_t) wire.size_bytes, wire.data.as_uint8);
    if (message == NULL) {
        return error_code(error, EINVAL, "Cannot unpack OTAP protobuf batch");
    }
    batch = calloc(1, sizeof(*batch));
    if (batch == NULL) {
        BATCH(free_unpacked)(message, NULL);
        return error_code(error, ENOMEM, "Cannot allocate OTAP batch");
    }
    batch->private_data = message;
    if (message->n_arrow_payloads == 0 || message->n_arrow_payloads >= TABLE_SLOTS) {
        result = error_code(error, EINVAL, "Invalid OTAP table count");
        goto fail;
    }
    for (index = 0; index < message->n_arrow_payloads; index++) {
        payload = message->arrow_payloads[index];
        result = register_table(reader->signal, payload->type, payload->schema_id, seen, error);
        if (result != 0) {
            goto fail;
        }
    }
    if (!root_present(reader->signal, seen)) {
        result = error_code(error, EINVAL, "OTAP batch is missing its root table");
        goto fail;
    }
    batch->tables = calloc(message->n_arrow_payloads, sizeof(*batch->tables));
    if (batch->tables == NULL) {
        result = error_code(error, ENOMEM, "Cannot allocate OTAP tables");
        goto fail;
    }
    batch->id = message->batch_id;
    batch->headers.data.as_uint8 = message->headers.data;
    batch->headers.size_bytes = message->headers.len;
    batch->n_tables = message->n_arrow_payloads;
    remaining = reader->options.max_batch_bytes;
    for (index = 0; index < batch->n_tables; index++) {
        result = read_payload(reader, message->arrow_payloads[index],
                              &remaining, borrow_views, &batch->tables[index], error);
        if (result != 0) {
            goto fail;
        }
    }
    *out = batch;
    return 0;

fail:
    flb_otap_batch_destroy(batch);
    return result;
}

static int read_batch(struct flb_otap_reader *reader, struct ArrowBufferView wire,
                      int borrow_views, struct flb_otap_batch **out, struct ArrowError *error)
{
    struct ArrowError local_error;
    int result;

    if (error == NULL) {
        error = &local_error;
    }
    error->message[0] = '\0';
    if (out == NULL) {
        return error_code(error, EINVAL, "OTAP reader requires a batch destination");
    }
    *out = NULL;
    if (reader == NULL || reader->failed) {
        return error_code(error, EINVAL, "OTAP reader requires a new stream");
    }
    result = decode_batch(reader, wire, borrow_views, out, error);
    if (result != 0) {
        reader->failed = 1;
        if (error->message[0] == '\0') {
            ArrowErrorSet(error, "Cannot decode OTAP batch (%d)", result);
        }
    }
    return result;
}

int flb_otap_read(struct flb_otap_reader *reader, struct ArrowBufferView wire,
                  struct flb_otap_batch **out, struct ArrowError *error)
{
    return read_batch(reader, wire, 0, out, error);
}

int flb_otap_read_views(struct flb_otap_reader *reader, struct ArrowBufferView wire,
                        struct flb_otap_batch **out, struct ArrowError *error)
{
    return read_batch(reader, wire, 1, out, error);
}

static void write_table_reset(struct otap_write_table *table)
{
    free(table->schema_id);
    if (table->schema.allocator.free != NULL) {
        ArrowBufferReset(&table->schema);
    }
    memset(table, 0, sizeof(*table));
}

struct flb_otap_writer *flb_otap_writer_create(enum flb_otap_signal signal,
                                             enum ArrowIpcCompressionType compression)
{
    struct flb_otap_writer *writer;

    if (!signal_valid(signal) || compression < NANOARROW_IPC_COMPRESSION_TYPE_NONE ||
        compression > NANOARROW_IPC_COMPRESSION_TYPE_ZSTD) {
        return NULL;
    }
    writer = calloc(1, sizeof(*writer));
    if (writer != NULL) {
        writer->signal = signal;
        writer->compression = compression;
    }
    return writer;
}

void flb_otap_writer_destroy(struct flb_otap_writer *writer)
{
    size_t index;

    if (writer != NULL) {
        for (index = 0; index < TABLE_SLOTS; index++) {
            write_table_reset(&writer->tables[index]);
        }
        free(writer);
    }
}

static int write_ipc(const struct flb_otap_writer *writer, const struct flb_otap_table_view *view,
                     struct ArrowBuffer *buffer, int64_t *schema_size, struct ArrowError *error)
{
    int result;
    struct ArrowIpcWriter ipc_writer;
    struct ArrowIpcOutputStream output;
    struct ArrowArrayView array_view;

    ArrowArrayViewInitFromType(&array_view, NANOARROW_TYPE_UNINITIALIZED);
    result = ArrowArrayViewInitFromSchema(&array_view, view->schema, error);
    if (result == 0) {
        result = ArrowArrayViewSetArray(&array_view, view->array, error);
    }
    if (result == 0) {
        result = ArrowArrayViewValidate(&array_view, NANOARROW_VALIDATION_LEVEL_FULL, error);
    }
    if (result != 0) {
        goto done;
    }
    if (array_view.storage_type != NANOARROW_TYPE_STRUCT || view->array->offset != 0 ||
        ArrowArrayViewComputeNullCount(&array_view) != 0) {
        result = error_code(error, EINVAL, "An OTAP table must be a non-null Arrow record batch");
        goto done;
    }
    result = ArrowIpcOutputStreamInitBuffer(&output, buffer);
    if (result != 0) {
        goto done;
    }
    result = ArrowIpcWriterInit(&ipc_writer, &output);
    if (result != 0) {
        output.release(&output);
        goto done;
    }
    result = ArrowIpcWriterSetCompression(&ipc_writer, writer->compression,
                                          NANOARROW_IPC_COMPRESSION_LEVEL_DEFAULT, error);
    if (result == 0) {
        result = ArrowIpcWriterWriteSchema(&ipc_writer, view->schema, error);
        *schema_size = buffer->size_bytes;
    }
    if (result == 0) {
        result = ArrowIpcWriterWriteArrayViewWithDictionaries(&ipc_writer, &array_view, error);
    }
    ArrowIpcWriterReset(&ipc_writer);

done:
    ArrowArrayViewReset(&array_view);
    return result;
}

static int write_payload(const struct flb_otap_writer *writer,
                         const struct flb_otap_table_view *view, struct otap_write_table *next,
                         struct ArrowBuffer *buffer, payload_message *payload,
                         struct ArrowError *error)
{
    const struct otap_write_table *current;
    int64_t schema_size;
    int64_t skip;
    int result;

    result = write_ipc(writer, view, buffer, &schema_size, error);
    if (result != 0) {
        return result;
    }
    current = &writer->tables[view->type];
    skip = 0;
    if (current->schema_id != NULL && strcmp(current->schema_id, view->schema_id) == 0) {
        if (current->schema.size_bytes != schema_size ||
            memcmp(current->schema.data, buffer->data, (size_t) schema_size) != 0) {
            return error_code(error, EINVAL, "Changed Arrow IPC schema requires a new schema ID");
        }
        skip = schema_size;
    }
    else {
        next->schema_id = copy_string(view->schema_id);
        if (next->schema_id == NULL) {
            return error_code(error, ENOMEM, "Cannot allocate OTAP schema ID");
        }
        ArrowBufferInit(&next->schema);
        result = ArrowBufferAppend(&next->schema, buffer->data, schema_size);
        if (result != 0) {
            return result;
        }
    }
    opentelemetry__proto__experimental__arrow__v1__arrow_payload__init(payload);
    payload->type = view->type;
    payload->schema_id = (char *) view->schema_id;
    payload->record.data = buffer->data + skip;
    payload->record.len = (size_t) (buffer->size_bytes - skip);
    return 0;
}

int flb_otap_write(struct flb_otap_writer *writer, int64_t id, struct ArrowBufferView headers,
                   const struct flb_otap_table_view *tables, size_t n_tables,
                   struct ArrowBuffer *out, struct ArrowError *error)
{
    struct ArrowError local_error;
    batch_message message;
    unsigned char seen[TABLE_SLOTS] = {0};
    struct otap_write_table next[TABLE_SLOTS] = {0};
    struct ArrowBuffer buffers[TABLE_SLOTS];
    payload_message payloads[TABLE_SLOTS];
    payload_message *pointers[TABLE_SLOTS];
    size_t index;
    size_t initialized;
    size_t size;
    int result;

    if (error == NULL) {
        error = &local_error;
    }
    error->message[0] = '\0';
    if (out == NULL) {
        return error_code(error, EINVAL, "OTAP writer requires a buffer destination");
    }
    ArrowBufferInit(out);
    if (writer == NULL || tables == NULL || n_tables == 0 || n_tables >= TABLE_SLOTS ||
        headers.size_bytes < 0 || (uint64_t) headers.size_bytes > SIZE_MAX ||
        (headers.size_bytes > 0 && headers.data.data == NULL)) {
        return error_code(error, EINVAL, "Invalid OTAP batch arguments");
    }

    for (index = 0; index < n_tables; index++) {
        result = register_table(writer->signal, tables[index].type, tables[index].schema_id,
                                seen, error);
        if (result != 0) {
            return result;
        }
        if (tables[index].schema == NULL || tables[index].schema->release == NULL ||
            tables[index].array == NULL || tables[index].array->release == NULL) {
            return error_code(error, EINVAL, "OTAP table requires an Arrow schema and array");
        }
    }
    if (!root_present(writer->signal, seen)) {
        return error_code(error, EINVAL, "OTAP batch is missing its root table");
    }

    initialized = 0;
    for (index = 0; index < n_tables; index++) {
        ArrowBufferInit(&buffers[index]);
        initialized++;
        result = write_payload(writer, &tables[index], &next[index], &buffers[index],
                               &payloads[index], error);
        if (result != 0) {
            goto done;
        }
        pointers[index] = &payloads[index];
    }

    BATCH(init)(&message);
    message.batch_id = id;
    message.headers.data = (uint8_t *) headers.data.as_uint8;
    message.headers.len = (size_t) headers.size_bytes;
    message.n_arrow_payloads = n_tables;
    message.arrow_payloads = pointers;
    size = BATCH(get_packed_size)(&message);
    if (size > INT64_MAX) {
        result = error_code(error, EMSGSIZE, "Encoded OTAP batch exceeds buffer limit");
        goto done;
    }
    result = ArrowBufferResize(out, (int64_t) size, 0);
    if (result != 0) {
        goto done;
    }
    out->size_bytes = BATCH(pack)(&message, out->data);

    for (index = 0; index < n_tables; index++) {
        if (next[index].schema_id != NULL) {
            write_table_reset(&writer->tables[tables[index].type]);
            writer->tables[tables[index].type] = next[index];
            memset(&next[index], 0, sizeof(next[index]));
        }
    }

done:
    for (index = 0; index < initialized; index++) {
        ArrowBufferReset(&buffers[index]);
        write_table_reset(&next[index]);
    }
    if (result != 0) {
        ArrowBufferReset(out);
        if (error->message[0] == '\0') {
            ArrowErrorSet(error, "Cannot encode OTAP batch (%d)", result);
        }
    }
    return result;
}
