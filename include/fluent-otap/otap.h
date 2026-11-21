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

#ifndef FLUENT_OTAP_H
#define FLUENT_OTAP_H

#include <nanoarrow/nanoarrow_ipc.h>
#include <arrow_service.pb-c.h>

#ifdef __cplusplus
extern "C" {
#endif

enum flb_otap_signal {
    FLB_OTAP_LOGS,
    FLB_OTAP_METRICS,
    FLB_OTAP_TRACES
};

typedef Opentelemetry__Proto__Experimental__Arrow__V1__ArrowPayloadType flb_otap_payload_type;

struct flb_otap_reader;
struct flb_otap_writer;

struct flb_otap_read_options {
    int64_t max_wire_bytes;
    int64_t max_batch_bytes;
    int64_t max_dictionary_bytes;
};

/* A table has either owned Arrow data or borrowed schema and array views. */
struct flb_otap_table {
    flb_otap_payload_type type;
    const char *schema_id;
    struct ArrowSchema schema;
    struct ArrowArray array;
    const struct ArrowSchema *view_schema;
    const struct ArrowArrayView *view;
};

/* Input views are borrowed for the duration of the write. */
struct flb_otap_table_view {
    flb_otap_payload_type type;
    const char *schema_id;
    const struct ArrowSchema *schema;
    const struct ArrowArray *array;
};

struct flb_otap_batch {
    int64_t id;
    struct ArrowBufferView headers;
    size_t n_tables;
    struct flb_otap_table *tables;
    void *private_data;
};

/* Each object represents one signal on one RPC. Use one thread at a time. */
void flb_otap_read_options_init(struct flb_otap_read_options *options);
struct flb_otap_reader *flb_otap_reader_create(enum flb_otap_signal signal,
                                             const struct flb_otap_read_options *options);
void flb_otap_reader_destroy(struct flb_otap_reader *reader);

/*
 * Decode a serialized BatchArrowRecords protobuf. Tables own their schemas
 * and arrays, which remain valid after reader destruction. On failure *out is NULL.
 * After a decode failure, destroy the reader and discard the RPC's IPC state.
 */
int flb_otap_read(struct flb_otap_reader *reader, struct ArrowBufferView wire,
                  struct flb_otap_batch **out, struct ArrowError *error);

/*
 * Decode for synchronous consumption. Table views borrow IPC buffers from the
 * batch and decoder state from the reader. Use them before destroying the batch,
 * reading again, or destroying the reader. Release the batch normally.
 */
int flb_otap_read_views(struct flb_otap_reader *reader, struct ArrowBufferView wire,
                        struct flb_otap_batch **out, struct ArrowError *error);
void flb_otap_batch_destroy(struct flb_otap_batch *batch);

struct flb_otap_writer *flb_otap_writer_create(enum flb_otap_signal signal,
                                             enum ArrowIpcCompressionType compression);
void flb_otap_writer_destroy(struct flb_otap_writer *writer);

/*
 * Encode one BatchArrowRecords protobuf. The caller supplies unique batch IDs
 * and stable schema IDs (distinct between tables for Go and Rust interoperability).
 * On success, the caller owns out and must call ArrowBufferReset(). On error,
 * out is empty and writer state is unchanged.
 */
int flb_otap_write(struct flb_otap_writer *writer, int64_t id, struct ArrowBufferView headers,
                   const struct flb_otap_table_view *tables, size_t n_tables,
                   struct ArrowBuffer *out, struct ArrowError *error);

#ifdef __cplusplus
}
#endif
#endif
