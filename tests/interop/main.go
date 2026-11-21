// SPDX-License-Identifier: Apache-2.0
//
// Generate fixtures with the upstream Go codec or verify C-encoded batches.
package main

import (
	"fmt"
	"os"
	"path/filepath"
	"strings"

	arrowpb "github.com/open-telemetry/otel-arrow/go/api/experimental/arrow/v1"
	"github.com/open-telemetry/otel-arrow/go/pkg/config"
	"github.com/open-telemetry/otel-arrow/go/pkg/otel/arrow_record"
	"go.opentelemetry.io/collector/pdata/pcommon"
	"go.opentelemetry.io/collector/pdata/plog"
	"go.opentelemetry.io/collector/pdata/pmetric"
	"go.opentelemetry.io/collector/pdata/ptrace"
	"google.golang.org/protobuf/proto"
)

func must(err error) {
	if err != nil {
		panic(err)
	}
}

func path(dir, signal string, index int) string {
	return filepath.Join(dir, fmt.Sprintf("%s-%d.pb", signal, index))
}

func save(dir, signal string, index int, batch *arrowpb.BatchArrowRecords, err error) {
	must(err)
	wire, err := proto.Marshal(batch)
	must(err)
	must(os.WriteFile(path(dir, signal, index), wire, 0644))
}

func msg(i int) string {
	return fmt.Sprintf("body-%d-%s", i, strings.Repeat("sample", 100))
}

func populate(dir string) {
	logsProducer := arrow_record.NewProducerWithOptions(config.WithZstd())
	defer logsProducer.Close()
	for i := 0; i < 3; i++ {
		logs := plog.NewLogs()
		res := logs.ResourceLogs().AppendEmpty()
		res.Resource().Attributes().PutStr("service.name", "codec")
		res.SetSchemaUrl("https://example.test/resource")
		scope := res.ScopeLogs().AppendEmpty()
		scope.Scope().SetName("otap-test")
		scope.Scope().Attributes().PutInt("scope.attr", 9)
		for j := 0; j < 16; j++ {
			rec := scope.LogRecords().AppendEmpty()
			rec.SetTimestamp(pcommon.Timestamp(1_700_000_000_000_000_000 + j))
			rec.SetSeverityText(fmt.Sprintf("notice-%d", i))
			if i == 2 {
				rec.Body().SetInt(42)
			} else {
				rec.Body().SetStr(msg(i))
			}
			rec.Attributes().PutBool("enabled", true)
			rec.Attributes().PutEmptySlice("list").AppendEmpty().SetStr("sample")
		}
		batch, err := logsProducer.BatchArrowRecordsFromLogs(logs)
		save(dir, "logs", i, batch, err)
	}

	metricsProducer := arrow_record.NewProducerWithOptions(config.WithZstd())
	defer metricsProducer.Close()
	metrics := pmetric.NewMetrics()
	metric := metrics.ResourceMetrics().AppendEmpty().ScopeMetrics().AppendEmpty().Metrics().AppendEmpty()
	metric.SetName("fluentbit.requests")
	point := metric.SetEmptyGauge().DataPoints().AppendEmpty()
	point.SetTimestamp(pcommon.Timestamp(1_700_000_000_000_000_000))
	point.SetIntValue(17)
	point.Attributes().PutStr("method", "GET")
	batch, err := metricsProducer.BatchArrowRecordsFromMetrics(metrics)
	save(dir, "metrics", 0, batch, err)

	tracesProducer := arrow_record.NewProducerWithOptions(config.WithZstd())
	defer tracesProducer.Close()
	traces := ptrace.NewTraces()
	span := traces.ResourceSpans().AppendEmpty().ScopeSpans().AppendEmpty().Spans().AppendEmpty()
	span.SetName("operation")
	span.SetTraceID(pcommon.TraceID{1})
	span.SetSpanID(pcommon.SpanID{2})
	span.SetStartTimestamp(pcommon.Timestamp(1_700_000_000_000_000_000))
	span.SetEndTimestamp(pcommon.Timestamp(1_700_000_000_000_000_100))
	span.Events().AppendEmpty().SetName("event")
	batch, err = tracesProducer.BatchArrowRecordsFromTraces(traces)
	save(dir, "traces", 0, batch, err)
}

func load(dir, signal string, index int) *arrowpb.BatchArrowRecords {
	wire, err := os.ReadFile(path(dir, signal, index))
	must(err)
	var batch arrowpb.BatchArrowRecords
	must(proto.Unmarshal(wire, &batch))
	return &batch
}

func verify(dir string) {
	logsConsumer := arrow_record.NewConsumer()
	defer logsConsumer.Close()
	for i := 0; i < 3; i++ {
		batches, err := logsConsumer.LogsFrom(load(dir, "logs", i))
		must(err)
		if len(batches) != 1 || batches[0].LogRecordCount() != 16 {
			panic("unexpected log count")
		}
		res := batches[0].ResourceLogs().At(0)
		scope := res.ScopeLogs().At(0)
		svc, _ := res.Resource().Attributes().Get("service.name")
		for j := 0; j < scope.LogRecords().Len(); j++ {
			rec := scope.LogRecords().At(j)
			list, _ := rec.Attributes().Get("list")
			if svc.Str() != "codec" || scope.Scope().Name() != "otap-test" ||
				list.Slice().At(0).Str() != "sample" || rec.SeverityText() != fmt.Sprintf("notice-%d", i) ||
				(i < 2 && rec.Body().Str() != msg(i)) || (i == 2 && rec.Body().Int() != 42) {
				panic("unexpected log content")
			}
		}
	}

	metricsConsumer := arrow_record.NewConsumer()
	defer metricsConsumer.Close()
	metrics, err := metricsConsumer.MetricsFrom(load(dir, "metrics", 0))
	must(err)
	metric := metrics[0].ResourceMetrics().At(0).ScopeMetrics().At(0).Metrics().At(0)
	if metric.Name() != "fluentbit.requests" || metric.Gauge().DataPoints().At(0).IntValue() != 17 {
		panic("unexpected metric")
	}

	tracesConsumer := arrow_record.NewConsumer()
	defer tracesConsumer.Close()
	traces, err := tracesConsumer.TracesFrom(load(dir, "traces", 0))
	must(err)
	span := traces[0].ResourceSpans().At(0).ScopeSpans().At(0).Spans().At(0)
	if span.Name() != "operation" || span.SpanID() != (pcommon.SpanID{2}) ||
		span.Events().At(0).Name() != "event" {
		panic("unexpected span")
	}
}

func main() {
	if len(os.Args) != 3 {
		panic("usage: interop generate|verify DIRECTORY")
	}
	switch os.Args[1] {
	case "generate":
		populate(os.Args[2])
	case "verify":
		verify(os.Args[2])
	default:
		panic("expected generate or verify")
	}
}
