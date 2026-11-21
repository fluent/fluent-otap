#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0

"""Regenerate or check the C bindings for the pinned OTAP service schema."""

import argparse
import subprocess
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SCHEMA = ROOT / "proto"
BINDINGS = ROOT / "proto_c"
PREFIX = "opentelemetry__proto__experimental__arrow__v1__"

C_HEADER = """/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

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

"""


def adapt_initializers(path):
    code = path.read_text(encoding="utf-8")
    wrappers = []
    # protobuf-c calls each descriptor's initializer with ProtobufCMessage *.
    for name, c_type in (
        ("batch_arrow_records", "BatchArrowRecords"),
        ("arrow_payload", "ArrowPayload"),
        ("batch_status", "BatchStatus"),
    ):
        typed_init = f"{PREFIX}{name}__init"
        generic_init = f"{name}_message_init"
        wrappers.append(
            f"static void {generic_init}(ProtobufCMessage *message)\n"
            "{\n"
            f"  {typed_init}((Opentelemetry__Proto__Experimental__Arrow__V1__{c_type} *) message);\n"
            "}\n"
        )
        marker = f"(ProtobufCMessageInit) {typed_init},"
        if code.count(marker) != 1:
            raise ValueError(f"Unexpected initializer in generated code for {typed_init}")
        code = code.replace(marker, f"{generic_init},")
    include = '#include "arrow_service.pb-c.h"\n'
    code = code.replace(include, include + "\n" + "\n".join(wrappers) + "\n", 1)
    path.write_bytes(code.encode("utf-8"))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--protoc-c", default="protoc-c", metavar="PATH")
    parser.add_argument("--check", action="store_true", help="check the checked-in files")
    args = parser.parse_args()
    compiler = args.protoc_c
    compiler_path = Path(compiler)
    if compiler_path.parent != Path(".") or compiler.startswith(("./", ".\\")):
        compiler = str(compiler_path.absolute())

    with tempfile.TemporaryDirectory() as directory:
        generated = Path(directory)
        subprocess.run(
            [compiler, f"--c_out={generated}", "arrow_service.proto"], cwd=SCHEMA, check=True
        )
        adapt_initializers(generated / "arrow_service.pb-c.c")
        stale = []
        for name in ("arrow_service.pb-c.c", "arrow_service.pb-c.h"):
            content = C_HEADER.encode("utf-8") + (generated / name).read_bytes()
            destination = BINDINGS / name
            if args.check:
                if not destination.exists() or destination.read_bytes() != content:
                    stale.append(name)
            else:
                destination.write_bytes(content)
        if stale:
            parser.exit(1, f"Generated bindings differ: {', '.join(stale)}\n")


if __name__ == "__main__":
    main()
