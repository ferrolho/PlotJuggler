#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""Generate the tiny indexless ROS2 Float64 MCAP browser fixture.

The fixture is synthetic project test data covered by the repository license.
Reproduce it with:

    python3 -m pip install mcap==1.3.1
    python3 tests/wasm/fixtures/generate_unchunked_float64.py

The writer settings are load-bearing: no chunks and no indexes force the
official MCAP plugin through its serial offset-locator fallback. The script
verifies that structure before replacing the checked-in base64 file.
"""

import base64
import hashlib
import io
import struct
import textwrap
from importlib.metadata import version
from pathlib import Path

from mcap.reader import make_reader
from mcap.records import AttachmentIndex, Chunk, ChunkIndex, Message, MessageIndex, MetadataIndex
from mcap.stream_reader import StreamReader
from mcap.writer import CompressionType, IndexType, Writer


OUTPUT = Path(__file__).with_name("unchunked_float64.mcap.b64")
VALUES = tuple(float(index * index - 3 * index) for index in range(12))
STEP_NS = 250_000_000


def cdr_float64(value: float) -> bytes:
    """Encode one ROS2 std_msgs/msg/Float64 payload as little-endian CDR."""
    return b"\x00\x01\x00\x00" + struct.pack("<d", value)


def build_fixture() -> bytes:
    output = io.BytesIO()
    writer = Writer(
        output,
        compression=CompressionType.NONE,
        index_types=IndexType.NONE,
        repeat_channels=True,
        repeat_schemas=True,
        use_chunking=False,
        use_statistics=True,
        use_summary_offsets=True,
        enable_crcs=True,
        enable_data_crcs=False,
    )
    writer.start(profile="ros2", library="pj-w8h-generator/1")
    schema_id = writer.register_schema(
        name="std_msgs/msg/Float64",
        encoding="ros2msg",
        data=b"float64 data\n",
    )
    channel_id = writer.register_channel(
        topic="/unchunked/seek",
        message_encoding="cdr",
        schema_id=schema_id,
    )
    for sequence, value in enumerate(VALUES):
        timestamp = sequence * STEP_NS
        writer.add_message(
            channel_id=channel_id,
            log_time=timestamp,
            publish_time=timestamp,
            sequence=sequence,
            data=cdr_float64(value),
        )
    writer.finish()
    return output.getvalue()


def verify_fixture(payload: bytes) -> None:
    records = list(StreamReader(io.BytesIO(payload), emit_chunks=True, validate_crcs=True).records)
    forbidden = (Chunk, ChunkIndex, MessageIndex, AttachmentIndex, MetadataIndex)
    unexpected = [type(record).__name__ for record in records if isinstance(record, forbidden)]
    if unexpected:
        raise RuntimeError(f"fixture unexpectedly contains chunk/index records: {unexpected}")
    if sum(isinstance(record, Message) for record in records) != len(VALUES):
        raise RuntimeError("fixture does not contain the expected bare Message records")

    reader = make_reader(io.BytesIO(payload), validate_crcs=True)
    summary = reader.get_summary()
    if summary is None or summary.chunk_indexes:
        raise RuntimeError("fixture unexpectedly contains chunk indexes")
    messages = list(reader.iter_messages(log_time_order=False))
    if len(messages) != len(VALUES):
        raise RuntimeError(f"expected {len(VALUES)} messages, found {len(messages)}")
    if any(message.log_time != index * STEP_NS for index, (_, _, message) in enumerate(messages)):
        raise RuntimeError("fixture timestamps do not match the declared sequence")


def main() -> None:
    if version("mcap") != "1.3.1":
        raise RuntimeError(f"mcap==1.3.1 is required, found {version('mcap')}")
    payload = build_fixture()
    verify_fixture(payload)
    encoded = base64.b64encode(payload).decode("ascii")
    OUTPUT.write_text("\n".join(textwrap.wrap(encoded, width=76)) + "\n", encoding="ascii")
    digest = hashlib.sha256(payload).hexdigest()
    print(f"wrote {OUTPUT} ({len(payload)} bytes, sha256={digest})")


if __name__ == "__main__":
    main()
